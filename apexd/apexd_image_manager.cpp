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
#include <libfiemap/split_fiemap_writer.h>
#include <sys/sendfile.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <type_traits>

#include "apex_image_list.pb.h"
#include "apex_storage_metadata.pb.h"
#include "apexd.h"
#include "apexd_dm.h"
#include "apexd_image_manager_private.h"
#include "apexd_utils.h"
#include "interval.h"

using android::base::borrowed_fd;
using android::base::ErrnoError;
using android::base::Error;
using android::base::RemoveFileIfExists;
using android::base::Result;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::fiemap::FiemapWriter;
using android::fiemap::SplitFiemap;
using apex::proto::ApexStorageMetadata;

using namespace std::chrono_literals;

namespace android::apex {

namespace {

ApexImageManager* gImageManager;

// Utility type for static_assert at the end of `if constexpr` branches.
template <typename T>
struct TypeDependentFalse {
  enum { value = false };
};

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

Result<void> SendFile(const std::string& dest_path, const std::string& src_path,
                      size_t size) {
  unique_fd dest_fd(open(dest_path.c_str(), O_RDWR | O_CLOEXEC));
  if (!dest_fd.ok()) {
    return Error() << "Failed to open " << dest_path;
  }
  return SendFile(dest_fd, src_path, size);
}

Result<void> EnsureBlockDeviceIsUserdata(const std::string& bdev) {
  struct stat userdata, given;
  if (!stat(bdev.c_str(), &given) && !stat(kUserdataDevice, &userdata)) {
    if (S_ISBLK(given.st_mode) && S_ISBLK(userdata.st_mode) &&
        given.st_rdev == userdata.st_rdev) {
      return {};
    }
    return Error() << "Invalid device for the APEX storage, which should be "
                   << kUserdataDevice << ", but is " << bdev;
  }
  return ErrnoError() << "Failed to stat " << bdev;
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

Result<ApexStorageMetadata> ApexStorageMetadata_Load(
    const std::string& filename) {
  unique_fd fd(open(filename.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd < 0) {
    if (errno == ENOENT) {
      return {};
    }
    return ErrnoError() << "Failed to open " << filename;
  }

  std::string content;
  if (!base::ReadFdToString(fd.get(), &content)) {
    return ErrnoError() << "Failed to read " << filename;
  }

  ApexStorageMetadata metadata;
  if (!metadata.ParseFromString(content)) {
    return Error() << "Failed to parse " << filename;
  }
  return metadata;
}

Result<void> ApexStorageMetadata_Save(const ApexStorageMetadata& metadata,
                                      const std::string& filename) {
  auto temp_filename = filename + ".tmp";

  std::string content;
  if (!metadata.SerializeToString(&content)) {
    return Error() << "Failed to serialize ApexStorageMetadata";
  }
  if (!base::WriteStringToFile(content, temp_filename)) {
    return ErrnoError() << "Failed to write " << temp_filename;
  }
  if (auto rc = rename(temp_filename.c_str(), filename.c_str()); rc == -1) {
    return ErrnoError() << "Failed to rename " << temp_filename << " to "
                        << filename;
  }
  return {};
}

std::vector<std::string> ApexStorageMetadata_GetAllImageNames(
    const ApexStorageMetadata& metadata) {
  std::vector<std::string> image_names;
  image_names.reserve(metadata.images_size());
  for (const auto& image_info : metadata.images()) {
    image_names.emplace_back(image_info.image_name());
  }
  return image_names;
}

void ApexStorageMetadata_AddApexImageInfo(ApexStorageMetadata& metadata,
                                          const std::string& name,
                                          const std::vector<Interval>& extents,
                                          time_t mtime) {
  auto& bp_image_info = *metadata.add_images();
  bp_image_info.set_image_name(name);
  bp_image_info.mutable_extents()->Reserve(extents.size());
  for (const auto& extent : extents) {
    auto& pb_extent = *bp_image_info.add_extents();
    pb_extent.set_offset(extent.offset);
    pb_extent.set_length(extent.length);
  }
  bp_image_info.set_mtime(mtime);
}

Result<DmDevice> CreateDmLinear(const std::string& name,
                                const std::string& block_dev,
                                const std::vector<Interval>& extents,
                                bool read_only) {
  DmTable table;
  uint64_t sector = 0;
  for (const auto& extent : extents) {
    if (extent.offset % kBytesInSector != 0 ||
        extent.length % kBytesInSector != 0) {
      return Error() << "Failed to create dm-linear: Extent is not "
                        "sector-aligned: offset="
                     << extent.offset << ", length=" << extent.length;
    }
    table.Emplace<DmTargetLinear>(sector, extent.length / kBytesInSector,
                                  block_dev, extent.offset / kBytesInSector);
    sector += extent.length / kBytesInSector;
  }
  if (read_only) {
    table.set_readonly(true);
  }
  return CreateDmDevice(name, table, /*reuse=*/false);
}

template <typename T>
Interval ExtentToInterval(const T& extent) {
  if constexpr (std::is_same_v<T, struct fiemap_extent>) {
    return {extent.fe_physical, extent.fe_length};
  } else if constexpr (requires {
                         extent.offset();
                         extent.length();
                       }) {
    return {extent.offset(), extent.length()};
  } else {
    static_assert(TypeDependentFalse<T>::value, "Invalid extent type");
  }
}

std::vector<Interval> ExtentsToIntervals(const auto& extents) {
  std::vector<Interval> intervals;
  intervals.reserve(extents.size());
  for (const auto& extent : extents) {
    intervals.emplace_back(ExtentToInterval(extent));
  }
  return intervals;
}

uint64_t DeterminePinnedFileAlignment(const std::string& data_dir) {
  std::string tempfile = data_dir + "/tempfile";
  // Create the smallest file possible (one block).
  auto writer = FiemapWriter::Open(tempfile, 1);
  if (!writer) {
    // fallback to 1, which leads to allocating a pinned file per apex.
    return 1;
  }
  auto intervals = ExtentsToIntervals(writer->extents());
  auto allocated_size = IntervalsGetLength(intervals);
  unlink(tempfile.c_str());
  return allocated_size;
}

std::vector<Interval> ApexStorageMetadata_GetUsedExtents(
    const ApexStorageMetadata& metadata) {
  std::vector<Interval> used_extents;
  for (const auto& apex_image : metadata.images()) {
    used_extents.append_range(ExtentsToIntervals(apex_image.extents()));
  }
  return used_extents;
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

Result<std::unique_ptr<SplitFiemap>> OpenOrCreateApexStorage(
    const std::string& storage_path, uint64_t initial_size) {
  auto storage = SplitFiemap::Open(storage_path);
  if (!storage) {
    storage = SplitFiemap::Create(storage_path, initial_size, 0);
    if (!storage) {
      return Error() << "Failed to create APEX storage at " << storage_path;
    }
  }
  // The SplitFiemap should be on top of "userdata" partition.
  auto block_dev = storage->bdev_path();
  OR_RETURN(EnsureBlockDeviceIsUserdata(block_dev));
  return storage;
}

// Heuristic: it's typical for f2fs to use 2 MiB alignment for pinned file
// allocation. But some implementation may use much bigger value like > 1 GiB.
// 2 MiB is chosen to use "APEX storage (backing/pinned file) per APEX image"
// strategy for most cases (including EXT4, of which the alignment is
// considered as 1). For a big alignment device, we'll use a single APEX storage
// to avoid wasting space that's allocated but not actually used.
static bool HasApexStoragePerImage(const ApexStorageMetadata& metadata) {
  return metadata.allocation_alignment() <= 2 * 1024 * 1024;  // 2 MiB
}

Result<std::unique_ptr<FreeSpaceAllocator>> FreeSpaceAllocator::Create(
    const std::string& storage_path, uint64_t initial_size,
    const std::vector<Interval>& used_extents) {
  auto storage = OR_RETURN(OpenOrCreateApexStorage(storage_path, initial_size));
  // Calculate free space by subtracting APEX allocation from the entire APEX
  // Storage.
  auto free_extents =
      SubtractIntervals(ExtentsToIntervals(storage->extents()), used_extents);
  auto free_space = IntervalsGetLength(free_extents);

  // Grow the APEX Storage if necessary.
  // TODO(b/402256229) Consider shrink. BootCompletedCleanUp() might be a good
  // chance to shrink.
  if (free_space < initial_size) {
    LOG(INFO) << "Free space (" << free_space
              << " bytes) is not enough for incoming APEXes (" << initial_size
              << " bytes). Growing it by " << (initial_size - free_space)
              << " bytes.";
    if (!storage->Grow(initial_size - free_space)) {
      return Error() << "Failed to grow apex.img";
    }
    // Update free extents after growing
    free_extents =
        SubtractIntervals(ExtentsToIntervals(storage->extents()), used_extents);
  }
  return std::make_unique<FreeSpaceAllocator>(std::move(free_extents));
}

Result<std::vector<Interval>> FreeSpaceAllocator::CreateImage(
    const std::string& image_name, uint64_t size) {
  std::vector<Interval> allocated;
  while (size > 0) {
    if (free_extents.empty()) {
      return Error() << "Failed to allocate " << image_name << " (" << size
                     << " bytes).";
    }

    // free_extents is sorted with the largest one first, so we can just take
    // from the front/top.
    Interval largest_extent = free_extents.top();
    free_extents.pop();

    auto [taken, remaining] = largest_extent.SplitAtLength(size);
    allocated.push_back(taken);
    size -= taken.length;

    if (remaining.length > 0) {
      free_extents.push(remaining);
    }
  }
  return NormalizeIntervals(allocated);
}

ApexStoragePerImageCreator::~ApexStoragePerImageCreator() {
  for (const auto& file : intermediate_files) {
    SplitFiemap::RemoveSplitFiles(file);
  }
}

Result<std::vector<Interval>> ApexStoragePerImageCreator::CreateImage(
    const std::string& image_name, uint64_t size) {
  auto storage_path = data_dir + "/" + image_name;
  auto storage = SplitFiemap::Create(storage_path, size, 0);
  if (!storage) {
    return Error() << "Failed to create APEX storage for " << image_name << " ("
                   << size << "): " << storage_path;
  }
  intermediate_files.push_back(storage_path);
  // SplitFiemap may be bigger due to the allocation alignment.
  auto [allocated, _] =
      TakeLengthFromStart(ExtentsToIntervals(storage->extents()), size);
  if (IntervalsGetLength(allocated) != size) {
    return Error() << "Failed to allocate " << image_name << " (" << size
                   << ") from " << storage_path;
  }
  return allocated;
}

ApexImageManager::ApexImageManager(const std::string& metadata_dir,
                                   const std::string& data_dir)
    : metadata_dir_(metadata_dir), data_dir_(data_dir) {}

// PinApexFiles makes apex_files accessible even before /data is mounted. At a
// high-level, it pins those apex files, extract their extents, and save the
// extents in metadata_dir_. Later on, regardless of whether /data is mounted or
// not, one can use the extents to build dm-linear block devices which will give
// direct access to the apex files content, effectively bypassing the filesystem
// layer.
//
// However, in reality, it's slightly more complex than this. Any data stored in
// /data is encrypted via dm-default-key. This means that if you construct the
// dm-linear block devices directly from the extents of the apex files, you will
// get encrypted data when reading the block devices.
//
// To work around this problem, this function creates a new file in
// data_dir_. That file is then pinned. Its extents are used to store APEX
// files. A temporary dm-linear block device is constructed with those extents,
// and then the content of apex file is copied to the block device. By doing so,
// the block device have unencrypted copy of the apex file. The extents
// allocated for APEX files are stored in metadata_dir/apex.img.metadata.
Result<std::vector<std::string>> ApexImageManager::PinApexFiles(
    std::span<const ApexFile> apex_files) {
  // The locations (aka extents) where APEX files are stored are handled by
  // ApexStorageMetadata (/metadata/apex/images/apex.img.metadata)
  auto storage_metadata_path = GetApexStorageMetadataPath();
  auto metadata = OR_RETURN(ApexStorageMetadata_Load(storage_metadata_path));

  // Determine the allocation alignment of pinned files first.
  if (metadata.allocation_alignment() == 0) {
    auto alignment = DeterminePinnedFileAlignment(data_dir_);
    metadata.set_allocation_alignment(alignment);
    LOG(INFO) << "Allocation alignment is " << alignment;
  }

  // If the alignment is small (e.g. 2 MiB), use the one backing/pinned file per
  // APEX file strategy. Otherwise, we create a single split-file (apex.img) and
  // put all APEX files in it.
  std::unique_ptr<ImageCreator> image_creator;
  if (HasApexStoragePerImage(metadata)) {
    image_creator = std::make_unique<ApexStoragePerImageCreator>(data_dir_);
  } else {
    auto new_apex_size = 0ul;
    for (const auto& apex_file : apex_files) {
      auto apex_path = apex_file.GetPath();
      new_apex_size += OR_RETURN(GetFileSize(apex_path));
    }

    auto storage_path = data_dir_ + "/apex.img";
    auto used_extents = ApexStorageMetadata_GetUsedExtents(metadata);
    image_creator = OR_RETURN(
        FreeSpaceAllocator::Create(storage_path, new_apex_size, used_extents));
  }

  // Now, okay to store incoming APEX files to the store.

  std::vector<std::string> new_images;
  new_images.reserve(apex_files.size());
  for (const auto& apex_file : apex_files) {
    // Get a unique "image" name from the apex name
    auto image_name =
        AllocateNewName(ApexStorageMetadata_GetAllImageNames(metadata),
                        apex_file.GetManifest().name());
    new_images.emplace_back(image_name);

    auto apex_path = apex_file.GetPath();
    auto file_size = OR_RETURN(GetFileSize(apex_path));
    auto mtime = OR_RETURN(GetLastModifiedTime(apex_path));

    auto extents = OR_RETURN(image_creator->CreateImage(image_name, file_size));

    // Update APEX storage metadata
    ApexStorageMetadata_AddApexImageInfo(metadata, image_name, extents, mtime);

    // Now, copy the apex file to the APEX storage thru the dm-linear block
    // device which bypasseses the filesystem (/data) and encyryption layer
    // (dm-default-key).
    auto dev = OR_RETURN(CreateDmLinear(image_name, kUserdataDevice, extents,
                                        /*read_only=*/false));
    OR_RETURN(SendFile(dev.GetDevPath(), apex_path, file_size));
  }

  // Now save the metadata.
  OR_RETURN(ApexStorageMetadata_Save(metadata, storage_metadata_path));
  image_creator->MarkDone();
  return new_images;
}

Result<void> ApexImageManager::DeleteImage(const std::string& image) {
  auto metadata_path = GetApexStorageMetadataPath();
  auto metadata = OR_RETURN(ApexStorageMetadata_Load(metadata_path));

  auto it = std::find_if(
      metadata.images().begin(), metadata.images().end(),
      [&](const auto& image_info) { return image_info.image_name() == image; });
  if (it == metadata.images().end()) {
    return Error() << "Failed to delete image " << image << ": not found";
  }
  // Erase the entry and save the updated metadata
  metadata.mutable_images()->erase(it);

  if (HasApexStoragePerImage(metadata)) {
    auto storage_path = data_dir_ + "/" + image;
    std::string message;
    if (!SplitFiemap::RemoveSplitFiles(storage_path, &message)) {
      return Error() << "Failed to delete image " << image << ": " << message;
    }
  }
  return ApexStorageMetadata_Save(metadata, metadata_path);
}

Result<void> ApexImageManager::UnmapAndDeleteImage(const std::string& image) {
  OR_RETURN(UnmapImageIfExists(image));
  return DeleteImage(image);
}

std::vector<std::string> ApexImageManager::GetAllImages() const {
  std::vector<std::string> images;
  auto metadata_path = GetApexStorageMetadataPath();
  auto metadata = ApexStorageMetadata_Load(metadata_path);
  if (metadata.ok()) {
    images.reserve(metadata->images_size());
    for (const auto& image : metadata->images()) {
      images.emplace_back(image.image_name());
    }
  }
  return images;
}

std::optional<std::string> ApexImageManager::FindPinnedApex(
    const ApexFile& apex) const {
  // Get the dm-device name first. Note that dm-linear devices created for APEX
  // images are named with image names.
  auto& dm = DeviceMapper::Instance();
  if (!dm.IsDmBlockDevice(apex.GetPath())) {
    return std::nullopt;
  }
  auto name = dm.GetDmDeviceNameByPath(apex.GetPath());
  if (!name) {
    return std::nullopt;
  }
  // Verify the name is actually one of those APEX images.
  // TODO(405903373): Cache lp_metadata for faster lookup
  if (std::ranges::contains(GetAllImages(), *name)) {
    return *name;
  }
  return std::nullopt;
}

std::optional<std::string> ApexImageManager::GetMappedPath(
    const std::string& image) const {
  auto& dm = DeviceMapper::Instance();
  if (dm.GetState(image) == DmDeviceState::INVALID) {
    return std::nullopt;
  }
  std::string path;
  if (dm.GetDmDevicePathByName(image, &path)) {
    return path;
  }
  return std::nullopt;
}

Result<std::string> ApexImageManager::MapImage(const std::string& image) {
  // Check if it's already mapped.
  auto path = GetMappedPath(image);
  if (path) {
    return *path;
  }

  // Otherwise, map the image to a dm-linear device:
  // 1. load the metadata
  // 2. get the extents of the image
  // 3. create a dm-linear device with the extent

  auto metadata_path = GetApexStorageMetadataPath();
  auto metadata = OR_RETURN(ApexStorageMetadata_Load(metadata_path));

  // get extents of the image.
  auto it = std::find_if(
      metadata.images().begin(), metadata.images().end(),
      [&](const auto& image_info) { return image_info.image_name() == image; });
  if (it == metadata.images().end()) {
    return Error() << "Failed to find image " << image;
  }
  auto extents = ExtentsToIntervals(it->extents());
  auto mtime = it->mtime();

  // create a dm-linear device on the userdata partition
  auto dev = OR_RETURN(
      CreateDmLinear(image, kUserdataDevice, extents, /*read_only=*/true));
  auto dev_path = dev.GetDevPath();
  OR_RETURN(SetLastModifiedTime(dev_path, mtime));
  dev.Release();  // dm-linear device should not be deleted on exit
  return dev_path;
}

Result<void> ApexImageManager::UnmapImage(const std::string& image) {
  // Dm-linear device mapped for an APEX image is named after the image name.
  auto& dm = DeviceMapper::Instance();
  if (!dm.DeleteDevice(image)) {
    return Error() << "Failed to unmap image " << image;
  }
  return {};
}

Result<void> ApexImageManager::UnmapImageIfExists(const std::string& image) {
  auto& dm = DeviceMapper::Instance();
  if (!dm.DeleteDeviceIfExists(image)) {
    return Error() << "Failed to unmap image " << image;
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

std::string ApexImageManager::GetApexStorageMetadataPath() const {
  return metadata_dir_ + "/apex.img.metadata";
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
    ApexListType list_type) const {
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