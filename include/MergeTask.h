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
#include <Backup.h>
#include <BackupLayer.h>

void update_files_dirs(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, layer_id_t idx);

void prune_files_dirs(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, layer_id_t idx);

void prune_chunks(std::vector<std::shared_ptr<ArchiveBackupLayer>>& layers, index_t idx);

void cat_prune_pool(const std::shared_ptr<ArchiveBackupLayer>& layer,
                    const std::vector<std::shared_ptr<ArchiveBackupLayer>>& to_merge_layers);
