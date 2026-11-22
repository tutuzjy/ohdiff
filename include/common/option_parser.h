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

#pragma once

#include <common/defs.h>
#include <boost/program_options.hpp>
#include <Backup.h>
#include <chrono>
#include <memory>

static layer_id_t parse_layer_number(const boost::program_options::variables_map& vm, const Backup& backup)
{
    if (vm.count("timepoint") && vm.count("layers")) {
        throw std::runtime_error("Only one of --timepoint or --layers can be specified");
    }
    if (vm.count("timepoint")) {
        std::string timepoint = vm["timepoint"].as<std::string>();
        std::chrono::system_clock::time_point tp;
        try {
            tp = parse_timepoint(timepoint);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse timepoint: " + std::string(e.what()));
        }
        return backup.num_layers_at_time(tp);
    } else if (vm.count("layers")) {
        return vm["layers"].as<layer_id_t>();
    } else {
        return backup.num_layers();
    }
}

static std::unique_ptr<tbb::global_control> parse_threads(const boost::program_options::variables_map& vm)
{
    if (vm.count("threads")) {
        int threads = vm["threads"].as<int>();
        constexpr int MINIMUM_THREADS = 2;
        if (threads < MINIMUM_THREADS) { // oneTBB 在单线程时会有问题
            std::cerr << "Warning: Number of threads should be at least 2" << std::endl;
            threads = MINIMUM_THREADS;
        }
        return std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, threads);
    }
    return nullptr;
}

static int parse_compress_level(const boost::program_options::variables_map& vm)
{
    std::string comp = vm["compression"].as<std::string>();
    std::transform(comp.begin(), comp.end(), comp.begin(), ::tolower);
    if (comp == "none" || comp == "no" || comp == "false" || comp == "off") {
        return NO_COMPRESSION_LEVEL;
    } else {
        try {
            return std::stoi(comp);
        } catch (const std::exception& e) {
            throw std::runtime_error("Invalid compression level: " + std::string(e.what()));
        }
    }
}
