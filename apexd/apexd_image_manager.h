/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include <libfiemap/image_manager.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "apex_file.h"

namespace android::apex {

class ApexImageManager {
 public:
  ~ApexImageManager() = default;

  // Pin APEX files in /data/apex/images and save their metadata(e.g. FIEMAP
  // extents) in /metadata/apex/images so that they are available before /data
  // partition is mounted.
  // Returns names which correspond to pinned APEX files.
  base::Result<std::vector<std::string>> PinApexFiles(
      std::span<const ApexFile> apex_files);
  base::Result<void> DeleteImage(const std::string& image);
  std::vector<std::string> GetAllImages();

  static std::unique_ptr<ApexImageManager> Create(
      const std::string& metadata_images_dir,
      const std::string& data_images_dir);

 private:
  ApexImageManager(const std::string& metadata_dir,
                   const std::string& data_dir);

  std::string metadata_dir_;
  std::string data_dir_;
  std::unique_ptr<fiemap::IImageManager> fsmgr_;
};

void InitializeImageManager(ApexImageManager* image_manager);
ApexImageManager* GetImageManager();

}  // namespace android::apex