/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/result.h>

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "interval.h"

// Exposed for testing
namespace android::apex {

struct ImageCreator {
  virtual ~ImageCreator() {}
  virtual base::Result<std::vector<Interval>> CreateImage(
      const std::string& image_name, uint64_t size) = 0;
  virtual void MarkDone() {}
};

class FreeSpaceAllocator : public ImageCreator {
 public:
  FreeSpaceAllocator(std::vector<Interval>&& free_extents)
      : free_extents(IntervalComparatorByLength(), std::move(free_extents)) {}

  base::Result<std::vector<Interval>> CreateImage(const std::string& image_name,
                                                  uint64_t size) override;

  static base::Result<std::unique_ptr<FreeSpaceAllocator>> Create(
      const std::string& storage_path, uint64_t initial_size,
      const std::vector<Interval>& used_extents);

 private:
  // The longest extent/interval is the top element.
  std::priority_queue<Interval, std::vector<Interval>,
                      IntervalComparatorByLength>
      free_extents;
};

class ApexStoragePerImageCreator : public ImageCreator {
 public:
  ApexStoragePerImageCreator(const std::string& data_dir)
      : data_dir(data_dir) {}

  ~ApexStoragePerImageCreator();

  base::Result<std::vector<Interval>> CreateImage(const std::string& image_name,
                                                  uint64_t size) override;

  void MarkDone() override { intermediate_files.clear(); }

 private:
  std::string data_dir;
  // These files will be deleted on exit unless MarkDone() is called.
  std::vector<std::string> intermediate_files;
};

}  // namespace android::apex
