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

#include "apexd_image_manager.h"

#include <android-base/file.h>
#include <android-base/result.h>
#include <android-base/unique_fd.h>
#include <libdm/dm.h>
#include <sys/sendfile.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

#include "apex_image_list.pb.h"
#include "apexd.h"
#include "apexd_utils.h"

using android::base::borrowed_fd;
using android::base::ErrnoError;
using android::base::Error;
using android::base::RemoveFileIfExists;
using android::base::Result;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using namespace std::chrono_literals;

namespace android::apex {

namespace {

ApexImageManager* gImageManager;

Result<void> SendFile(borrowed_fd dest_fd, const std::string& src_path,
                      size_t size) {
  unique_fd src_fd(open(src_path.c_str(), O_RDONLY));
  if (!src_fd.ok()) {
    return Error() << "Failed to open " << src_path;
  }
  int rc = sendfile(dest_fd.get(), src_fd, nullptr, size);
  if (rc == -1) {
    return ErrnoError() << "Failed to sendfile from " << src_path;
  }
  return {};
}

// Find a unique "image" name for the apex name: e.g. com.android.foo_2.apex
std::string AllocateNewName(const std::vector<std::string>& known_names,
                            const std::string& apex_name) {
  // Note that because fsmgr's ImageManager uses the name as partition name,
  // the name can't be longer than 36. Let's limit the name up to 26 and reserve
  // the suffix (e.g "_0000.apex")
  auto base_name = apex_name.substr(0, 26);
  auto count = std::ranges::count_if(known_names, [&](const auto& name) {
    return name.starts_with(base_name);
  });
  // Find free slot for the "base_name"
  for (auto i = 0; i < count; i++) {
    std::string new_name =
        base_name + "_" + std::to_string(i) + kDmLinearApexSuffix;
    if (std::ranges::find(known_names, new_name) == known_names.end()) {
      return new_name;
    }
  }
  return base_name + "_" + std::to_string(count) + kDmLinearApexSuffix;
}

Result<void> WriteImageList(const std::vector<ApexListEntry>& list,
                            const std::string& filename) {
  unique_fd fd(
      open(filename.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC | O_TRUNC, 0660));
  if (fd < 0) {
    return ErrnoError() << "Failed to open " << filename;
  }

  // Serialize using proto
  using ::apex::proto::ApexImageList;

  ApexImageList pb_list;
  pb_list.mutable_entries()->Reserve(list.size());
  for (const auto& entry : list) {
    ApexImageList::Entry pb_entry;
    pb_entry.set_image_name(entry.image_name);
    pb_entry.set_apex_name(entry.apex_name);
    *pb_list.add_entries() = std::move(pb_entry);
  }
  if (!pb_list.SerializeToFileDescriptor(fd.get())) {
    return Error() << "Failed to save APEX image list to " << filename;
  }

  fsync(fd.get());
  return {};
}

Result<std::vector<ApexListEntry>> ReadImageList(const std::string& filename) {
  unique_fd fd(open(filename.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd < 0) {
    if (errno == ENOENT) {
      return {};
    }
    return ErrnoError() << "Failed to open " << filename;
  }

  std::vector<ApexListEntry> list;

  // Deserialize using proto
  using ::apex::proto::ApexImageList;

  ApexImageList pb_list;
  if (!pb_list.ParseFromFileDescriptor(fd.get())) {
    return Error() << "Failed to parse APEX image list from " << filename;
  }
  list.reserve(pb_list.entries_size());
  for (const auto& entry : pb_list.entries()) {
    list.emplace_back(entry.image_name(), entry.apex_name());
  }

  return list;
}

}  // namespace

std::vector<ApexListEntry> UpdateApexListWithNewEntries(
    std::vector<ApexListEntry> list,
    const std::vector<ApexListEntry>& new_entries) {
  // Collect updated apex names
  std::vector<std::string> updated_names;
  updated_names.reserve(new_entries.size());
  for (const auto& entry : new_entries) {
    updated_names.push_back(entry.apex_name);
  }
  // Remove updated apexes from existing list first.
  std::erase_if(list, [&](const auto& entry) {
    return std::ranges::contains(updated_names, entry.apex_name);
  });
  // Add new entries to the list
  list.append_range(new_entries);
  return list;
}

ApexImageManager::ApexImageManager(const std::string& metadata_dir,
                                   const std::string& data_dir)
    : metadata_dir_(metadata_dir),
      data_dir_(data_dir),
      fsmgr_(fiemap::ImageManager::Open(metadata_dir, data_dir)) {}

// PinApexFiles makes apex_files accessible even before /data is mounted. At a
// high-level, it pins those apex files, extract their extents, and save the
// extents in metadata_dir_. Later on, regardless of whether /data is mounted or
// not, one can use the extents to build dm-liner block devices which will give
// direct access to the apex files content, effectively bypassing the filesystem
// layer.
//
// However, in reality, it's slightly more complex than this. Any data stored in
// /data is encrypted via dm-default-key. This means that if you construct the
// dm-liner block devices directly from the extents of the apex files, you will
// get encrypted data when reading the block devices.
//
// To work around this problem, for each apex file, this function creates a new
// file in data_dir_/<name>.img that has the size >= size of the apex
// file. That new file is then pinned, and its extents are saved to
// metadata_dir_/. Then the function constructs a temporary dm-linear block
// device using the extents and copy the content of apex file to the block
// device. By doing so, the block device have unencrypted copy of the apex file.
//
// This comes with a size overhead of extra copies of APEX files and wasted
// space due to the file-system specific granularity of pinned files.
// (TODO/402256229)
Result<std::vector<std::string>> ApexImageManager::PinApexFiles(
    std::span<const ApexFile> apex_files) {
  std::vector<std::string> new_images;
  // On error, clean up new backing files
  auto guard = base::make_scope_guard([&]() {
    for (const auto& image : new_images) {
      fsmgr_->DeleteBackingImage(image);
    }
  });

  for (const auto& apex_file : apex_files) {
    // Get a unique "image" name from the apex name
    auto image_name = AllocateNewName(fsmgr_->GetAllBackingImages(),
                                      apex_file.GetManifest().name());

    auto apex_path = apex_file.GetPath();
    auto file_size = OR_RETURN(GetFileSize(apex_path));

    // Create a pinned file for the apex file using
    // fiemap::ImageManager::CreateBackingImage() which creates
    // /data/apex/images/{image_name}.img and saves its extents in
    // /metadata/apex/images/lp_metadata.
    auto status = fsmgr_->CreateBackingImage(image_name, file_size, 0);
    if (!status.is_ok()) {
      return Error() << "Failed to create a pinned backing file for "
                     << apex_path;
    }
    new_images.emplace_back(image_name);

    // Now, copy the apex file to the pinned file thru the block device which
    // bypasseses the filesystem (/data) and encyryption layer (dm-default-key).
    // MappedDevice::Open() constructs a dm-linear device from the extents of
    // the pinned file.
    auto device = fiemap::MappedDevice::Open(fsmgr_.get(), 10s, image_name);
    if (!device) {
      return Error() << "Failed to map the image: " << image_name;
    }
    OR_RETURN(SendFile(device->fd(), apex_path, file_size));
  }

  guard.Disable();
  return new_images;
}

Result<void> ApexImageManager::DeleteImage(const std::string& image) {
  if (!fsmgr_->DeleteBackingImage(image)) {
    return Error() << "Failed to delete backing image: " << image;
  }
  return {};
}

Result<void> ApexImageManager::UnmapAndDeleteImage(const std::string& image) {
  OR_RETURN(UnmapImageIfExists(image));
  return DeleteImage(image);
}

std::vector<std::string> ApexImageManager::GetAllImages() {
  return fsmgr_->GetAllBackingImages();
}

std::optional<std::string> ApexImageManager::FindPinnedApex(
    const ApexFile& apex) const {
  DeviceMapper& dm = DeviceMapper::Instance();
  if (!dm.IsDmBlockDevice(apex.GetPath())) {
    return std::nullopt;
  }
  auto name = dm.GetDmDeviceNameByPath(apex.GetPath());
  if (!name) {
    return std::nullopt;
  }
  // TODO(405903373): Cache lp_metadata for faster lookup
  if (fsmgr_->BackingImageExists(name.value())) {
    return name.value();
  }
  return std::nullopt;
}

std::optional<std::string> ApexImageManager::GetMappedPath(
    const std::string& image) {
  std::string path;
  if (fsmgr_->GetMappedImageDevice(image, &path)) {
    return path;
  }
  return std::nullopt;
}

Result<std::string> ApexImageManager::MapImage(const std::string& image) {
  std::string path;
  if (fsmgr_->GetMappedImageDevice(image, &path)) {
    return path;
  }
  if (!fsmgr_->MapImageDevice(image, 10s, &path)) {
    return Error() << "Failed to create dm-linear device for " << image;
  }
  return path;
}

Result<void> ApexImageManager::UnmapImage(const std::string& image) {
  if (!fsmgr_->UnmapImageDevice(image)) {
    return Error() << "Failed to unmap dm-linear device for " << image;
  }
  return {};
}

Result<void> ApexImageManager::UnmapImageIfExists(const std::string& image) {
  if (fsmgr_->IsImageMapped(image)) {
    return UnmapImage(image);
  }
  return {};
}

std::string ApexImageManager::GetApexListFile(ApexListType list_type) const {
  switch (list_type) {
    case ApexListType::ACTIVE:
      return metadata_dir_ + "/active";
    case ApexListType::BACKUP:
      return metadata_dir_ + "/backup";
  }
}

Result<void> ApexImageManager::UpdateApexList(
    ApexListType list_type, const std::vector<ApexListEntry>& list) {
  auto listfile = GetApexListFile(list_type);

  // Write to a tempfile first and then rename it to target name to avoid
  // losing an existing file or half-written file.

  auto tempfile = listfile + ".tmp";
  OR_RETURN(WriteImageList(list, tempfile));

  auto cleanup = base::make_scope_guard([&]() {
    if (auto rc = unlink(tempfile.c_str()); rc == -1 && errno != ENOENT) {
      PLOG(ERROR) << "Fail to delete " << tempfile;
    }
  });

  // rename() replaces an existing file if there's any.
  if (auto rc = rename(tempfile.c_str(), listfile.c_str()); rc == -1) {
    return ErrnoError() << "Fail to create " << listfile;
  }
  return {};
}

Result<std::vector<ApexListEntry>> ApexImageManager::GetApexList(
    ApexListType list_type) {
  auto list_file = GetApexListFile(list_type);
  return ReadImageList(list_file);
}

ApexImageManager* GetImageManager() { return gImageManager; }

void InitializeImageManager(ApexImageManager* image_manager) {
  gImageManager = image_manager;
}

std::unique_ptr<ApexImageManager> ApexImageManager::Create(
    const std::string& metadata_images_dir,
    const std::string& data_images_dir) {
  return std::unique_ptr<ApexImageManager>(
      new ApexImageManager(metadata_images_dir, data_images_dir));
}

}  // namespace android::apex