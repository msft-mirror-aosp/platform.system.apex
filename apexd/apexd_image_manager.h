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

// ApexImageManager manages two lists of APEX files (or image names).
// - ACTIVE: the list of "active" apexes. Candidates for activation.
// - BACKUP: a copy of the last ACTIVE list that was successful.
//
// The lists are stored in /metadata/apex/images directory.
enum ApexListType {
  ACTIVE,
  BACKUP,
};

struct ApexListEntry {
  std::string image_name;
  std::string apex_name;

  inline auto operator<=>(const ApexListEntry&) const = default;
};

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

  // True if the apex is backed by a dm-linear device created by
  // ApexImageManager
  bool IsPinnedApex(const ApexFile& file) const;

  // Creates a dm-linear block device for a pinned apex and returns the path of
  // the created block device.
  base::Result<std::string> MapImage(const std::string& image);
  base::Result<void> UnmapImage(const std::string& image);

  base::Result<void> UpdateApexList(ApexListType list_type,
                                    const std::vector<ApexListEntry>& entries);
  base::Result<std::vector<ApexListEntry>> GetApexList(ApexListType list_type);

  static std::unique_ptr<ApexImageManager> Create(
      const std::string& metadata_images_dir,
      const std::string& data_images_dir);

 private:
  ApexImageManager(const std::string& metadata_dir,
                   const std::string& data_dir);

  std::string GetApexListFile(ApexListType list_type) const;

  std::string metadata_dir_;
  std::string data_dir_;
  std::unique_ptr<fiemap::IImageManager> fsmgr_;
};

void InitializeImageManager(ApexImageManager* image_manager);
ApexImageManager* GetImageManager();

}  // namespace android::apex