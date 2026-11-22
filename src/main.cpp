/*
 * Copyright (c) 2026 ohdiff contributors.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <string_view>
#include <utility>
#include <Backup.h>
#include <BackupTask.h>
#include <boost/program_options.hpp>
#include <chrono>
#include <common/defs.h>
#include <common/option_parser.h>
#include <common/utils.h>
#include <fmt/chrono.h>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace po = boost::program_options;

namespace {
constexpr int MINIMUM_ARGUMENT_COUNT = 2;
constexpr int SPEED_PRECISION = 2;
constexpr int MILLISECONDS_PER_SECOND = 1000;
constexpr auto PROGRESS_INTERVAL = std::chrono::milliseconds(100);
std::vector<std::function<void()>> g_cleanupFuncs;

void Cleanup()
{
    for (const auto& function : g_cleanupFuncs) {
        function();
    }
}

std::shared_ptr<PeriodicProgressHandler> ProgressHandler(bool enabled)
{
    if (!enabled) {
        return nullptr;
    }
    return std::make_shared<PeriodicProgressHandler>(
        [](const PeriodicProgressHandler& handler) {
            static size_t previous = 0;
            const size_t progress = handler.get_progress();
            const float speed =
                static_cast<float>(progress - previous) / handler.get_interval().count() * MILLISECONDS_PER_SECOND;
            std::cout << "Processed " << data_size(progress) << " " << data_size(previous) << '\t'
                      << data_size(speed, SPEED_PRECISION) << "/s\n";
            previous = progress;
        },
        PROGRESS_INTERVAL);
}

void ParseOptions(int argc, char* argv[], const po::options_description& description, po::variables_map& options)
{
    po::store(po::parse_command_line(argc - 1, argv + 1, description), options);
    po::notify(options);
}

void CommonOptions(po::options_description& description, bool* showProgress)
{
    description.add_options()("no-low-mem", po::bool_switch(&global_config.performance_mode)->default_value(false),
                              "Disable low memory mode")("threads,t", po::value<int>(), "Number of threads")(
        "progress", po::bool_switch(showProgress)->default_value(false), "Show progress");
}

void RunBackup(int argc, char* argv[], po::options_description& description, po::variables_map& options)
{
    std::string source;
    std::string destination;
    std::string logFile;
    bool showProgress = false;
    description.add_options()("src,s", po::value<std::string>(&source)->required(), "Source path")(
        "backup,b", po::value<std::string>(&destination)->required(), "Backup directory")(
        "log-file", po::value<std::string>(&logFile),
        "Operation log for incremental backup (defaults to /data/log/<inode>.log)")(
        "compression,c", po::value<std::string>()->default_value(std::to_string(DEFAULT_COMPRESSION_LEVEL)),
        "Compression level, or 'off' for no compression");
    CommonOptions(description, &showProgress);
    ParseOptions(argc, argv, description, options);
    [[maybe_unused]] const std::unique_ptr<tbb::global_control> threadLimit = parse_threads(options);
    const int compressionLevel = parse_compress_level(options);
    g_cleanupFuncs.push_back([destination]() { clean_temp_files(destination); });
    Backup backup(destination);
    backup.add_layer(source, compressionLevel, ProgressHandler(showProgress), logFile);
}

void RunRestore(int argc, char* argv[], po::options_description& description, po::variables_map& options)
{
    std::string source;
    std::string destination;
    std::string picked;
    std::string timepoint;
    layer_id_t layers = 0;
    bool showProgress = false;
    description.add_options()("backup,b", po::value<std::string>(&source)->required(), "Backup directory")(
        "dst,d", po::value<std::string>(&destination)->required(),
        "Restore destination")("layers,l", po::value<layer_id_t>(&layers), "Number of layers to restore")(
        "timepoint", po::value<std::string>(&timepoint), "Timepoint, for example 2024-06-29T18:08:00")(
        "pick,p", po::value<std::string>(&picked)->default_value("/"), "File or directory to restore");
    CommonOptions(description, &showProgress);
    ParseOptions(argc, argv, description, options);
    [[maybe_unused]] const std::unique_ptr<tbb::global_control> threadLimit = parse_threads(options);
    Backup backup(source);
    backup.restore(parse_layer_number(options, backup), destination, picked, ProgressHandler(showProgress));
}

void RunMerge(int argc, char* argv[], po::options_description& description, po::variables_map& options)
{
    std::string directory;
    layer_id_t start = 0;
    layer_id_t end = 0;
    description.add_options()("backup,b", po::value<std::string>(&directory)->required(), "Backup directory")(
        "start,s", po::value<layer_id_t>(&start)->required(), "Start layer index (inclusive)")(
        "end,e", po::value<layer_id_t>(&end)->required(), "End layer index (inclusive)")("threads,t", po::value<int>(),
                                                                                         "Number of threads");
    ParseOptions(argc, argv, description, options);
    if (start == 0 || end < start) {
        throw std::invalid_argument("Merge layers must satisfy 1 <= start <= end");
    }
    [[maybe_unused]] const std::unique_ptr<tbb::global_control> threadLimit = parse_threads(options);
    g_cleanupFuncs.push_back([directory]() { clean_temp_files(directory); });
    Backup backup(directory);
    backup.merge_layers(start - 1, end);
}

void RunList(int argc, char* argv[], po::options_description& description, po::variables_map& options)
{
    std::string directory;
    description.add_options()("backup,b", po::value<std::string>(&directory)->required(), "Backup directory");
    ParseOptions(argc, argv, description, options);
    Backup backup(directory);
    for (const auto& layer : backup.archive_layers) {
        std::cout << layer->id + 1 << '\t' << fmt::format("{:%F %T}", layer->time) << '\t'
                  << data_size(layer->pool_size) << '\t' << "+ "
                  << layer->file_index.size() << " files\t - " << layer->del_file_index.size() << " files\n";
    }
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < MINIMUM_ARGUMENT_COUNT) {
        std::cerr << "Usage: " << argv[0] << " <command> [options]\n";
        return 1;
    }
    po::options_description description("Allowed options");
    po::variables_map options;
    try {
        const std::string command = argv[1];
        using Command = void (*)(int, char*[], po::options_description&, po::variables_map&);
        const std::pair<std::string_view, Command> commands[] = {
            {"backup", RunBackup}, {"restore", RunRestore}, {"merge", RunMerge}, {"list", RunList}};
        const auto handler = std::find_if(std::begin(commands), std::end(commands),
                                          [&](const auto& entry) { return entry.first == command; });
        if (handler == std::end(commands)) {
            throw std::invalid_argument("Unknown command: " + command);
        }
        handler->second(argc, argv, description, options);
    } catch (const po::error& error) {
        std::cerr << "Error: " << error.what() << '\n' << description << '\n';
        Cleanup();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        Cleanup();
        return 1;
    }
    return 0;
}
