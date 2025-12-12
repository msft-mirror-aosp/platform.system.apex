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

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "apex_file.h"
#include "apexd_dm.h"
#include "interval.h"

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

struct ApexImageInfo {
  std::vector<Interval> extents;
  uint64_t mtime;
};

// Returns an updated list. A new entry replaces any existing entries with the
// same apex name.
std::vector<ApexListEntry> UpdateApexListWithNewEntries(
    std::vector<ApexListEntry> list,
    const std::vector<ApexListEntry>& new_entries);

base::Result<DmDevice> CreateDmLinear(const std::string& name,
                                      const std::string& block_dev,
                                      const std::vector<Interval>& extents,
                                      bool read_only);

class ApexImageManager {
 public:
  virtual ~ApexImageManager() = default;

  // Pin APEX files in /data/apex/images and save their metadata(e.g. FIEMAP
  // extents) in /metadata/apex/images so that they are available before /data
  // partition is mounted.
  // Returns names which correspond to pinned APEX files.
  virtual base::Result<std::vector<std::string>> PinApexFiles(
      std::span<const ApexFile> apex_files);
  base::Result<void> DeleteImage(const std::string& image);
  base::Result<void> UnmapAndDeleteImage(const std::string& image);
  std::vector<std::string> GetAllImages() const;
  base::Result<void> RemoveUnreferencedImages() const;
  void ClearDeletedImageNames() const;

  // True if the apex is backed by a dm-linear device created by
  // ApexImageManager
  bool IsPinnedApex(const ApexFile& file) const {
    return FindPinnedApex(file).has_value();
  }

  // Returns the image name if the apex is backed by a dm-linear device created
  // by ApexImageManager
  std::optional<std::string> FindPinnedApex(const ApexFile& file) const;

  // Returns the path of the block device if mapped. Similar to MapImage(), but
  // this doesn't create a block device if not mapped already.
  std::optional<std::string> GetMappedPath(const std::string& image) const;

  // Creates a dm-linear block device for a pinned apex and returns the path of
  // the created block device.
  virtual base::Result<std::string> MapImage(const std::string& image);
  base::Result<void> UnmapImage(const std::string& image);
  base::Result<void> UnmapImageIfExists(const std::string& image);
  base::Result<std::vector<Interval>> GetImageExtents(const std::string& image);

  // Creates a backup of the current ACTIVE apex list
  base::Result<void> BackupApexList();
  // Restores the ACTIVE apex list from the last backup
  base::Result<void> RestoreApexList();

  base::Result<void> UpdateApexList(ApexListType list_type,
                                    const std::vector<ApexListEntry>& entries);
  base::Result<std::vector<ApexListEntry>> GetApexList(
      ApexListType list_type) const;

  static std::unique_ptr<ApexImageManager> Create(
      const std::string& metadata_images_dir,
      const std::string& data_images_dir);

 protected:
  ApexImageManager(const std::string& metadata_dir,
                   const std::string& data_dir);

  base::Result<ApexImageInfo> GetApexImageInfo(const std::string& image);

  std::string GetApexListFile(ApexListType list_type) const;
  std::string GetApexStorageMetadataPath() const;

  std::string metadata_dir_;
  std::string data_dir_;
};

void InitializeImageManager(ApexImageManager* image_manager);
ApexImageManager* GetImageManager();

}  // namespace android::apex