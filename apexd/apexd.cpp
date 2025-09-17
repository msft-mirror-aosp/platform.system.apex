/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define ATRACE_TAG ATRACE_TAG_PACKAGE_MANAGER

#include "apexd.h"

#include <ApexProperties.sysprop.h>
#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/thread_annotations.h>
#include <android-base/unique_fd.h>
#include <dirent.h>
#include <fcntl.h>
#include <google/protobuf/util/message_differencer.h>
#include <libavb/libavb.h>
#include <libdm/dm.h>
#include <libdm/dm_table.h>
#include <libdm/dm_target.h>
#include <linux/loop.h>
#include <selinux/android.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Trace.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "VerityUtils.h"
#include "apex_constants.h"
#include "apex_database.h"
#include "apex_file.h"
#include "apex_file_repository.h"
#include "apex_manifest.h"
#include "apex_sha.h"
#include "apex_shim.h"
#include "apexd_brand_new_verifier.h"
#include "apexd_checkpoint.h"
#include "apexd_dm.h"
#include "apexd_image_manager.h"
#include "apexd_lifecycle.h"
#include "apexd_loop.h"
#include "apexd_metrics.h"
#include "apexd_private.h"
#include "apexd_rollback_utils.h"
#include "apexd_session.h"
#include "apexd_utils.h"
#include "apexd_vendor_apex.h"
#include "apexd_verity.h"
#include "com_android_apex.h"
#include "com_android_apex_flags.h"

namespace flags = com::android::apex::flags;
namespace fs = std::filesystem;

using android::base::boot_clock;
using android::base::ConsumePrefix;
using android::base::ErrnoError;
using android::base::Error;
using android::base::GetBoolProperty;
using android::base::GetProperty;
using android::base::Join;
using android::base::ParseUint;
using android::base::RemoveFileIfExists;
using android::base::Result;
using android::base::SetProperty;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::base::WriteStringToFile;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetVerity;
using ::apex::proto::ApexManifest;
using apex::proto::SessionState;
using google::protobuf::util::MessageDifferencer;

namespace android {
namespace apex {

using MountedApexData = MountedApexDatabase::MountedApexData;
Result<std::vector<ApexFile>> OpenApexFilesInSessionDirs(
    int session_id, const std::vector<int>& child_session_ids);

Result<std::vector<std::string>> StagePackagesImpl(
    const std::vector<std::string>& tmp_paths);

namespace {

static constexpr const char* kBuildFingerprintSysprop = "ro.build.fingerprint";

// This should be in UAPI, but it's not :-(
static constexpr const char* kDmVerityRestartOnCorruption =
    "restart_on_corruption";

MountedApexDatabase gMountedApexes;

// Can be set by SetConfig()
std::optional<ApexdConfig> gConfig;

// Set by InitializeSessionManager
ApexSessionManager* gSessionManager;

CheckpointInterface* gVoldService;
bool gSupportsFsCheckpoints = false;
bool gInFsCheckpointMode = false;

// Process-wise global mutex to serialize install/staging functions:
// - submitStagedSession
// - markStagedSessionReady
// - installAndActivatePackage
// This is to ensure that there's no overlapping between install/staging.
// To be specific, we don't want to perform verification when there's a
// VERIFIED session, which is not yet fully staged.
struct Mutex : std::mutex {
  const Mutex& operator!() const { return *this; }  // for negative capability
} gInstallLock;

static constexpr size_t kLoopDeviceSetupAttempts = 3u;

// Please DO NOT add new modules to this list without contacting
// mainline-modularization@ first.
static const std::vector<std::string> kBootstrapApexes = ([]() {
  std::vector<std::string> ret = {
      "com.android.i18n",
      "com.android.runtime",
      "com.android.tzdata",
#ifdef RELEASE_AVF_ENABLE_EARLY_VM
      "com.android.virt",
#endif
  };

  auto vendor_vndk_ver = GetProperty("ro.vndk.version", "");
  if (vendor_vndk_ver != "") {
    ret.push_back("com.android.vndk.v" + vendor_vndk_ver);
  }
  auto product_vndk_ver = GetProperty("ro.product.vndk.version", "");
  if (product_vndk_ver != "" && product_vndk_ver != vendor_vndk_ver) {
    ret.push_back("com.android.vndk.v" + product_vndk_ver);
  }
  return ret;
})();

static constexpr const int kNumRetriesWhenCheckpointingEnabled = 1;

bool IsBootstrapApex(const ApexFile& apex) {
  static std::vector<std::string> additional = []() {
    std::vector<std::string> ret;
    if (android::base::GetBoolProperty("ro.boot.apex.early_adbd", false)) {
      ret.push_back("com.android.adbd");
    }
    return ret;
  }();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (apex.GetManifest().vendorbootstrap() || apex.GetManifest().bootstrap()) {
    return true;
  }
#pragma clang diagnostic pop

  return std::find(kBootstrapApexes.begin(), kBootstrapApexes.end(),
                   apex.GetManifest().name()) != kBootstrapApexes.end() ||
         std::find(additional.begin(), additional.end(),
                   apex.GetManifest().name()) != additional.end();
}

std::unique_ptr<DmTable> CreateVerityTable(const ApexVerityData& verity_data,
                                           const std::string& block_device,
                                           bool restart_on_corruption) {
  AvbHashtreeDescriptor* desc = verity_data.desc.get();
  auto table = std::make_unique<DmTable>();

  const uint64_t start = 0;
  const uint64_t length = desc->image_size / 512;  // in sectors

  const std::string& hash_device = block_device;
  const uint32_t num_data_blocks = desc->image_size / desc->data_block_size;
  const uint32_t hash_start_block = desc->tree_offset / desc->hash_block_size;

  auto target = std::make_unique<DmTargetVerity>(
      start, length, desc->dm_verity_version, block_device, hash_device,
      desc->data_block_size, desc->hash_block_size, num_data_blocks,
      hash_start_block, verity_data.hash_algorithm, verity_data.root_digest,
      verity_data.salt);

  target->IgnoreZeroBlocks();
  if (restart_on_corruption) {
    target->SetVerityMode(kDmVerityRestartOnCorruption);
  }
  table->AddTarget(std::move(target));

  table->set_readonly(true);

  return table;
};

/**
 * When we create hardlink for a new apex package in kActiveApexPackagesDataDir,
 * there might be an older version of the same package already present in there.
 * Since a new version of the same package is being installed on this boot, the
 * old one needs to deleted so that we don't end up activating same package
 * twice.
 *
 * @param affected_packages package names of the news apex that are being
 * installed in this boot
 * @param files_to_keep path to the new apex packages in
 * kActiveApexPackagesDataDir
 */
Result<void> RemovePreviouslyActiveApexFiles(
    const std::vector<std::string>& affected_packages,
    const std::vector<std::string>& files_to_keep) {
  auto all_active_apex_files =
      FindFilesBySuffix(gConfig->active_apex_data_dir, {kApexPackageSuffix});

  if (!all_active_apex_files.ok()) {
    return all_active_apex_files.error();
  }

  for (const std::string& path : *all_active_apex_files) {
    if (std::ranges::contains(files_to_keep, path)) {
      // This is a path that was staged and should be kept.
      continue;
    }

    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return apex_file.error();
    }
    const std::string& package_name = apex_file->GetManifest().name();
    if (!std::ranges::contains(affected_packages, package_name)) {
      // This apex belongs to a package that wasn't part of this stage sessions,
      // hence it should be kept.
      continue;
    }

    LOG(DEBUG) << "Deleting previously active apex " << apex_file->GetPath();
    if (unlink(apex_file->GetPath().c_str()) != 0) {
      return ErrnoError() << "Failed to unlink " << apex_file->GetPath();
    }
  }

  return {};
}

// Reads the entire device to verify the image is authenticatic
Result<void> ReadVerityDevice(const std::string& verity_device,
                              uint64_t device_size) {
  static constexpr int kBlockSize = 4096;
  static constexpr size_t kBufSize = 1024 * kBlockSize;
  std::vector<uint8_t> buffer(kBufSize);

  unique_fd fd(
      TEMP_FAILURE_RETRY(open(verity_device.c_str(), O_RDONLY | O_CLOEXEC)));
  if (fd.get() == -1) {
    return ErrnoError() << "Can't open " << verity_device;
  }

  size_t bytes_left = device_size;
  while (bytes_left > 0) {
    size_t to_read = std::min(bytes_left, kBufSize);
    if (!android::base::ReadFully(fd.get(), buffer.data(), to_read)) {
      return ErrnoError() << "Can't verify " << verity_device << "; corrupted?";
    }
    bytes_left -= to_read;
  }

  return {};
}

Result<void> VerifyMountedImage(const ApexFile& apex,
                                const std::string& mount_point) {
  // Verify that apex_manifest.pb inside mounted image matches the one in the
  // outer .apex container.
  Result<ApexManifest> verified_manifest =
      ReadManifest(mount_point + "/" + kManifestFilenamePb);
  if (!verified_manifest.ok()) {
    return verified_manifest.error();
  }
  if (!MessageDifferencer::Equals(*verified_manifest, apex.GetManifest())) {
    return Errorf(
        "Manifest inside filesystem does not match manifest outside it");
  }
  if (shim::IsShimApex(apex)) {
    return shim::ValidateShimApex(mount_point, apex);
  }
  return {};
}

Result<loop::LoopbackDeviceUniqueFd> CreateLoopForApex(const ApexFile& apex,
                                                       int32_t loop_id) {
  if (!apex.GetImageOffset() || !apex.GetImageSize()) {
    return Error() << "Cannot create mount point without image offset and size";
  }
  const std::string& full_path = apex.GetPath();
  loop::LoopbackDeviceUniqueFd loopback_device;
  for (size_t attempts = 1;; ++attempts) {
    Result<loop::LoopbackDeviceUniqueFd> ret =
        loop::CreateAndConfigureLoopDevice(
            full_path, apex.GetImageOffset().value(),
            apex.GetImageSize().value(), loop_id);
    if (ret.ok()) {
      loopback_device = std::move(*ret);
      break;
    }
    if (attempts >= kLoopDeviceSetupAttempts) {
      return Error() << "Could not create loop device for " << full_path << ": "
                     << ret.error();
    }
  }
  LOG(VERBOSE) << "Loopback device created: " << loopback_device.name;
  return std::move(loopback_device);
}

bool IsMountBeforeDataEnabled() { return gConfig->mount_before_data; }

[[maybe_unused]] bool CanMountBeforeDataOnNextBoot() {
  // If there's no data apex files in /data/apex/active and no capex files, then
  // apexd-bootstrap can mount ALL apexes (preinstalled and pinned data apexes).
  if (!IsEmptyDirectory(gConfig->active_apex_data_dir)) {
    return false;
  }
  auto& repo = ApexFileRepository::GetInstance();
  if (std::ranges::any_of(
          repo.GetPreInstalledApexFiles(),
          [](const ApexFile& apex) { return apex.IsCompressed(); })) {
    return false;
  }
  return true;
}

Result<DmDevice> CreateDmLinearForPayload(const ApexFile& apex) {
  if (!apex.GetImageOffset() || !apex.GetImageSize()) {
    return Error() << "Cannot create mount point without image offset and size";
  }

  auto image_manager = GetImageManager();
  auto image_name = image_manager->FindPinnedApex(apex);
  if (!image_name) {
    return Error() << "Not a pinned apex: " << apex.GetPath();
  }

  auto extents = OR_RETURN(image_manager->GetImageExtents(*image_name));

  auto payload_extents =
      ApplyOffsetLength(extents, *apex.GetImageOffset(), *apex.GetImageSize());

  auto device_name = *image_name + kDmLinearPayloadSuffix;
  auto dev =
      OR_RETURN(CreateDmLinear(device_name, kUserdataDevice, payload_extents,
                               /*read_only=*/false));
  OR_RETURN(loop::ConfigureReadAhead(dev.GetDevPath()));
  return std::move(dev);
}

Result<MountedApexData> MountPackageImpl(const ApexFile& apex,
                                         const std::string& mount_point,
                                         int32_t loop_id,
                                         const std::string& device_name,
                                         bool verify_image, bool reuse_device) {
  auto tag = "MountPackageImpl: " + apex.GetManifest().name();
  ATRACE_NAME(tag.c_str());
  if (apex.IsCompressed()) {
    return Error() << "Cannot directly mount compressed APEX "
                   << apex.GetPath();
  }

  // Steps to mount an APEX file:
  //
  // 1. create a mount point (directory)
  // 2. create a block device for the payload part of the APEX
  // 3. wrap it with a dm-verity device if the APEX is not on top of verity
  //    device
  // 4. mount the payload filesystm
  // 5. verify the mount

  // Step 1. Create a directory for the mount point

  LOG(VERBOSE) << "Creating mount point: " << mount_point;
  auto time_started = boot_clock::now();
  // Note: the mount point could exist in case when the APEX was activated
  // during the bootstrap phase (e.g., the runtime or tzdata APEX).
  // Although we have separate mount namespaces to separate the early activated
  // APEXes from the normally activate APEXes, the mount points themselves
  // are shared across the two mount namespaces because /apex (a tmpfs) itself
  // mounted at / which is (and has to be) a shared mount. Therefore, if apexd
  // finds an empty directory under /apex, it's not a problem and apexd can use
  // it.
  auto exists = PathExists(mount_point);
  if (!exists.ok()) {
    return exists.error();
  }
  if (!*exists && mkdir(mount_point.c_str(), kMkdirMode) != 0) {
    return ErrnoError() << "Could not create mount point " << mount_point;
  }
  auto deleter = [&mount_point]() {
    if (rmdir(mount_point.c_str()) != 0) {
      PLOG(WARNING) << "Could not rmdir " << mount_point;
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);
  if (!IsEmptyDirectory(mount_point)) {
    return Error() << mount_point << " is not empty";
  }

  const std::string& full_path = apex.GetPath();

  // Step 2. Create a block device for the payload

  std::string block_device;
  loop::LoopbackDeviceUniqueFd loop;
  DmDevice linear_dev;

  if (IsMountBeforeDataEnabled() && GetImageManager()->IsPinnedApex(apex)) {
    linear_dev = OR_RETURN(CreateDmLinearForPayload(apex));
    block_device = linear_dev.GetDevPath();
  } else {
    loop = OR_RETURN(CreateLoopForApex(apex, loop_id));
    block_device = loop.name;
  }

  // Step 3. Wrap the block device with dm-verity (optional)

  auto verity_data = apex.VerifyApexVerity(apex.GetBundledPublicKey());
  if (!verity_data.ok()) {
    return Error() << "Failed to verify Apex Verity data for " << full_path
                   << ": " << verity_data.error();
  }

  auto& instance = ApexFileRepository::GetInstance();
  if (instance.IsBlockApex(apex)) {
    auto root_digest = instance.GetBlockApexRootDigest(apex.GetPath());
    if (root_digest.has_value() &&
        root_digest.value() != verity_data->root_digest) {
      return Error() << "Failed to verify Apex Verity data for " << full_path
                     << ": root digest (" << verity_data->root_digest
                     << ") mismatches with the one (" << root_digest.value()
                     << ") specified in config";
    }
  }

  // for APEXes in immutable partitions, we don't need to mount them on
  // dm-verity because they are already in the dm-verity protected partition;
  // system. However, note that we don't skip verification to ensure that APEXes
  // are correctly signed.
  const bool mount_on_verity = !instance.IsPreInstalledApex(apex) ||
                               // decompressed apexes are on /data
                               instance.IsDecompressedApex(apex) ||
                               // block apexes are from host
                               instance.IsBlockApex(apex);

  DmDevice verity_dev;
  if (mount_on_verity) {
    auto verity_table =
        CreateVerityTable(*verity_data, block_device,
                          /* restart_on_corruption = */ !verify_image);
    Result<DmDevice> verity_dev_res =
        CreateDmDevice(device_name, *verity_table, reuse_device);
    if (!verity_dev_res.ok()) {
      // verify root digest for better debugging
      if (auto st = VerifyVerityRootDigest(apex); !st.ok()) {
        LOG(ERROR) << "Failed to verify root digest with " << full_path << ": "
                   << st.error();
      }
      return Error() << "Failed to create dm-verity for path=" << full_path
                     << " block=" << block_device << ": "
                     << verity_dev_res.error();
    }
    verity_dev = std::move(*verity_dev_res);
    OR_RETURN(loop::ConfigureReadAhead(verity_dev.GetDevPath()));

    // TODO(b/158467418): consider moving this inside
    // RunVerifyFnInsideTempMount.
    if (verify_image) {
      OR_RETURN(ReadVerityDevice(verity_dev.GetDevPath(),
                                 (*verity_data).desc->image_size));
    }

    block_device = verity_dev.GetDevPath();
  }

  // Step 4. Mount the payload filesystem at the mount point

  uint32_t mount_flags = MS_NOATIME | MS_NODEV | MS_DIRSYNC | MS_RDONLY;
  if (apex.GetManifest().nocode()) {
    mount_flags |= MS_NOEXEC;
  }

  if (!apex.GetFsType()) {
    return Error() << "Cannot mount package without FsType";
  }
  if (mount(block_device.c_str(), mount_point.c_str(),
            apex.GetFsType().value().c_str(), mount_flags, nullptr) != 0) {
    return ErrnoError() << "Mounting failed for package " << full_path;
  }

  // Step 5. After mounting, verify the mounted image

  auto status = VerifyMountedImage(apex, mount_point);
  if (!status.ok()) {
    if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW) != 0) {
      PLOG(ERROR) << "Failed to umount " << mount_point;
    }
    return Error() << "Failed to verify " << full_path << ": "
                   << status.error();
  }

  MountedApexData apex_data(apex.GetManifest().version(), loop.name,
                            apex.GetPath(), mount_point, verity_dev.GetName(),
                            linear_dev.GetName());

  // Time to accept the temporaries as good.
  linear_dev.Release();
  verity_dev.Release();
  loop.CloseGood();
  scope_guard.Disable();

  auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          boot_clock::now() - time_started)
                          .count();
  LOG(VERBOSE) << "Successfully mounted package " << full_path << " on "
               << mount_point << " duration=" << time_elapsed;
  return apex_data;
}

// Run test hook commands specified by the sysprop for testing.
//
// The sysprop value may have a list of commands separated by |.
// Available commands are
// - sleep_ms <ms>: sleep(ms)
// - error <message>: return Error()
Result<void> RunTestHookCommands(const std::string& sysprop) {
  auto hook_commands = GetProperty(sysprop, "");
  if (!hook_commands.empty()) {
    // Clear the sysprop so that the command runs only once
    SetProperty(sysprop, "");

    for (auto command : base::Split(hook_commands, "|")) {
      if (command.empty()) {
        continue;
      }
      LOG(INFO) << "Running " << command;
      auto args = base::Split(command, " ");
      uint32_t num = 0;
      if (args[0] == "sleep_ms" && args.size() == 2 &&
          ParseUint(args[1], &num)) {
        usleep(num * 1000);
      } else if (args[0] == "error" && args.size() == 2) {
        return Error() << args[1];
      } else {
        LOG(ERROR) << "Invalid command: " << command;
      }
    }
  }
  return {};
}

}  // namespace

Result<void> Unmount(const MountedApexData& data, bool deferred) {
  LOG(DEBUG) << "Unmounting " << data.full_path << " from mount point "
             << data.mount_point << " deferred = " << deferred;
  // Unmount whatever is mounted.
  if (umount2(data.mount_point.c_str(), UMOUNT_NOFOLLOW) != 0 &&
      errno != EINVAL && errno != ENOENT) {
    return ErrnoError() << "Failed to unmount directory " << data.mount_point;
  }

  if (!deferred) {
    if (rmdir(data.mount_point.c_str()) != 0) {
      PLOG(ERROR) << "Failed to rmdir " << data.mount_point;
    }
  }

  // Try to free up the device-mapper devices.
  if (!data.verity_name.empty()) {
    OR_RETURN(DeleteDmDevice(data.verity_name, deferred));
  }
  if (!data.linear_name.empty()) {
    OR_RETURN(DeleteDmDevice(data.linear_name, deferred));
  }

  // Since we now use LO_FLAGS_AUTOCLEAR when configuring loop devices, we don't
  // need to manually clear the loop here. (umount2 above will clear the loop.)
  return {};
}

namespace {

auto RunVerifyFnInsideTempMounts(std::span<const ApexFile> apex_files,
                                 auto verify_fn)
    -> decltype(verify_fn(std::vector<std::string>{})) {
  // Temp mounts will be cleaned up on exit.
  std::vector<MountedApexData> mounted_data;
  auto guard = android::base::make_scope_guard([&]() {
    for (const auto& data : mounted_data) {
      if (auto result = Unmount(data, /*deferred=*/false); !result.ok()) {
        LOG(WARNING) << "Failed to unmount " << data.mount_point << ": "
                     << result.error();
      }
    }
  });

  // Temp mounts all apexes.
  // This will also read the entire block device for each apex,
  // so we can be sure there is no corruption.
  std::vector<std::string> mount_points;
  for (const auto& apex : apex_files) {
    auto mount_point =
        apexd_private::GetPackageTempMountPoint(apex.GetManifest());
    auto package_id = GetPackageId(apex.GetManifest());
    auto device_name = package_id + ".tmp";

    LOG(DEBUG) << "Temp mounting " << package_id << " to " << mount_point;
    auto data = OR_RETURN(MountPackageImpl(apex, mount_point, loop::kFreeLoopId,
                                           device_name,
                                           /*verify_image=*/true,
                                           /*reuse_device=*/false));
    mount_points.push_back(mount_point);
    mounted_data.push_back(data);
  }

  // Invoke fn with mount_points.
  return verify_fn(mount_points);
}

// Singluar variant of RunVerifyFnInsideTempMounts for convenience
auto RunVerifyFnInsideTempMount(const ApexFile& apex, auto verify_fn)
    -> decltype(verify_fn(std::string{})) {
  return RunVerifyFnInsideTempMounts(
      Single(apex),
      [&](const auto& mount_points) { return verify_fn(mount_points[0]); });
}

// Converts a list of apex file paths into a list of ApexFile objects
//
// Returns error when trying to open empty set of inputs.
Result<std::vector<ApexFile>> OpenApexFiles(
    const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Errorf("Empty set of inputs");
  }
  std::vector<ApexFile> ret;
  for (const std::string& path : paths) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return apex_file.error();
    }
    ret.emplace_back(std::move(*apex_file));
  }
  return ret;
}

Result<void> ValidateStagingShimApex(const ApexFile& to) {
  using android::base::StringPrintf;
  auto system_shim = ApexFile::Open(
      StringPrintf("%s/%s", kApexPackageSystemDir, shim::kSystemShimApexName));
  if (!system_shim.ok()) {
    return system_shim.error();
  }
  auto verify_fn = [&](const std::string& system_apex_path) {
    return shim::ValidateUpdate(system_apex_path, to.GetPath());
  };
  return RunVerifyFnInsideTempMount(*system_shim, verify_fn);
}

Result<void> VerifyVndkVersion(const ApexFile& apex_file) {
  const std::string& vndk_version = apex_file.GetManifest().vndkversion();
  if (vndk_version.empty()) {
    return {};
  }

  static std::string vendor_vndk_version = GetProperty("ro.vndk.version", "");
  static std::string product_vndk_version =
      GetProperty("ro.product.vndk.version", "");

  const auto& instance = ApexFileRepository::GetInstance();
  const auto& partition = OR_RETURN(instance.GetPartition(apex_file));
  if (partition == ApexPartition::Vendor || partition == ApexPartition::Odm) {
    if (vndk_version != vendor_vndk_version) {
      return Error() << "vndkVersion(" << vndk_version
                     << ") doesn't match with device VNDK version("
                     << vendor_vndk_version << ")";
    }
    return {};
  }
  if (partition == ApexPartition::Product) {
    if (vndk_version != product_vndk_version) {
      return Error() << "vndkVersion(" << vndk_version
                     << ") doesn't match with device VNDK version("
                     << product_vndk_version << ")";
    }
    return {};
  }
  return Error() << "vndkVersion(" << vndk_version << ") is set";
}

// A version of apex verification that happens during boot.
// This function should only verification checks that are necessary to run on
// each boot. Try to avoid putting expensive checks inside this function.
Result<void> VerifyPackageBoot(const ApexFile& apex_file) {
  // Verify bundled key against preinstalled data
  OR_RETURN(apexd_private::CheckBundledPublicKeyMatchesPreinstalled(apex_file));
  // Verify bundled key against apex itself
  OR_RETURN(apex_file.VerifyApexVerity(apex_file.GetBundledPublicKey()));

  if (shim::IsShimApex(apex_file)) {
    // Validating shim is not a very cheap operation, but it's fine to perform
    // it here since it only runs during CTS tests and will never be triggered
    // during normal flow.
    const auto& result = ValidateStagingShimApex(apex_file);
    if (!result.ok()) {
      return result;
    }
  }

  if (auto result = VerifyVndkVersion(apex_file); !result.ok()) {
    return result;
  }

  return {};
}

Result<void> VerifyNoOverlapInSessions(std::span<const ApexFile> apex_files,
                                       std::span<const ApexSession> sessions) {
  for (const auto& session : sessions) {
    // We don't want to install/stage if the same package is already staged.
    if (session.GetState() == SessionState::STAGED) {
      for (const auto& apex : apex_files) {
        if (std::ranges::contains(session.GetApexNames(),
                                  apex.GetManifest().name())) {
          return Error() << "APEX " << apex.GetManifest().name()
                         << " is already staged by session " << session.GetId()
                         << ".";
        }
      }
    }
  }
  return {};  // okay
}

struct VerificationResult {
  std::map<std::string, std::vector<std::string>> apex_hals;
};

// A version of apex verification that happens on SubmitStagedSession.
// This function contains checks that might be expensive to perform, e.g. temp
// mounting a package and reading entire dm-verity device, and shouldn't be run
// during boot.
Result<VerificationResult> VerifyPackagesStagedInstall(
    const std::vector<ApexFile>& apex_files) {
  for (const auto& apex_file : apex_files) {
    OR_RETURN(VerifyPackageBoot(apex_file));
  }

  // Extra verification for brand-new APEX. The case that brand-new APEX is
  // not enabled when there is install request for brand-new APEX is already
  // covered in |VerifyPackageBoot|.
  if (ApexFileRepository::IsBrandNewApexEnabled()) {
    for (const auto& apex_file : apex_files) {
      OR_RETURN(VerifyBrandNewPackageAgainstActive(apex_file, gMountedApexes));
    }
  }

  auto sessions = gSessionManager->GetSessions();

  // Check overlapping: reject if the same package is already staged
  OR_RETURN(VerifyNoOverlapInSessions(apex_files, sessions));

  // Since there can be multiple staged sessions, let's verify incoming APEXes
  // with all staged apexes mounted.
  std::vector<ApexFile> all_apex_files;
  for (const auto& session : sessions) {
    if (session.GetState() != SessionState::STAGED) {
      continue;
    }
    auto session_id = session.GetId();
    auto child_session_ids = session.GetChildSessionIds();
    auto staged_apex_files = OpenApexFilesInSessionDirs(
        session_id, {child_session_ids.begin(), child_session_ids.end()});
    if (staged_apex_files.ok()) {
      std::ranges::move(*staged_apex_files, std::back_inserter(all_apex_files));
    } else {
      // Let's not abort with a previously staged session
      LOG(ERROR) << "Failed to open previously staged APEX files: "
                 << staged_apex_files.error();
    }
  }

  // + incoming APEXes at the end.
  for (const auto& apex_file : apex_files) {
    all_apex_files.push_back(apex_file);
  }

  auto check_fn = [&](const std::vector<std::string>& mount_points)
      -> Result<VerificationResult> {
    VerificationResult result;
    result.apex_hals = OR_RETURN(CheckVintf(all_apex_files, mount_points));
    return result;
  };
  return RunVerifyFnInsideTempMounts(all_apex_files, check_fn);
}

Result<void> DeleteBackup() {
  auto exists = PathExists(std::string(kApexBackupDir));
  if (!exists.ok()) {
    return Error() << "Can't clean " << kApexBackupDir << " : "
                   << exists.error();
  }
  if (!*exists) {
    LOG(DEBUG) << kApexBackupDir << " does not exist. Nothing to clean";
    return {};
  }
  return DeleteDirContent(std::string(kApexBackupDir));
}

Result<void> BackupActivePackages() {
  LOG(DEBUG) << "Initializing  backup of " << gConfig->active_apex_data_dir;

  // Previous restore might've delete backups folder.
  auto create_status = CreateDirIfNeeded(kApexBackupDir, 0700);
  if (!create_status.ok()) {
    return Error() << "Backup failed : " << create_status.error();
  }

  auto apex_active_exists =
      PathExists(std::string(gConfig->active_apex_data_dir));
  if (!apex_active_exists.ok()) {
    return Error() << "Backup failed : " << apex_active_exists.error();
  }
  if (!*apex_active_exists) {
    LOG(DEBUG) << gConfig->active_apex_data_dir
               << " does not exist. Nothing to backup";
    return {};
  }

  auto active_packages =
      FindFilesBySuffix(gConfig->active_apex_data_dir, {kApexPackageSuffix});
  if (!active_packages.ok()) {
    return Error() << "Backup failed : " << active_packages.error();
  }

  auto cleanup_status = DeleteBackup();
  if (!cleanup_status.ok()) {
    return Error() << "Backup failed : " << cleanup_status.error();
  }

  auto backup_path_fn = [](const ApexFile& apex_file) {
    return StringPrintf("%s/%s%s", kApexBackupDir,
                        GetPackageId(apex_file.GetManifest()).c_str(),
                        kApexPackageSuffix);
  };

  auto deleter = []() {
    auto result = DeleteDirContent(std::string(kApexBackupDir));
    if (!result.ok()) {
      LOG(ERROR) << "Failed to cleanup " << kApexBackupDir << " : "
                 << result.error();
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  for (const std::string& path : *active_packages) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return Error() << "Backup failed : " << apex_file.error();
    }
    const auto& dest_path = backup_path_fn(*apex_file);
    if (link(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
      return ErrnoError() << "Failed to backup " << apex_file->GetPath();
    }
  }

  scope_guard.Disable();  // Accept the backup.
  return {};
}

Result<void> RestoreActivePackages() {
  LOG(DEBUG) << "Initializing  restore of " << gConfig->active_apex_data_dir;

  auto backup_exists = PathExists(std::string(kApexBackupDir));
  if (!backup_exists.ok()) {
    return backup_exists.error();
  }
  if (!*backup_exists) {
    return Error() << kApexBackupDir << " does not exist";
  }

  struct stat stat_data;
  if (stat(gConfig->active_apex_data_dir, &stat_data) != 0) {
    return ErrnoError() << "Failed to access " << gConfig->active_apex_data_dir;
  }

  LOG(DEBUG) << "Deleting existing packages in "
             << gConfig->active_apex_data_dir;
  auto delete_status =
      DeleteDirContent(std::string(gConfig->active_apex_data_dir));
  if (!delete_status.ok()) {
    return delete_status;
  }

  LOG(DEBUG) << "Renaming " << kApexBackupDir << " to "
             << gConfig->active_apex_data_dir;
  if (rename(kApexBackupDir, gConfig->active_apex_data_dir) != 0) {
    return ErrnoError() << "Failed to rename " << kApexBackupDir << " to "
                        << gConfig->active_apex_data_dir;
  }

  LOG(DEBUG) << "Restoring original permissions for "
             << gConfig->active_apex_data_dir;
  if (chmod(gConfig->active_apex_data_dir, stat_data.st_mode & ALLPERMS) != 0) {
    return ErrnoError() << "Failed to restore original permissions for "
                        << gConfig->active_apex_data_dir;
  }

  return {};
}

Result<void> UnmountPackage(const ApexFile& apex, bool deferred,
                            bool detach_mount_point) {
  LOG(INFO) << "Unmounting " << GetPackageId(apex.GetManifest())
            << " deferred : " << deferred
            << " detach_mount_point : " << detach_mount_point;

  const ApexManifest& manifest = apex.GetManifest();

  std::optional<MountedApexData> data;
  bool latest = false;

  auto fn = [&](const MountedApexData& d, bool l) {
    if (d.full_path == apex.GetPath()) {
      data.emplace(d);
      latest = l;
    }
  };
  gMountedApexes.ForallMountedApexes(manifest.name(), fn);

  if (!data) {
    return Error() << "Did not find " << apex.GetPath();
  }

  if (latest) {
    std::string mount_point = apexd_private::GetActiveMountPoint(manifest);
    LOG(INFO) << "Unmounting " << mount_point;
    int flags = UMOUNT_NOFOLLOW;
    if (detach_mount_point) {
      flags |= MNT_DETACH;
    }
    if (umount2(mount_point.c_str(), flags) != 0) {
      auto err = errno;
      // Invoke apexd-lsof for better debugging on "Device or resource busy"
      if (err == EBUSY && base::GetBoolProperty("ro.debuggable", false)) {
        LOG(WARNING) << mount_point << " is busy. See apexd-lsof logs.";
        if (!SetProperty("apexd.debug.lsof", mount_point)) {
          LOG(ERROR) << "Failed to invoke apexd-lsof";
        }
      }
      return Error(err) << "Failed to unmount " << mount_point;
    }

    if (!deferred) {
      if (rmdir(mount_point.c_str()) != 0) {
        PLOG(ERROR) << "Failed to rmdir " << mount_point;
      }
    }
  }

  // Clean up gMountedApexes now, even though we're not fully done.
  gMountedApexes.RemoveMountedApex(manifest.name(), apex.GetPath());
  return Unmount(*data, deferred);
}

}  // namespace

void SetConfig(const ApexdConfig& config) { gConfig = config; }

const ApexdConfig& GetConfig() {
  CHECK(gConfig.has_value()) << "Call SetConfig() first";
  return *gConfig;
}

Result<void> MountPackage(const ApexFile& apex, const std::string& mount_point,
                          int32_t loop_id, const std::string& device_name,
                          bool reuse_device) {
  auto ret = MountPackageImpl(apex, mount_point, loop_id, device_name,
                              /* verify_image = */ false, reuse_device);
  if (!ret.ok()) {
    return ret.error();
  }

  gMountedApexes.AddMountedApex(apex.GetManifest().name(), *ret);
  return {};
}

namespace apexd_private {

Result<void> CheckBundledPublicKeyMatchesPreinstalled(const ApexFile& apex) {
  const auto& name = apex.GetManifest().name();
  // Check if the bundled key matches the preinstalled one.
  auto preinstalled =
      ApexFileRepository::GetInstance().GetPreInstalledApex(name);
  if (preinstalled.has_value()) {
    if (preinstalled->get().GetBundledPublicKey() ==
        apex.GetBundledPublicKey()) {
      return {};
    }
    return Error() << "public key doesn't match the pre-installed one";
  }
  if (ApexFileRepository::IsBrandNewApexEnabled()) {
    if (VerifyBrandNewPackageAgainstPreinstalled(apex).ok()) {
      return {};
    }
  }
  return Error() << "No preinstalled apex found for unverified package "
                 << name;
}

bool IsMounted(const std::string& full_path) {
  bool found_mounted = false;
  gMountedApexes.ForallMountedApexes([&](const std::string&,
                                         const MountedApexData& data,
                                         [[maybe_unused]] bool latest) {
    if (full_path == data.full_path) {
      found_mounted = true;
    }
  });
  return found_mounted;
}

std::string GetPackageMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, GetPackageId(manifest).c_str());
}

std::string GetPackageTempMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s.tmp", GetPackageMountPoint(manifest).c_str());
}

std::string GetActiveMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, manifest.name().c_str());
}

}  // namespace apexd_private

Result<void> ResumeRevertIfNeeded() {
  auto sessions =
      gSessionManager->GetSessionsInState(SessionState::REVERT_IN_PROGRESS);
  if (sessions.empty()) {
    return {};
  }
  return RevertActiveSessions("", "");
}

// Activates given APEX file.
//
// In a nutshel activation of an APEX consist of the following steps:
//   1. Create loop devices that is backed by the given apex_file
//   2. If apex_file resides on /data partition then create a dm-verity device
//    backed by the loop device created in step (1).
//   3. Create a mount point under /apex for this APEX.
//   4. Mount the dm-verity device on that mount point.
//     4.1 In case APEX file comes from a partition that is already
//       dm-verity protected (e.g. /system) then we mount the loop device.

Result<void> ActivatePackageImpl(const ApexFile& apex_file, int32_t loop_id,
                                 const std::string& device_name,
                                 bool reuse_device) {
  ATRACE_NAME("ActivatePackageImpl");

  // Validate upgraded shim apex
  if (shim::IsShimApex(apex_file) &&
      !ApexFileRepository::GetInstance().IsPreInstalledApex(apex_file)) {
    // This is not cheap for shim apex, but it is fine here since we have
    // upgraded shim apex only during CTS tests.
    Result<void> result = VerifyPackageBoot(apex_file);
    if (!result.ok()) {
      LOG(ERROR) << "Failed to validate shim apex: " << apex_file.GetPath();
      return result;
    }
  }

  // See whether we think it's active, and do not allow to activate the same
  // version. Also detect whether this is the highest version.
  // We roll this into a single check.
  const ApexManifest& manifest = apex_file.GetManifest();
  bool version_found_mounted = false;
  {
    int64_t new_version = manifest.version();
    bool version_found_active = false;
    gMountedApexes.ForallMountedApexes(
        manifest.name(), [&](const MountedApexData& data, bool latest) {
          if (data.version == new_version) {
            version_found_mounted = true;
            version_found_active = latest;
          }
        });
    if (version_found_active) {
      LOG(DEBUG) << "Package " << manifest.name() << " with version "
                 << manifest.version() << " already active";
      return {};
    }
  }

  const std::string& mount_point =
      apexd_private::GetPackageMountPoint(manifest);

  if (!version_found_mounted) {
    auto mount_status = MountPackage(apex_file, mount_point, loop_id,
                                     device_name, reuse_device);
    if (!mount_status.ok()) {
      return mount_status;
    }
  }

  // Bind mount the latest version to /apex/<package_name>.
  auto st = gMountedApexes.DoIfLatest(
      manifest.name(), apex_file.GetPath(), [&]() -> Result<void> {
        return apexd_private::BindMount(
            apexd_private::GetActiveMountPoint(manifest), mount_point);
      });
  if (!st.ok()) {
    return Error() << "Failed to update package " << manifest.name()
                   << " to version " << manifest.version() << " : "
                   << st.error();
  }

  LOG(DEBUG) << "Successfully activated " << apex_file.GetPath()
             << " package_name: " << manifest.name()
             << " version: " << manifest.version();
  return {};
}

// Wrapper around ActivatePackageImpl.
// Do not use, this wrapper is going away.
Result<void> ActivatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to activate " << full_path;

  Result<ApexFile> apex_file = ApexFile::Open(full_path);
  if (!apex_file.ok()) {
    return apex_file.error();
  }
  return ActivatePackageImpl(*apex_file, loop::kFreeLoopId,
                             GetPackageId(apex_file->GetManifest()),
                             /* reuse_device= */ false);
}

Result<void> DeactivatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to deactivate " << full_path;

  Result<ApexFile> apex_file = ApexFile::Open(full_path);
  if (!apex_file.ok()) {
    return apex_file.error();
  }

  return UnmountPackage(*apex_file,
                        /*deferred=*/false, /*detach_mount_point=*/false);
}

Result<std::vector<std::string>> ScanApexFilesInSessionDirs(
    int session_id, const std::vector<int>& child_session_ids) {
  std::vector<int> ids_to_scan;
  if (!child_session_ids.empty()) {
    ids_to_scan = child_session_ids;
  } else {
    ids_to_scan = {session_id};
  }

  // Find apex files in the staging directory
  std::vector<std::string> apex_file_paths;
  for (int id_to_scan : ids_to_scan) {
    std::string session_dir_path = std::string(gConfig->staged_session_dir) +
                                   "/session_" + std::to_string(id_to_scan);
    Result<std::vector<std::string>> scan =
        FindFilesBySuffix(session_dir_path, {kApexPackageSuffix});
    if (!scan.ok()) {
      return scan.error();
    }
    if (scan->size() != 1) {
      return Error() << "Expected exactly one APEX file in directory "
                     << session_dir_path << ". Found: " << scan->size();
    }
    std::string& apex_file_path = (*scan)[0];
    apex_file_paths.push_back(std::move(apex_file_path));
  }
  return apex_file_paths;
}

Result<std::vector<std::string>> ScanSessionApexFiles(
    const ApexSession& session) {
  auto child_session_ids =
      std::vector{std::from_range, session.GetChildSessionIds()};
  return ScanApexFilesInSessionDirs(session.GetId(), child_session_ids);
}

Result<std::vector<ApexFile>> OpenApexFilesInSessionDirs(
    int session_id, const std::vector<int>& child_session_ids) {
  auto apex_file_paths =
      OR_RETURN(ScanApexFilesInSessionDirs(session_id, child_session_ids));
  return OpenApexFiles(apex_file_paths);
}

Result<std::vector<ApexFile>> GetStagedApexFiles(
    int session_id, const std::vector<int>& child_session_ids) {
  // We should only accept sessions in SessionState::STAGED state
  auto session = OR_RETURN(gSessionManager->GetSession(session_id));
  if (session.GetState() != SessionState::STAGED) {
    return Error() << "Session " << session_id << " is not in state STAGED";
  }

  return OpenApexFilesInSessionDirs(session_id, child_session_ids);
}

Result<ClassPath> MountAndDeriveClassPath(
    const std::vector<ApexFile>& apex_files) {
  // Calculate classpaths of temp mounted staged apexs
  return RunVerifyFnInsideTempMounts(apex_files, [](const auto& mount_points) {
    return ClassPath::DeriveClassPath(mount_points);
  });
}

std::vector<ApexFile> GetActivePackages() {
  std::vector<ApexFile> ret;
  gMountedApexes.ForallMountedApexes(
      [&](const std::string&, const MountedApexData& data, bool latest) {
        if (!latest) {
          return;
        }

        Result<ApexFile> apex_file = ApexFile::Open(data.full_path);
        if (!apex_file.ok()) {
          return;
        }
        ret.emplace_back(std::move(*apex_file));
      });

  return ret;
}

std::vector<ApexFileRef> CalculateInactivePackages(
    const std::vector<ApexFileRef>& active_apexes) {
  std::set<std::string> active_preinstalled_names;
  auto& repo = ApexFileRepository::GetInstance();
  for (const auto& apex : active_apexes) {
    if (repo.IsPreInstalledApex(apex)) {
      active_preinstalled_names.insert(apex.get().GetManifest().name());
    }
  }

  std::vector<ApexFileRef> inactive = repo.GetPreInstalledApexFiles();
  auto new_end = std::remove_if(
      inactive.begin(), inactive.end(), [&](const ApexFile& apex) {
        return active_preinstalled_names.contains(apex.GetManifest().name());
      });
  inactive.erase(new_end, inactive.end());
  return inactive;
}

void EmitApexInfoList(const std::vector<ApexFileRef>& active,
                      bool is_bootstrap) {
  std::vector<ApexFileRef> inactive;
  // we skip for non-activated built-in apexes in bootstrap mode
  // in order to avoid boottime increase
  if (IsMountBeforeDataEnabled() || !is_bootstrap) {
    inactive = CalculateInactivePackages(active);
  }

  std::stringstream xml;
  CollectApexInfoList(xml, active, inactive);

  unique_fd fd(TEMP_FAILURE_RETRY(
      open(kApexInfoList, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));
  if (fd.get() == -1) {
    PLOG(ERROR) << "Can't open " << kApexInfoList;
    return;
  }
  if (!android::base::WriteStringToFd(xml.str(), fd)) {
    PLOG(ERROR) << "Can't write to " << kApexInfoList;
  }
}

std::vector<ApexFile> GetFactoryPackages() {
  std::vector<ApexFile> ret;

  // Decompressed APEX is considered factory package
  std::vector<std::string> decompressed_pkg_names;
  auto active_pkgs = GetActivePackages();
  for (ApexFile& apex : active_pkgs) {
    if (ApexFileRepository::GetInstance().IsDecompressedApex(apex)) {
      decompressed_pkg_names.push_back(apex.GetManifest().name());
      ret.emplace_back(std::move(apex));
    }
  }

  const auto& file_repository = ApexFileRepository::GetInstance();
  for (const auto& ref : file_repository.GetPreInstalledApexFiles()) {
    Result<ApexFile> apex_file = ApexFile::Open(ref.get().GetPath());
    if (!apex_file.ok()) {
      LOG(ERROR) << apex_file.error();
      continue;
    }
    // Ignore compressed APEX if it has been decompressed already
    if (apex_file->IsCompressed() &&
        std::find(decompressed_pkg_names.begin(), decompressed_pkg_names.end(),
                  apex_file->GetManifest().name()) !=
            decompressed_pkg_names.end()) {
      continue;
    }

    ret.emplace_back(std::move(*apex_file));
  }
  return ret;
}

/**
 * Abort individual staged session.
 *
 * Returns without error only if session was successfully aborted.
 **/
Result<void> AbortStagedSession(int session_id) REQUIRES(!gInstallLock) {
  auto install_guard = std::scoped_lock{gInstallLock};
  auto session = gSessionManager->GetSession(session_id);
  if (!session.ok()) {
    return Error() << "No session found with id " << session_id;
  }

  switch (session->GetState()) {
    case SessionState::VERIFIED:
      [[fallthrough]];
    case SessionState::STAGED:
      if (IsMountBeforeDataEnabled()) {
        for (const auto& image : session->GetApexImages()) {
          auto result = GetImageManager()->DeleteImage(image);
          if (!result.ok()) {
            // There's not much we can do with error. Let's log it. On boot
            // completion, dangling images (not referenced by any) will be
            // deleted anyway.
            LOG(ERROR) << result.error();
          }
        }
      }
      return session->DeleteSession();
    default:
      return Error() << "Session " << *session << " can't be aborted";
  }
}

namespace {

enum ActivationMode { kBootstrapMode = 0, kBootMode, kOtaChrootMode, kVmMode };

Result<void> ActivateApex(const ApexFile& apex, ActivationMode mode,
                          size_t index) {
  ATRACE_NAME("ActivateApex");
  int32_t loop_id = loop::kFreeLoopId;
  if (mode == ActivationMode::kBootstrapMode) {
    // Bootstrap mode needs to be very fast in a normal situation (no errors).
    // Creating a loop device can be faster by specifying an ID. Since this is a
    // bootstrap mode, we can assume that the range of indexes [0..) are free.
    loop_id = static_cast<int32_t>(index);
  }
  std::string device_name;
  if (mode == ActivationMode::kBootMode) {
    device_name = apex.GetManifest().name();
  } else {
    device_name = GetPackageId(apex.GetManifest());
  }
  if (mode == ActivationMode::kOtaChrootMode) {
    device_name += ".chroot";
  }
  bool reuse_device = mode == ActivationMode::kBootMode;
  return ActivatePackageImpl(apex, loop_id, device_name, reuse_device);
}

struct ActivationContext {
  // APEXes for which a different version was activated than in the previous
  // boot. This can happen in the following scenarios:
  //   1. This APEX is part of the staged session that was applied during this
  //   boot.
  //   2. We failed to activate APEX from /data/apex/active and fallback to the
  //   pre-installed APEX.
  std::set<std::string> changed_active_apexes;

  std::unordered_map<std::string, ApexFile> decompressed_apex_store;
  // Wrapper to ProcessCompressedApex to keep the ApexFile object in the store
  Result<ApexFileRef> DecompressApex(const ApexFile& capex,
                                     bool is_ota_chroot) {
    auto name = capex.GetManifest().name();
    auto it = decompressed_apex_store.find(name);
    if (it != decompressed_apex_store.end()) {
      return std::cref(it->second);
    }
    auto decompressed = OR_RETURN(ProcessCompressedApex(capex, is_ota_chroot));
    auto pair = decompressed_apex_store.emplace(name, std::move(decompressed));
    return std::cref(pair.first->second);
  }
};

// Custom result type for ActivateApexPackages()
struct ActivationResult {
  std::vector<ApexFileRef> activated;
  std::vector<ApexFileRef> failed;
  std::string error_message;

  bool ok() const { return failed.empty(); }
  const std::string& error() const { return error_message; }
};

// In case some of the apexes failed to activate, we will attempt to activate
// their corresponding pre-installed copies.
std::vector<ApexFileRef> GetFallbackApexes(
    const std::vector<ApexFileRef>& failed) {
  LOG(INFO) << "Trying to activate pre-installed versions of missing apexes";
  const auto& file_repository = ApexFileRepository::GetInstance();
  std::vector<ApexFileRef> fallback_apexes;
  for (const auto& apex : failed) {
    if (file_repository.IsPreInstalledApex(apex)) {
      // We tried to activate pre-installed apex in the first place. No need
      // to try again.
      continue;
    }
    const std::string& name = apex.get().GetManifest().name();
    auto preinstalled = file_repository.GetPreInstalledApex(name);
    if (!preinstalled.has_value()) {
      // Not every apex has preinstalled.
      CHECK(ApexFileRepository::IsBrandNewApexEnabled() ||
            file_repository.IsBlockApex(apex))
          << "No preinstalled APEX found for " << name;
      continue;
    }
    fallback_apexes.push_back(preinstalled.value());
  }
  return fallback_apexes;
}

// @param revert_on_error if true, will try to revert all active sessions
//          and reboot the device in case of an error.
// @param fallback_on_error if true, will try to activate pre-installed
//          APEXes for failed activation and clean up the failed list.
ActivationResult ActivateApexPackages(ActivationContext& ctx,
                                      const std::vector<ApexFileRef>& apexes,
                                      ActivationMode mode, bool revert_on_error,
                                      bool fallback_on_error) {
  ATRACE_NAME("ActivateApexPackages");
  size_t apex_cnt = apexes.size();
  std::vector<Result<ApexFileRef>> results;
  results.reserve(apex_cnt);

  // Decompress compressed apexes, if any, only in supported modes.
  // TODO(b/179248390) do this in parallel
  bool compressed_apex_supported = mode == ActivationMode::kBootMode ||
                                   mode == ActivationMode::kOtaChrootMode;
  bool is_ota_chroot = mode == ActivationMode::kOtaChrootMode;
  for (const auto& apex : apexes) {
    if (apex.get().IsCompressed() && compressed_apex_supported) {
      results.push_back(ctx.DecompressApex(apex, is_ota_chroot));
    } else {
      results.push_back(apex);
    }
  }

  size_t worker_num =
      android::sysprop::ApexProperties::boot_activation_threads().value_or(0);
  // Setting number of workers to the number of packages to load
  // This seems to provide the best performance
  if (worker_num == 0) {
    worker_num = apex_cnt;
  } else {
    worker_num = std::min(apex_cnt, worker_num);
  }

  ForEachParallel(worker_num, 0uz, apex_cnt, [&](size_t index) {
    if (results[index].ok()) {
      auto status = ActivateApex(*results[index], mode, index);
      if (!status.ok()) {
        results[index] = status.error();
      }
    }
  });

  ActivationResult activation_result;
  for (size_t i = 0; i < apex_cnt; ++i) {
    auto& res = results[i];
    if (res.ok()) {
      activation_result.activated.push_back(*res);
    } else {
      LOG(ERROR) << res.error();
      activation_result.failed.push_back(apexes[i]);
      if (activation_result.failed.size() == 1) {
        activation_result.error_message = res.error().message();
      }
    }
  }
  LOG(INFO) << "Activated " << activation_result.activated.size()
            << " packages.";

  if (!activation_result.ok()) {
    std::string error_message = StringPrintf("Failed to activate packages: %s",
                                             activation_result.error().c_str());
    LOG(ERROR) << error_message;
    if (revert_on_error) {
      if (auto st = RevertActiveSessionsAndReboot("", error_message);
          !st.ok()) {
        LOG(ERROR) << "Failed to revert : " << st.error();
      }
    }
    if (fallback_on_error) {
      auto fallback_apexes = GetFallbackApexes(activation_result.failed);
      auto st = ActivateApexPackages(ctx, fallback_apexes, mode,
                                     /*revert_on_error=*/false,
                                     /*fallback_on_error=*/false);
      if (!st.ok()) {
        LOG(ERROR) << st.error();
      }
      // Treat fallback to pre-installed APEXes as a change of the active APEX,
      // since we are already in a pretty dire situation, so it's better if we
      // drop all the caches.
      for (const auto& apex : st.activated) {
        ctx.changed_active_apexes.insert(apex.get().GetManifest().name());
      }
      // Collect activated apex files and clear the failure.
      activation_result.activated.append_range(std::move(st.activated));
      activation_result.failed.clear();
    }
  }
  return activation_result;
}

}  // namespace

/**
 * Snapshots data from base_dir/apexdata/<apex name> to
 * base_dir/apexrollback/<rollback id>/<apex name>.
 */
Result<void> SnapshotDataDirectory(const std::string& base_dir,
                                   const int rollback_id,
                                   const std::string& apex_name,
                                   bool pre_restore = false) {
  auto rollback_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   rollback_id, pre_restore ? kPreRestoreSuffix : "");
  const Result<void> result = CreateDirIfNeeded(rollback_path, 0700);
  if (!result.ok()) {
    return Error() << "Failed to create snapshot directory for rollback "
                   << rollback_id << " : " << result.error();
  }
  auto from_path = StringPrintf("%s/%s/%s", base_dir.c_str(), kApexDataSubDir,
                                apex_name.c_str());
  auto to_path =
      StringPrintf("%s/%s", rollback_path.c_str(), apex_name.c_str());

  return ReplaceFiles(from_path, to_path);
}

/**
 * Restores snapshot from base_dir/apexrollback/<rollback id>/<apex name>
 * to base_dir/apexdata/<apex name>.
 * Note the snapshot will be deleted after restoration succeeded.
 */
Result<void> RestoreDataDirectory(const std::string& base_dir,
                                  const int rollback_id,
                                  const std::string& apex_name,
                                  bool pre_restore = false) {
  auto from_path = StringPrintf(
      "%s/%s/%d%s/%s", base_dir.c_str(), kApexSnapshotSubDir, rollback_id,
      pre_restore ? kPreRestoreSuffix : "", apex_name.c_str());
  auto to_path = StringPrintf("%s/%s/%s", base_dir.c_str(), kApexDataSubDir,
                              apex_name.c_str());
  Result<void> result = ReplaceFiles(from_path, to_path);
  if (!result.ok()) {
    return result;
  }
  result = RestoreconPath(to_path);
  if (!result.ok()) {
    return result;
  }
  result = DeleteDir(from_path);
  if (!result.ok()) {
    LOG(ERROR) << "Failed to delete the snapshot: " << result.error();
  }
  return {};
}

void SnapshotOrRestoreDeIfNeeded(const std::string& base_dir,
                                 const ApexSession& session) {
  if (session.HasRollbackEnabled()) {
    for (const auto& apex_name : session.GetApexNames()) {
      Result<void> result =
          SnapshotDataDirectory(base_dir, session.GetRollbackId(), apex_name);
      if (!result.ok()) {
        LOG(ERROR) << "Snapshot failed for " << apex_name << ": "
                   << result.error();
      }
    }
  } else if (session.IsRollback()) {
    for (const auto& apex_name : session.GetApexNames()) {
      if (!gSupportsFsCheckpoints) {
        // Snapshot before restore so this rollback can be reverted.
        SnapshotDataDirectory(base_dir, session.GetRollbackId(), apex_name,
                              true /* pre_restore */);
      }
      Result<void> result =
          RestoreDataDirectory(base_dir, session.GetRollbackId(), apex_name);
      if (!result.ok()) {
        LOG(ERROR) << "Restore of data failed for " << apex_name << ": "
                   << result.error();
      }
    }
  }
}

void SnapshotOrRestoreDeSysData() {
  auto sessions = gSessionManager->GetSessionsInState(SessionState::ACTIVATED);

  for (const ApexSession& session : sessions) {
    SnapshotOrRestoreDeIfNeeded(kDeSysDataDir, session);
  }
}

int SnapshotOrRestoreDeUserData() {
  auto user_dirs = GetDeUserDirs();

  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading dirs " << user_dirs.error();
    return 1;
  }

  auto sessions = gSessionManager->GetSessionsInState(SessionState::ACTIVATED);

  for (const ApexSession& session : sessions) {
    for (const auto& user_dir : *user_dirs) {
      SnapshotOrRestoreDeIfNeeded(user_dir, session);
    }
  }

  return 0;
}

Result<void> SnapshotCeData(const int user_id, const int rollback_id,
                            const std::string& apex_name) {
  auto base_dir = StringPrintf("%s/%d", kCeDataDir, user_id);
  return SnapshotDataDirectory(base_dir, rollback_id, apex_name);
}

Result<void> RestoreCeData(const int user_id, const int rollback_id,
                           const std::string& apex_name) {
  auto base_dir = StringPrintf("%s/%d", kCeDataDir, user_id);
  return RestoreDataDirectory(base_dir, rollback_id, apex_name);
}

Result<void> DestroySnapshots(const std::string& base_dir,
                              const int rollback_id) {
  auto path = StringPrintf("%s/%s/%d", base_dir.c_str(), kApexSnapshotSubDir,
                           rollback_id);
  return DeleteDir(path);
}

Result<void> DestroyDeSnapshots(const int rollback_id) {
  DestroySnapshots(kDeSysDataDir, rollback_id);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    return Error() << "Error reading user dirs " << user_dirs.error();
  }

  for (const auto& user_dir : *user_dirs) {
    DestroySnapshots(user_dir, rollback_id);
  }

  return {};
}

Result<void> DestroyCeSnapshots(const int user_id, const int rollback_id) {
  auto path = StringPrintf("%s/%d/%s/%d", kCeDataDir, user_id,
                           kApexSnapshotSubDir, rollback_id);
  return DeleteDir(path);
}

/**
 * Deletes all credential-encrypted snapshots for the given user, except for
 * those listed in retain_rollback_ids.
 */
Result<void> DestroyCeSnapshotsNotSpecified(
    int user_id, const std::vector<int>& retain_rollback_ids) {
  auto snapshot_root =
      StringPrintf("%s/%d/%s", kCeDataDir, user_id, kApexSnapshotSubDir);
  auto snapshot_dirs = GetSubdirs(snapshot_root);
  if (!snapshot_dirs.ok()) {
    return Error() << "Error reading snapshot dirs " << snapshot_dirs.error();
  }

  for (const auto& snapshot_dir : *snapshot_dirs) {
    uint snapshot_id;
    bool parse_ok = ParseUint(
        std::filesystem::path(snapshot_dir).filename().c_str(), &snapshot_id);
    if (parse_ok &&
        std::find(retain_rollback_ids.begin(), retain_rollback_ids.end(),
                  snapshot_id) == retain_rollback_ids.end()) {
      Result<void> result = DeleteDir(snapshot_dir);
      if (!result.ok()) {
        return Error() << "Destroy CE snapshot failed for " << snapshot_dir
                       << " : " << result.error();
      }
    }
  }
  return {};
}

void RestorePreRestoreSnapshotsIfPresent(const std::string& base_dir,
                                         const ApexSession& session) {
  auto pre_restore_snapshot_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   session.GetRollbackId(), kPreRestoreSuffix);
  if (auto st = PathExists(pre_restore_snapshot_path); st.ok() && st.value()) {
    for (const auto& apex_name : session.GetApexNames()) {
      Result<void> result = RestoreDataDirectory(
          base_dir, session.GetRollbackId(), apex_name, true /* pre_restore */);
      if (!result.ok()) {
        LOG(ERROR) << "Restore of pre-restore snapshot failed for " << apex_name
                   << ": " << result.error();
      }
    }
  }
}

void RestoreDePreRestoreSnapshotsIfPresent(const ApexSession& session) {
  RestorePreRestoreSnapshotsIfPresent(kDeSysDataDir, session);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading user dirs to restore pre-restore snapshots"
               << user_dirs.error();
    return;
  }

  for (const auto& user_dir : *user_dirs) {
    RestorePreRestoreSnapshotsIfPresent(user_dir, session);
  }
}

void DeleteDePreRestoreSnapshots(const std::string& base_dir,
                                 const ApexSession& session) {
  auto pre_restore_snapshot_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   session.GetRollbackId(), kPreRestoreSuffix);
  Result<void> result = DeleteDir(pre_restore_snapshot_path);
  if (!result.ok()) {
    LOG(ERROR) << "Deletion of pre-restore snapshot failed: " << result.error();
  }
}

void DeleteDePreRestoreSnapshots(const ApexSession& session) {
  DeleteDePreRestoreSnapshots(kDeSysDataDir, session);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading user dirs to delete pre-restore snapshots"
               << user_dirs.error();
    return;
  }

  for (const auto& user_dir : *user_dirs) {
    DeleteDePreRestoreSnapshots(user_dir, session);
  }
}

void MarkBootCompleted() { ApexdLifecycle::GetInstance().MarkBootCompleted(); }

// Moves all apexes in the session to "active" state in a transactional manner.
// Returns the name list of the apexes in the session on success.
Result<std::vector<std::string>> TryActivateStagedSession(
    const ApexSession& session) {
  // If device supports fs-checkpoint, then apex session should only be
  // installed when in checkpoint-mode. Otherwise, we will not be able to
  // revert /data on error.
  if (gSupportsFsCheckpoints && !gInFsCheckpointMode) {
    return Error()
           << "Cannot install apex session if not in fs-checkpoint mode";
  }

  if (IsMountBeforeDataEnabled()) {
    if (session.GetApexImages().empty()) {
      return Error() << "No apex found in session";
    }
    auto image_manager = GetImageManager();
    std::vector<std::string> images{std::from_range, session.GetApexImages()};

    auto unmap_devices = base::make_scope_guard([&]() {
      for (const auto& image : images) {
        auto unmap = image_manager->UnmapImageIfExists(image);
        if (!unmap.ok()) {
          LOG(ERROR) << unmap.error();
        }
      }
    });

    std::vector<std::string> apex_names_in_session;
    apex_names_in_session.reserve(images.size());
    for (const auto& image : images) {
      auto dm_device = OR_RETURN(image_manager->MapImage(image));
      auto apex_file = OR_RETURN(ApexFile::Open(dm_device));
      OR_RETURN(VerifyPackageBoot(apex_file));

      apex_names_in_session.push_back(apex_file.GetManifest().name());
    }

    std::vector<ApexListEntry> new_entries;
    new_entries.reserve(images.size());
    for (size_t i = 0; i < images.size(); i++) {
      new_entries.emplace_back(images[i], apex_names_in_session[i]);
    }
    // Now, update "active" list
    auto active_list =
        OR_RETURN(image_manager->GetApexList(ApexListType::ACTIVE));
    OR_RETURN(image_manager->UpdateApexList(
        ApexListType::ACTIVE,
        UpdateApexListWithNewEntries(std::move(active_list), new_entries)));

    // Let's keep mapped devices because they needs to be mapped as "active" in
    // ScanDataApexFiles().
    unmap_devices.Disable();
    return apex_names_in_session;
  } else {
    auto apexes = OR_RETURN(ScanSessionApexFiles(session));
    auto packages = StagePackagesImpl(apexes);
    if (!packages.ok()) {
      return Error() << "Activation failed for packages "
                     << base::Join(apexes, ", ") << ": " << packages.error();
    }
    return std::move(*packages);
  }
}

// Scans all STAGED sessions and activate them so that APEXes in those sessions
// become available for activation. Sessions are updated to be ACTIVATED state,
// or ACTIVATION_FAILED if something goes wrong.
// Note that this doesn't abort with failed sessions. Apexd just marks them as
// failed and continues activation process. It's higher level component (e.g.
// system_server) that needs to handle the failures.
void ActivateStagedSessions(ActivationContext& ctx,
                            std::vector<ApexSession>&& sessions) {
  auto fail = [](ApexSession& session, const std::string& message) {
    LOG(ERROR) << "Fail: session " << session.GetId() << ": " << message;
    session.SetErrorMessage(message);
    LOG(WARNING) << "Marking session " << session.GetId() << " as failed.";
    auto st = session.UpdateStateAndCommit(SessionState::ACTIVATION_FAILED);
    if (!st.ok()) {
      LOG(WARNING) << "Failed to mark session " << session.GetId()
                   << " as failed : " << st.error();
    }
  };

  // Sessions from the previous build fingerprint should be removed first.
  std::string build_fingerprint = GetProperty(kBuildFingerprintSysprop, "");
  for (auto& session : sessions) {
    if (session.GetBuildFingerprint().compare(build_fingerprint) != 0) {
      fail(session, "APEX build fingerprint has changed");
    }
  }

  std::vector<std::reference_wrapper<ApexSession>> sessions_to_activate;
  for (auto& session : sessions) {
    if (session.GetState() == SessionState::STAGED) {
      sessions_to_activate.push_back(std::ref(session));
    }
  }
  if (gSupportsFsCheckpoints) {
    // A session that is in the ACTIVATED state should still be re-activated if
    // fs checkpointing is supported. In this case, a session may be in the
    // ACTIVATED state yet the data/apex/active directory may have been
    // reverted. The session should be reverted in this scenario.
    for (auto& session : sessions) {
      if (session.GetState() == SessionState::ACTIVATED) {
        sessions_to_activate.push_back(std::ref(session));
      }
    }
  }

  LOG(INFO) << "Found " << sessions_to_activate.size()
            << " sessions to activate";

  for (ApexSession& session : sessions_to_activate) {
    auto session_id = session.GetId();
    auto packages = TryActivateStagedSession(session);
    if (!packages.ok()) {
      fail(session, packages.error().message());
      continue;
    }

    LOG(INFO) << "Session(" << session_id
              << ") is successfully activated: " << base::Join(*packages, ", ");
    ctx.changed_active_apexes.insert_range(*packages);

    auto st = session.UpdateStateAndCommit(SessionState::ACTIVATED);
    if (!st.ok()) {
      LOG(ERROR) << "Failed to mark " << session
                 << " as activated : " << st.error();
    }
  }
}

namespace {
std::string StageDestPath(const ApexFile& apex_file) {
  return StringPrintf("%s/%s%s", gConfig->active_apex_data_dir,
                      GetPackageId(apex_file.GetManifest()).c_str(),
                      kApexPackageSuffix);
}

}  // namespace

Result<std::vector<std::string>> StagePackagesImpl(
    const std::vector<std::string>& tmp_paths) {
  if (tmp_paths.empty()) {
    return Error() << "Empty set of inputs";
  }
  LOG(DEBUG) << "StagePackagesImpl() for " << Join(tmp_paths, ',');

  // Note: this function is temporary. As such the code is not optimized,
  // e.g.,
  //       it will open ApexFiles multiple times.

  // 1) Verify all packages.
  Result<std::vector<ApexFile>> apex_files = OpenApexFiles(tmp_paths);
  if (!apex_files.ok()) {
    return apex_files.error();
  }
  for (const ApexFile& apex_file : *apex_files) {
    if (shim::IsShimApex(apex_file)) {
      // Shim apex will be validated on every boot. No need to do it here.
      continue;
    }
    Result<void> result = VerifyPackageBoot(apex_file);
    if (!result.ok()) {
      return result.error();
    }
  }

  // Make sure that kActiveApexPackagesDataDir exists.
  auto create_dir_status =
      CreateDirIfNeeded(std::string(gConfig->active_apex_data_dir), 0755);
  if (!create_dir_status.ok()) {
    return create_dir_status.error();
  }

  // 2) Now stage all of them.

  // Ensure the APEX gets removed on failure.
  std::vector<std::string> staged_files;
  auto deleter = [&staged_files]() {
    for (const std::string& staged_path : staged_files) {
      if (TEMP_FAILURE_RETRY(unlink(staged_path.c_str())) != 0) {
        PLOG(ERROR) << "Unable to unlink " << staged_path;
      }
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  std::vector<std::string> staged_packages;
  for (const ApexFile& apex_file : *apex_files) {
    // move apex to /data/apex/active.
    std::string dest_path = StageDestPath(apex_file);
    if (access(dest_path.c_str(), F_OK) == 0) {
      LOG(DEBUG) << dest_path << " already exists. Deleting";
      if (TEMP_FAILURE_RETRY(unlink(dest_path.c_str())) != 0) {
        return ErrnoError() << "Failed to unlink " << dest_path;
      }
    }

    if (link(apex_file.GetPath().c_str(), dest_path.c_str()) != 0) {
      return ErrnoError() << "Unable to link " << apex_file.GetPath() << " to "
                          << dest_path;
    }
    staged_files.push_back(dest_path);
    staged_packages.push_back(apex_file.GetManifest().name());

    LOG(DEBUG) << "Success linking " << apex_file.GetPath() << " to "
               << dest_path;
  }

  scope_guard.Disable();  // Accept the state.

  OR_RETURN(RemovePreviouslyActiveApexFiles(staged_packages, staged_files));

  return staged_packages;
}

Result<void> StagePackages(const std::vector<std::string>& tmp_paths) {
  OR_RETURN(StagePackagesImpl(tmp_paths));
  return {};
}

Result<void> UnstagePackages(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Error() << "Empty set of inputs";
  }
  LOG(DEBUG) << "UnstagePackages() for " << Join(paths, ',');

  std::vector<ApexFile> apex_files;
  // Ensure the input paths are APEX files, but not pre-installed.
  for (const std::string& path : paths) {
    auto apex = ApexFile::Open(path);
    if (!apex.ok()) {
      return apex.error();
    }
    if (ApexFileRepository::GetInstance().IsPreInstalledApex(*apex)) {
      return Error() << "Can't uninstall pre-installed apex " << path;
    }
    apex_files.emplace_back(std::move(*apex));
  }

  // For now, UnstagePackages() is only for tests and callers should call
  // reboot() immediately.
  // TODO(b/384040968) Implement a proper "uninstall". Until then, we just
  // unlink/remove the input APEX paths.
  if (IsMountBeforeDataEnabled()) {
    // Removing image names from the ACTIVE list is enough. After reboot, the
    // actual images will be removed as part of boot-completion cleanup.
    auto image_manager = GetImageManager();
    auto active_list =
        OR_RETURN(image_manager->GetApexList(ApexListType::ACTIVE));
    for (const auto& apex_file : apex_files) {
      auto image = image_manager->FindPinnedApex(apex_file);
      if (!image) {
        return Error() << "Can't uninstall: image not found: "
                       << apex_file.GetPath();
      }
      std::erase_if(active_list, [&](const auto& entry) {
        return entry.image_name == *image;
      });
    }
    OR_RETURN(image_manager->UpdateApexList(ApexListType::ACTIVE, active_list));
  } else {
    for (const std::string& path : paths) {
      if (unlink(path.c_str()) != 0) {
        return ErrnoError() << "Can't unlink " << path;
      }
    }
  }

  return {};
}

void MarkSessions(std::vector<ApexSession>& sessions,
                  SessionState::State state) {
  for (auto& session : sessions) {
    auto st = session.UpdateStateAndCommit(state);
    LOG(DEBUG) << "Marking " << session << " as "
               << SessionState_State_Name(state);
    if (!st.ok()) {
      LOG(WARNING) << "Failed to mark session " << session << " as "
                   << SessionState_State_Name(state) << ": " << st.error();
    }
  }
}

/**
 * During apex installation, staged sessions located in
 * /metadata/apex/sessions mutate the active set of APEXes. If some error occurs
 * during installation, we need to revert the active set of APEXes to its
 * original state and reboot.
 *
 * For example, if the active set of APEXes are kept in /data/apex/active, the
 * directory should be backed up on staging, and restored here. In case FS
 * checkpointing is supported, the backup/restore can be skipped because
 * reboot will restore the /data partition to the original state.
 *
 * With mount_before_data, the /data partition is not changed during
 * installation. Instead, the list of active APEXes is kept in /metadata/apex.
 * The list should be backed up on staging, and restored here. FS checkpointing
 * doesn't matter.
 *
 * Also, we need to put staged sessions in /metadata/apex/sessions in
 * REVERTED state so that they do not get activated on next reboot.
 */
Result<void> RevertActiveSessions(const std::string& crashing_native_process,
                                  const std::string& error_message) {
  // First check whenever there is anything to revert. If there is none, then
  // fail. This prevents apexd from boot looping a device in case a native
  // process is crashing and there are no apex updates.
  auto active_sessions = gSessionManager->GetSessions();
  active_sessions.erase(
      std::remove_if(active_sessions.begin(), active_sessions.end(),
                     [](const auto& s) {
                       return s.IsFinalized() ||
                              s.GetState() == SessionState::UNKNOWN;
                     }),
      active_sessions.end());
  if (active_sessions.empty()) {
    return Error() << "Revert requested, when there are no active sessions.";
  }

  // Before proceeding the actual revert, let's mark active sessions as
  // REVERT_IN_PROGRESS so that even if the revert fails we can resume revert
  // on next reboot and avoid reapplying the active sessions.
  for (auto& session : active_sessions) {
    if (!crashing_native_process.empty()) {
      session.SetCrashingNativeProcess(crashing_native_process);
    }
    if (!error_message.empty()) {
      session.SetErrorMessage(error_message);
    }
    auto status =
        session.UpdateStateAndCommit(SessionState::REVERT_IN_PROGRESS);
    if (!status.ok()) {
      return Error() << "Revert of session " << session
                     << " failed : " << status.error();
    }
  }

  // Revert the active set of APEXes now!

  if (IsMountBeforeDataEnabled()) {
    auto st = GetImageManager()->RestoreApexList();
    if (!st.ok()) {
      MarkSessions(active_sessions, SessionState::REVERT_FAILED);
      return st;
    }
  } else if (!gSupportsFsCheckpoints) {
    auto st = RestoreActivePackages();
    if (!st.ok()) {
      MarkSessions(active_sessions, SessionState::REVERT_FAILED);
      return st;
    }
  } else {
    LOG(INFO) << "Not restoring active packages in checkpoint mode.";
  }

  // Installing a rollback means restoring the apexdata (DE_sys/DE_n) as well.
  // In case of reverting a rollback, the restored apexdata should be reverted.
  // This is done automatically for devices with FS checkpointing. Otherwise,
  // the apexdata should be manually snapshotted (aka, pre-restore snapshot),
  // and restored when reverting.
  //
  // In case of mount-before-data, RevertActiveSessions() can be invoked in
  // either apexd-bootstrap (before /data) or apexd (after /data). apexd works
  // fine for both cases: When it's called in apexd-bootstrap, the apexdata is
  // not restored yet, hence nothing to revert. When it's called in apexd, it
  // works just as expected. Btw, RestoreDePreRestoreSnapshotsIfPresent() will
  // emit some error messages (with no harm) when it's called during
  // apexd-bootstrap because there's no /data yet.

  for (auto& session : active_sessions) {
    if (!gSupportsFsCheckpoints && session.IsRollback()) {
      // If snapshots have already been restored, undo that by restoring the
      // pre-restore snapshot.
      RestoreDePreRestoreSnapshotsIfPresent(session);
    }
  }

  MarkSessions(active_sessions, SessionState::REVERTED);

  return {};
}

Result<void> RevertActiveSessionsAndReboot(
    const std::string& crashing_native_process,
    const std::string& error_message) {
  auto status = RevertActiveSessions(crashing_native_process, error_message);
  if (!status.ok()) {
    return status;
  }
  LOG(ERROR) << "Successfully reverted. Time to reboot device.";

  // Before reboot, need to abort the checkpoint mode if it is.

  // In case `vold` service is available, use it.
  if (gVoldService) {
    // If the device is in FS checkpoint mode, let's abort it and reboot so that
    // the device to be in "needsRollback" mode.
    if (gInFsCheckpointMode) {
      auto result = gVoldService->AbortChanges(/*message=*/"apexd_initiated",
                                               /*retry=*/false);
      if (!result.ok()) {
        LOG(ERROR) << result.error();
      }
    }
  } else if (IsMountBeforeDataEnabled()) {
    // This is the case when apexd-bootstrap fails to activate new APEXes.
    // Even if the filesystem supports checkpointing and the device is in the
    // checkpoint mode, apexd-bootstrap can't delegate "abortChanges" to vold
    // because vold hasn't started. apexd-bootstrap must therefore perform it
    // on its own.
    auto result = AbortChanges();
    if (!result.ok()) {
      LOG(ERROR) << "Failed to abort checkpoint: " << result.error();
    }
  }

  Reboot();
  return {};
}

void PrepareResources(size_t loop_device_cnt,
                      const std::vector<std::string>& apex_names) {
  LOG(INFO) << "Need to pre-allocate " << loop_device_cnt << " loop devices";
  if (auto res = loop::PreAllocateLoopDevices(loop_device_cnt); !res.ok()) {
    LOG(ERROR) << "Failed to pre-allocate loop devices : " << res.error();
  }

  DeviceMapper& dm = DeviceMapper::Instance();
  // Create empty dm device for each found APEX.
  // This is a boot time optimization that makes use of the fact that user
  // space paths will be created by ueventd before apexd is started, and hence
  // reducing the time to activate APEXes on /data.
  // Note: since at this point we don't know which APEXes are updated, we are
  // optimistically creating a verity device for all of them. Once boot
  // finishes, apexd will clean up unused devices.
  // TODO(b/192241176): move to apexd_verity.{h,cpp}
  for (const auto& name : apex_names) {
    if (!dm.CreatePlaceholderDevice(name)) {
      LOG(ERROR) << "Failed to create empty device " << name;
    }
  }
}

// Note that this needs to be called before scanning data apexes because
// revert or activation may change the active set of data apexes. For example,
// revert restores the active apexes from the last backup.
void ProcessSessions(ActivationContext& ctx) {
  auto sessions = gSessionManager->GetSessions();

  if (sessions.empty()) {
    LOG(INFO) << "No sessions to revert/activate.";
    return;
  }

  // If there's any pending revert, revert active sessions.
  if (std::ranges::any_of(sessions, [](const auto& session) {
        return session.GetState() == SessionState::REVERT_IN_PROGRESS;
      })) {
    if (auto status = RevertActiveSessions("", ""); !status.ok()) {
      LOG(ERROR) << "Failed to resume revert : " << status.error();
    }
  } else {
    // Otherwise, activate STAGED sessions.
    ActivateStagedSessions(ctx, std::move(sessions));
  }
}

std::vector<ApexFile> ScanDataApexFiles(ApexImageManager* manager) {
  CHECK(IsMountBeforeDataEnabled());
  auto image_list = manager->GetApexList(ApexListType::ACTIVE);
  if (!image_list.ok()) {
    LOG(ERROR) << "Failed to get active image list : " << image_list.error();
    return {};
  }
  std::vector<ApexFile> apex_files;
  apex_files.reserve(image_list->size());
  for (const auto& entry : *image_list) {
    auto path = manager->MapImage(entry.image_name);
    // Log error and keep searching for active apexes
    if (!path.ok()) {
      LOG(ERROR) << "Skip " << entry.image_name << ": " << path.error();
      continue;
    }
    auto apex_file = ApexFile::Open(*path);
    if (!apex_file.ok()) {
      manager->UnmapImage(entry.image_name);
      LOG(ERROR) << "Skip " << entry.image_name << ": " << apex_file.error();
      continue;
    }
    apex_files.push_back(std::move(*apex_file));
  }
  return apex_files;
}

Result<void> AddPreinstalledData(ApexFileRepository& instance) {
  if (auto status = instance.AddPreInstalledApex(gConfig->builtin_dirs);
      !status.ok()) {
    return Error() << "Failed to collect pre-installed APEX files: "
                   << status.error();
  }

  if (ApexFileRepository::IsBrandNewApexEnabled()) {
    if (auto status = instance.AddBrandNewApexCredentialAndBlocklist(
            gConfig->brand_new_apex_config_dirs);
        !status.ok()) {
      return Error() << "Failed to collect pre-installed public keys and "
                        "blocklists for brand-new APEX: "
                     << status.error();
    }
  }
  return {};
}

int OnBootstrap() {
  ATRACE_NAME("OnBootstrap");
  auto time_started = boot_clock::now();

  ApexFileRepository& instance = ApexFileRepository::GetInstance();
  if (auto st = AddPreinstalledData(instance); !st.ok()) {
    LOG(ERROR) << st.error();
    return 1;
  }

  ActivationContext ctx;
  std::vector<ApexFileRef> activation_list;
  bool fallback_on_error = false;
  bool revert_on_error = false;

  if (IsMountBeforeDataEnabled()) {
    // Wait until coldboot is done. This is to avoid unnecessary polling when
    // using/creating loop or device-mapper devices. Note that apexd relies on
    // devices created by init process for faster activation. Their nodes are
    // created by ueventd's coldboot. Hence, accessing them before coldboot is
    // done causes polling, which can be much slower than waiting for coldboot.
    // Similarly, before coldboot is done, ueventd can't handle a device
    // creation. This will also cause polling the userspace node creation.
    // Instead of racing with ueventd, let's wait until it finishes coldboot.
    base::WaitForProperty("ro.cold_boot_done", "true",
                          std::chrono::seconds(10));

    // Process sessions before scanning "active" data apexes because sessions
    // can change the list of active data apexes:
    // - if there's a pending revert, then reverts all active sessions.
    // - if there's staged sessions, then activate them first.
    ProcessSessions(ctx);
    auto data_apexes = ScanDataApexFiles(GetImageManager());
    instance.AddDataApexFiles(std::move(data_apexes));
    activation_list = instance.SelectApexForActivation();

    // When APEX activation fails with staged sessions, the device should abort
    // the staged sessions by marking the sessions as failed and rebooting the
    // device.
    revert_on_error = true;
    // When the revert fails, there's nothing much we can do. Hence, we fallback
    // to activating the preinstalled APEXes so that the device can still
    // boot. This is a last resort, and should be used only when the revert
    // fails. This is not a common case, but can happen if the device is
    // corrupted or the revert is not implemented correctly.
    fallback_on_error = true;
  } else {
    const auto& pre_installed_apexes = instance.GetPreInstalledApexFiles();
    size_t loop_device_cnt = pre_installed_apexes.size();
    std::vector<std::string> apex_names;
    apex_names.reserve(loop_device_cnt);
    // Find all bootstrap apexes
    for (const auto& apex : pre_installed_apexes) {
      apex_names.push_back(apex.get().GetManifest().name());
      if (IsBootstrapApex(apex.get())) {
        LOG(INFO) << "Found bootstrap APEX " << apex.get().GetPath();
        activation_list.push_back(apex);
        loop_device_cnt++;
      }
    }
    PrepareResources(loop_device_cnt, apex_names);
  }

  auto result =
      ActivateApexPackages(ctx, activation_list, ActivationMode::kBootstrapMode,
                           revert_on_error, fallback_on_error);
  if (!result.ok()) {
    LOG(ERROR) << "Failed to activate apexes: " << result.error();
    return 1;
  }
  EmitApexInfoList(result.activated, /*is_bootstrap=*/true);
  if (IsMountBeforeDataEnabled()) {
    SaveChangedActiveApexes(ctx.changed_active_apexes);
  }

  auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          boot_clock::now() - time_started)
                          .count();
  LOG(INFO) << "OnBootstrap done, duration=" << time_elapsed;
  return 0;
}

void InitializeVold(CheckpointInterface* checkpoint_service) {
  if (checkpoint_service == nullptr) {
    // For tests to reset global states because tests that change global states
    // may affect other tests.
    gVoldService = nullptr;
    gSupportsFsCheckpoints = false;
    gInFsCheckpointMode = false;
    return;
  }
  gVoldService = checkpoint_service;
  Result<bool> supports_fs_checkpoints = gVoldService->SupportsFsCheckpoints();
  if (supports_fs_checkpoints.ok()) {
    gSupportsFsCheckpoints = *supports_fs_checkpoints;
  } else {
    LOG(ERROR) << "Failed to check if filesystem checkpoints are supported: "
               << supports_fs_checkpoints.error();
  }
  if (gSupportsFsCheckpoints) {
    Result<bool> needs_checkpoint = gVoldService->NeedsCheckpoint();
    if (needs_checkpoint.ok()) {
      gInFsCheckpointMode = *needs_checkpoint;
    } else {
      LOG(ERROR) << "Failed to check if we're in filesystem checkpoint mode: "
                 << needs_checkpoint.error();
    }
  }
}

void InitializeSessionManager(ApexSessionManager* session_manager) {
  gSessionManager = session_manager;
}

void Initialize(CheckpointInterface* checkpoint_service) {
  InitializeVold(checkpoint_service);

  ApexFileRepository& instance = ApexFileRepository::GetInstance();
  if (auto status = AddPreinstalledData(instance); !status.ok()) {
    LOG(ERROR) << "Failed to collect preinstalled data: " << status.error();
    return;
  }

  gMountedApexes.PopulateFromMounts();
}

namespace {

Result<ApexFile> OpenAndValidateDecompressedApex(const ApexFile& capex,
                                                 const std::string& apex_path) {
  auto apex = ApexFile::Open(apex_path);
  if (!apex.ok()) {
    return Error() << "Failed to open decompressed APEX: " << apex.error();
  }
  auto result = ValidateDecompressedApex(capex, *apex);
  if (!result.ok()) {
    return result.error();
  }
  auto ctx = GetfileconPath(apex_path);
  if (!ctx.ok()) {
    return ctx.error();
  }
  if (!StartsWith(*ctx, gConfig->active_apex_selinux_ctx)) {
    return Error() << apex_path << " has wrong SELinux context " << *ctx;
  }
  return std::move(*apex);
}

}  // namespace

// Process a single compressed APEX. Returns the decompressed APEX if
// successful.
Result<ApexFile> ProcessCompressedApex(const ApexFile& capex,
                                       bool is_ota_chroot) {
  LOG(INFO) << "Processing compressed APEX " << capex.GetPath();
  const auto decompressed_apex_path =
      StringPrintf("%s/%s%s", gConfig->decompression_dir,
                   GetPackageId(capex.GetManifest()).c_str(),
                   kDecompressedApexPackageSuffix);
  // Check if decompressed APEX already exist
  auto decompressed_path_exists = PathExists(decompressed_apex_path);
  if (decompressed_path_exists.ok() && *decompressed_path_exists) {
    // Check if existing decompressed APEX is valid
    auto result =
        OpenAndValidateDecompressedApex(capex, decompressed_apex_path);
    if (result.ok()) {
      LOG(INFO) << "Skipping decompression for " << capex.GetPath();
      return result;
    }
    // Do not delete existing decompressed APEX when is_ota_chroot is true
    if (!is_ota_chroot) {
      // Existing decompressed APEX is not valid. We will have to redecompress
      LOG(WARNING) << "Existing decompressed APEX is invalid: "
                   << result.error();
      RemoveFileIfExists(decompressed_apex_path);
    }
  }

  // We can also reuse existing OTA APEX, depending on situation
  auto ota_apex_path = StringPrintf("%s/%s%s", gConfig->decompression_dir,
                                    GetPackageId(capex.GetManifest()).c_str(),
                                    kOtaApexPackageSuffix);
  auto ota_path_exists = PathExists(ota_apex_path);
  if (ota_path_exists.ok() && *ota_path_exists) {
    if (is_ota_chroot) {
      // During ota_chroot, we try to reuse ota APEX as is
      auto result = OpenAndValidateDecompressedApex(capex, ota_apex_path);
      if (result.ok()) {
        LOG(INFO) << "Skipping decompression for " << ota_apex_path;
        return result;
      }
      // Existing ota_apex is not valid. We will have to decompress
      LOG(WARNING) << "Existing decompressed OTA APEX is invalid: "
                   << result.error();
      RemoveFileIfExists(ota_apex_path);
    } else {
      // During boot, we can avoid decompression by renaming OTA apex
      // to expected decompressed_apex path

      // Check if ota_apex APEX is valid
      auto result = OpenAndValidateDecompressedApex(capex, ota_apex_path);
      if (result.ok()) {
        // ota_apex matches with capex. Slot has been switched.

        // Rename ota_apex to expected decompressed_apex path
        if (rename(ota_apex_path.c_str(), decompressed_apex_path.c_str()) ==
            0) {
          // Check if renamed decompressed APEX is valid
          result =
              OpenAndValidateDecompressedApex(capex, decompressed_apex_path);
          if (result.ok()) {
            LOG(INFO) << "Renamed " << ota_apex_path << " to "
                      << decompressed_apex_path;
            return result;
          }
          // Renamed ota_apex is not valid. We will have to decompress
          LOG(WARNING) << "Renamed decompressed APEX from " << ota_apex_path
                       << " to " << decompressed_apex_path
                       << " is invalid: " << result.error();
          RemoveFileIfExists(decompressed_apex_path);
        } else {
          PLOG(ERROR) << "Failed to rename file " << ota_apex_path;
        }
      }
    }
  }

  // There was no way to avoid decompression

  // Clean up reserved space before decompressing capex
  if (auto ret = DeleteDirContent(gConfig->ota_reserved_dir); !ret.ok()) {
    LOG(ERROR) << "Failed to clean up reserved space: " << ret.error();
  }

  auto decompression_dest =
      is_ota_chroot ? ota_apex_path : decompressed_apex_path;
  auto scope_guard = android::base::make_scope_guard(
      [&]() { RemoveFileIfExists(decompression_dest); });

  auto decompression_result = capex.Decompress(decompression_dest);
  if (!decompression_result.ok()) {
    return Error() << "Failed to decompress : " << capex.GetPath().c_str()
                   << " " << decompression_result.error();
  }

  // Fix label of decompressed file
  auto restore = RestoreconPath(decompression_dest);
  if (!restore.ok()) {
    return restore.error();
  }

  // Validate the newly decompressed APEX
  auto return_apex = OpenAndValidateDecompressedApex(capex, decompression_dest);
  if (!return_apex.ok()) {
    return Error() << "Failed to decompress CAPEX: " << return_apex.error();
  }

  scope_guard.Disable();
  return return_apex;
}

Result<void> ValidateDecompressedApex(const ApexFile& capex,
                                      const ApexFile& apex) {
  // Decompressed APEX must have same public key as CAPEX
  if (capex.GetBundledPublicKey() != apex.GetBundledPublicKey()) {
    return Error()
           << "Public key of compressed APEX is different than original "
           << "APEX for " << apex.GetPath();
  }
  // Decompressed APEX must have same version as CAPEX
  if (capex.GetManifest().version() != apex.GetManifest().version()) {
    return Error()
           << "Compressed APEX has different version than decompressed APEX "
           << apex.GetPath();
  }
  // Decompressed APEX must have same root digest as what is stored in CAPEX
  auto apex_verity = apex.VerifyApexVerity(apex.GetBundledPublicKey());
  if (!apex_verity.ok() ||
      capex.GetManifest().capexmetadata().originalapexdigest() !=
          apex_verity->root_digest) {
    return Error() << "Root digest of " << apex.GetPath()
                   << " does not match with" << " expected root digest in "
                   << capex.GetPath();
  }
  return {};
}

void ActivateApexesOnStart() {
  ActivationContext ctx;
  // Process sessions before adding data apexes.
  // If there is any new apex to be installed on /data/app-staging, hardlink
  // them to /data/apex/active first.
  ProcessSessions(ctx);

  auto& instance = ApexFileRepository::GetInstance();
  if (auto status = instance.AddDataApex(gConfig->active_apex_data_dir);
      !status.ok()) {
    LOG(ERROR) << "Failed to collect data APEX files : " << status.error();
  }

  auto activate_status = ActivateApexPackages(
      ctx, instance.SelectApexForActivation(), ActivationMode::kBootMode,
      /*revert_on_error=*/true, /*fallback_on_error=*/true);
  EmitApexInfoList(activate_status.activated, /*is_bootstrap=*/false);
  SaveChangedActiveApexes(ctx.changed_active_apexes);
}

void OnStart() {
  ATRACE_NAME("OnStart");
  LOG(INFO) << "Marking APEXd as starting";
  auto time_started = boot_clock::now();
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusStarting)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusStarting;
  }
  if constexpr (flags::mount_before_data()) {
    // When started with the feature(mount-before-data) enabled, make sure that
    // the device never goes back to the migration state even if OnStart() fails
    // to complete.
    if (IsMountBeforeDataEnabled()) {
      android::apex::TouchFile(gConfig->metadata_config_dir,
                               "mount_before_data");
    }
  }

  // Ask whether we should revert any active sessions; this can happen if
  // we've exceeded the retry count on a device that supports filesystem
  // checkpointing.
  if (gSupportsFsCheckpoints) {
    Result<bool> needs_revert = gVoldService->NeedsRollback();
    if (!needs_revert.ok()) {
      LOG(ERROR) << "Failed to check if we need a revert: "
                 << needs_revert.error();
    } else if (*needs_revert) {
      LOG(INFO) << "Exceeded number of session retries ("
                << kNumRetriesWhenCheckpointingEnabled
                << "). Starting a revert";
      if (auto st = RevertActiveSessions("", ""); st.ok()) {
        // After reverting the active sessions and restoring the active APEXes,
        // need to reboot to activate the restored active APEXes.
        if (IsMountBeforeDataEnabled()) {
          Reboot();
        }
      } else {
        LOG(ERROR) << "Revert failed: " << st.error();
      }
    }
  }

  // TODO(b/381175707) until migration is finished, OnStart should activate both
  // locations: /data/apex/active + pinned apexes
  if (!IsMountBeforeDataEnabled()) {
    ActivateApexesOnStart();
  }

  // Now that APEXes are mounted, snapshot or restore DE_sys data.
  SnapshotOrRestoreDeSysData();

  auto time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          boot_clock::now() - time_started)
                          .count();
  LOG(INFO) << "OnStart done, duration=" << time_elapsed;
}

void OnAllPackagesActivated() {
  // Set a system property to let other components know that APEXs are
  // activated, but are not yet ready to be used. init is expected to wait
  // for this status before performing configuration based on activated
  // apexes. Other components that need to use APEXs should wait for the
  // ready state instead.
  LOG(INFO) << "Marking APEXd as activated";
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusActivated)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusActivated;
  }
}

void OnAllPackagesReady() {
  // Set a system property to let other components know that APEXs are
  // correctly mounted and ready to be used. Before using any file from APEXs,
  // they can query this system property to ensure that they are okay to
  // access. Or they may have a on-property trigger to delay a task until
  // APEXs become ready.
  LOG(INFO) << "Marking APEXd as ready";
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusReady)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusReady;
  }
  // Since apexd.status property is a system property, we expose yet another
  // property as system_restricted_prop so that, for example, vendor can rely
  // on the "ready" event.
  if (!SetProperty(kApexAllReadyProp, "true")) {
    PLOG(ERROR) << "Failed to set " << kApexAllReadyProp << " to true";
  }
}

Result<std::vector<ApexFile>> SubmitStagedSession(
    const int session_id, const std::vector<int>& child_session_ids,
    const bool has_rollback_enabled, const bool is_rollback,
    const int rollback_id) REQUIRES(!gInstallLock) {
  auto install_guard = std::scoped_lock{gInstallLock};
  auto event = InstallRequestedEvent(InstallType::Staged, is_rollback);

  if (session_id == 0) {
    return Error() << "Session id was not provided.";
  }
  if (has_rollback_enabled && is_rollback) {
    return Error() << "Cannot set session " << session_id << " as both a"
                   << " rollback and enabled for rollback.";
  }

  // Create a backup of the current ACTIVE APEXes or update the existing backup.
  // This could be called just before applying staged sessions in
  // ProcessSessions() but we want to put as much as possible in
  // SubmitStagedSession() to avoid fail-and-recover during boot.
  OR_RETURN(BackupActiveApexes());

  auto ret =
      OR_RETURN(OpenApexFilesInSessionDirs(session_id, child_session_ids));
  event.AddFiles(ret);

  auto result = OR_RETURN(VerifyPackagesStagedInstall(ret));
  event.AddHals(result.apex_hals);

  std::vector<std::string> apex_images;
  if (IsMountBeforeDataEnabled()) {
    apex_images = OR_RETURN(GetImageManager()->PinApexFiles(ret));
  }

  // Run test commands only when installing Shim APEX on a debuggable device.
  if (GetBoolProperty("ro.debuggable", false) &&
      std::ranges::any_of(ret, &shim::IsShimApex)) {
    OR_RETURN(RunTestHookCommands("apexd.test_hook.submit_staged_session"));
  }

  // The incoming session is now verified by apexd. From now on, apexd keeps
  // its own session data. The session should be marked as "ready" so that it
  // becomes STAGED. On next reboot, STAGED sessions become ACTIVATED, which
  // means the APEXes in those sessions are in "active" state and to be
  // activated.
  //
  //    SubmitStagedSession     MarkStagedSessionReady
  //           |                          |
  //           V                          V
  //         VERIFIED (created) ------------> STAGED
  //                                            |
  //                                            | <--ActivateStagedSessions
  //                                            V
  //                                        ACTIVATED
  //

  auto session = gSessionManager->CreateSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  (*session).SetChildSessionIds(child_session_ids);
  std::string build_fingerprint = GetProperty(kBuildFingerprintSysprop, "");
  (*session).SetBuildFingerprint(build_fingerprint);
  session->SetHasRollbackEnabled(has_rollback_enabled);
  session->SetIsRollback(is_rollback);
  session->SetRollbackId(rollback_id);
  for (const auto& apex_file : ret) {
    session->AddApexName(apex_file.GetManifest().name());
  }
  session->SetApexFileHashes(event.GetFileHashes());
  session->SetApexImages(apex_images);
  Result<void> commit_status =
      (*session).UpdateStateAndCommit(SessionState::VERIFIED);
  if (!commit_status.ok()) {
    return commit_status.error();
  }

  event.MarkSucceeded();

  return ret;
}

Result<void> MarkStagedSessionReady(const int session_id)
    REQUIRES(!gInstallLock) {
  auto install_guard = std::scoped_lock{gInstallLock};
  auto session = gSessionManager->GetSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  // We should only accept sessions in SessionState::VERIFIED or
  // SessionState::STAGED state. In the SessionState::STAGED case, this
  // function is effectively a no-op.
  auto session_state = (*session).GetState();
  if (session_state == SessionState::STAGED) {
    return {};
  }
  if (session_state == SessionState::VERIFIED) {
    return (*session).UpdateStateAndCommit(SessionState::STAGED);
  }
  return Error() << "Invalid state for session " << session_id
                 << ". Cannot mark it as ready.";
}

Result<void> MarkStagedSessionSuccessful(const int session_id) {
  auto session = gSessionManager->GetSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  // Only SessionState::ACTIVATED or SessionState::SUCCESS states are
  // accepted. In the SessionState::SUCCESS state, this function is a no-op.
  if (session->GetState() == SessionState::SUCCESS) {
    return {};
  } else if (session->GetState() == SessionState::ACTIVATED) {
    // TODO: Handle activated apexes still unavailable to apexd at this time.
    // This is because apexd is started before this activation with a linker
    // configuration which doesn't know about statsd
    SendSessionApexInstallationEndedAtom(*session, InstallResult::Success);
    auto cleanup_status = DeleteBackup();
    if (!cleanup_status.ok()) {
      return Error() << "Failed to mark session " << *session
                     << " as successful : " << cleanup_status.error();
    }
    if (session->IsRollback() && !gSupportsFsCheckpoints) {
      DeleteDePreRestoreSnapshots(*session);
    }
    return session->UpdateStateAndCommit(SessionState::SUCCESS);
  } else {
    return Error() << "Session " << *session << " can not be marked successful";
  }
}

// Removes APEXes on /data that have not been activated
void RemoveInactiveDataApex() {
  std::vector<std::string> all_apex_files;
  Result<std::vector<std::string>> active_apex =
      FindFilesBySuffix(gConfig->active_apex_data_dir, {kApexPackageSuffix});
  if (!active_apex.ok()) {
    LOG(ERROR) << "Failed to scan " << gConfig->active_apex_data_dir << " : "
               << active_apex.error();
  } else {
    all_apex_files.insert(all_apex_files.end(),
                          std::make_move_iterator(active_apex->begin()),
                          std::make_move_iterator(active_apex->end()));
  }
  Result<std::vector<std::string>> decompressed_apex = FindFilesBySuffix(
      gConfig->decompression_dir, {kDecompressedApexPackageSuffix});
  if (!decompressed_apex.ok()) {
    LOG(ERROR) << "Failed to scan " << gConfig->decompression_dir << " : "
               << decompressed_apex.error();
  } else {
    all_apex_files.insert(all_apex_files.end(),
                          std::make_move_iterator(decompressed_apex->begin()),
                          std::make_move_iterator(decompressed_apex->end()));
  }

  for (const auto& path : all_apex_files) {
    if (!apexd_private::IsMounted(path)) {
      LOG(INFO) << "Removing inactive data APEX " << path;
      if (unlink(path.c_str()) != 0) {
        PLOG(ERROR) << "Failed to unlink inactive data APEX " << path;
      }
    }
  }

  // Update the active list first and remove unused pinned images. Note that
  // not every apex in active list is activated in case the preinstalled
  // APEXes may have changed due to OTA.

  auto image_manager = GetImageManager();
  std::vector<ApexListEntry> active_list;
  if (auto st = image_manager->GetApexList(ApexListType::ACTIVE); st.ok()) {
    active_list = std::move(*st);
  } else {
    LOG(ERROR) << "Failed to get active apex list: " << st.error();
    return;
  }
  // Remove skipped entries from ACTIVE list.
  std::erase_if(active_list, [&](const auto& entry) {
    auto path = image_manager->GetMappedPath(entry.image_name);
    return !path || !apexd_private::IsMounted(path.value());
  });
  // Then, update the list
  if (auto st =
          image_manager->UpdateApexList(ApexListType::ACTIVE, active_list);
      !st.ok()) {
    LOG(ERROR) << "Failed to update active apex list: " << st.error();
  }

  // Now, remove unused pinned images.

  // We've already checked that active_list contains what's actually activated.
  std::unordered_set<std::string> images_in_use;
  for (const auto& entry : active_list) {
    images_in_use.insert(entry.image_name);
  }

  // If there are sessions not yet deleted, apex images referenced by them are
  // also considered as being in use.
  // TODO(b/409309264) clarify if there IS non-finalized session at this point.
  for (const auto& session : gSessionManager->GetSessions()) {
    images_in_use.insert_range(session.GetApexImages());
  }

  for (const auto& image : image_manager->GetAllImages()) {
    if (images_in_use.contains(image)) {
      continue;
    }
    LOG(INFO) << "Removing inactive pinned APEX image: " << image;
    if (auto st = image_manager->UnmapAndDeleteImage(image); !st.ok()) {
      LOG(ERROR) << "Failed to remove pinned APEX image: " << image << ": "
                 << st.error();
    }
  }
}

bool IsApexDevice(const std::string& dev_name) {
  auto& repo = ApexFileRepository::GetInstance();
  for (const auto& apex : repo.GetPreInstalledApexFiles()) {
    if (StartsWith(dev_name, apex.get().GetManifest().name())) {
      return true;
    }
  }
  return false;
}

// TODO(b/192241176): move to apexd_verity.{h,cpp}.
void DeleteUnusedVerityDevices() {
  DeviceMapper& dm = DeviceMapper::Instance();
  std::vector<DeviceMapper::DmBlockDevice> all_devices;
  if (!dm.GetAvailableDevices(&all_devices)) {
    LOG(WARNING) << "Failed to fetch dm devices";
    return;
  }
  for (const auto& dev : all_devices) {
    auto state = dm.GetState(dev.name());
    if (state == DmDeviceState::SUSPENDED && IsApexDevice(dev.name())) {
      LOG(INFO) << "Deleting unused dm device " << dev.name();
      auto res = DeleteDmDevice(dev.name(), /* deferred= */ false);
      if (!res.ok()) {
        LOG(WARNING) << res.error();
      }
    }
  }
}

void BootCompletedCleanup() REQUIRES(!gInstallLock) {
  auto install_guard = std::scoped_lock{gInstallLock};
  gSessionManager->DeleteFinalizedSessions();
  RemoveInactiveDataApex();
  DeleteUnusedVerityDevices();

  if constexpr (flags::mount_before_data()) {
    // Mark "migration done" by creating /metadata/apex/config/mount_before_data
    if (IsMountBeforeDataEnabled() || CanMountBeforeDataOnNextBoot()) {
      android::apex::TouchFile(gConfig->metadata_config_dir,
                               "mount_before_data");
    }
  }
}

int UnmountAll() {
  // Use a separate DB instance to avoid interaction with other parts.
  MountedApexDatabase database;
  database.PopulateFromMounts();
  int ret = 0;
  database.ForallMountedApexes([&](const std::string& /*package*/,
                                   const MountedApexData& data, bool latest) {
    LOG(INFO) << "Unmounting " << data.full_path << " mounted on "
              << data.mount_point;
    auto apex = ApexFile::Open(data.full_path);
    if (!apex.ok()) {
      LOG(ERROR) << "Failed to open " << data.full_path << " : "
                 << apex.error();
      ret = 1;
      return;
    }
    if (latest) {
      auto pos = data.mount_point.find('@');
      CHECK(pos != std::string::npos);
      std::string bind_mount = data.mount_point.substr(0, pos);
      if (umount2(bind_mount.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0) {
        PLOG(ERROR) << "Failed to unmount bind-mount " << bind_mount;
        ret = 1;
        return;
      }
    }
    if (auto status = Unmount(data, /* deferred= */ true); !status.ok()) {
      LOG(ERROR) << "Failed to unmount " << data.mount_point << " : "
                 << status.error();
      ret = 1;
    }
  });
  return ret;
}

// Given a single new APEX incoming via OTA, should we allocate space for it?
bool ShouldAllocateSpaceForDecompression(const std::string& new_apex_name,
                                         const int64_t new_apex_version,
                                         const ApexFileRepository& instance,
                                         const MountedApexDatabase& db) {
  // An apex at most will have two versions on device: pre-installed and data.

  // Check if there is a pre-installed version for the new apex.
  if (!instance.HasPreInstalledVersion(new_apex_name)) {
    // We are introducing a new APEX that doesn't exist at all
    return true;
  }

  // Check if there is a data apex
  // If the current active apex is preinstalled, then it means no data apex.
  auto current_active = db.GetLatestMountedApex(new_apex_name);
  if (!current_active) {
    LOG(ERROR) << "Failed to get mount data for : " << new_apex_name
               << " is preinstalled, but not activated.";
    return true;
  }
  auto current_active_apex_file = ApexFile::Open(current_active->full_path);
  if (!current_active_apex_file.ok()) {
    LOG(ERROR) << "Failed to open " << current_active->full_path << " : "
               << current_active_apex_file.error();
    return true;
  }
  if (instance.IsPreInstalledApex(*current_active_apex_file)) {
    return true;
  }

  // From here on, data apex exists. So we should compare directly against data
  // apex.
  const int64_t data_version =
      current_active_apex_file->GetManifest().version();
  // We only decompress the new_apex if it has higher version than data apex.
  return new_apex_version > data_version;
}

int64_t CalculateSizeForCompressedApex(
    const std::vector<std::tuple<std::string, int64_t, int64_t>>&
        compressed_apexes) {
  const auto& instance = ApexFileRepository::GetInstance();
  int64_t result = 0;
  for (const auto& compressed_apex : compressed_apexes) {
    std::string module_name;
    int64_t version_code;
    int64_t decompressed_size;
    std::tie(module_name, version_code, decompressed_size) = compressed_apex;
    if (ShouldAllocateSpaceForDecompression(module_name, version_code, instance,
                                            gMountedApexes)) {
      result += decompressed_size;
    }
  }
  return result;
}

std::string CastPartition(ApexPartition in) {
  switch (in) {
    case ApexPartition::System:
      return "SYSTEM";
    case ApexPartition::SystemExt:
      return "SYSTEM_EXT";
    case ApexPartition::Product:
      return "PRODUCT";
    case ApexPartition::Vendor:
      return "VENDOR";
    case ApexPartition::Odm:
      return "ODM";
  }
}

void CollectApexInfoList(std::ostream& os,
                         const std::vector<ApexFileRef>& active_apexs,
                         const std::vector<ApexFileRef>& inactive_apexs) {
  auto& instance = ApexFileRepository::GetInstance();
  auto convert = [&](const ApexFile& apex, bool is_active) {
    auto preinstalled_path =
        instance.GetPreinstalledPath(apex.GetManifest().name());
    std::optional<std::string> preinstalled_module_path;
    if (preinstalled_path.ok()) {
      preinstalled_module_path = *preinstalled_path;
    }

    auto partition = CastPartition(OR_FATAL(instance.GetPartition(apex)));

    std::optional<int64_t> mtime =
        instance.GetBlockApexLastUpdateSeconds(apex.GetPath());
    if (!mtime.has_value()) {
      struct stat stat_buf;
      if (stat(apex.GetPath().c_str(), &stat_buf) == 0) {
        mtime.emplace(stat_buf.st_mtime);
      } else {
        PLOG(WARNING) << "Failed to stat " << apex.GetPath();
      }
    }
    com::android::apex::ApexInfo apex_info(
        apex.GetManifest().name(), apex.GetPath(), preinstalled_module_path,
        apex.GetManifest().version(), apex.GetManifest().versionname(),
        instance.IsPreInstalledApex(apex), is_active, mtime,
        apex.GetManifest().providesharedapexlibs(), partition);
    return apex_info;
  };
  // Note: xsdc-generated writer needs to construct the object structure, which
  // is a bit inefficient. Here the root element is manually handled for better
  // performance. Tests will ensure the output is well-formed.
  // TODO: extend xsdc for streaming writer
  os << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  os << "<apex-info-list>\n";
  for (const auto& apex : active_apexs) {
    convert(apex, /* is_active= */ true).write(os, "apex-info");
  }
  for (const auto& apex : inactive_apexs) {
    convert(apex, /* is_active= */ false).write(os, "apex-info");
  }
  os << "</apex-info-list>";
}

// Reserve |size| bytes in |dest_dir| by creating a zero-filled file.
// Also, we always clean up ota_apex that has been processed as
// part of pre-reboot decompression whenever we reserve space.
Result<void> ReserveSpaceForCompressedApex(int64_t size,
                                           const std::string& dest_dir) {
  if (size < 0) {
    return Error() << "Cannot reserve negative byte of space";
  }

  // Since we are reserving space, then we must be preparing for a new OTA.
  // Clean up any processed ota_apex from previous OTA.
  auto ota_apex_files =
      FindFilesBySuffix(gConfig->decompression_dir, {kOtaApexPackageSuffix});
  if (!ota_apex_files.ok()) {
    return Error() << "Failed to clean up ota_apex: " << ota_apex_files.error();
  }
  for (const std::string& ota_apex : *ota_apex_files) {
    RemoveFileIfExists(ota_apex);
  }

  auto file_path = StringPrintf("%s/full.tmp", dest_dir.c_str());
  if (size == 0) {
    LOG(INFO) << "Cleaning up reserved space for compressed APEX";
    // Ota is being cancelled. Clean up reserved space
    RemoveFileIfExists(file_path);
    return {};
  }

  LOG(INFO) << "Reserving " << size << " bytes for compressed APEX";
  unique_fd dest_fd(
      open(file_path.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT, 0644));
  if (dest_fd.get() == -1) {
    return ErrnoError() << "Failed to open file for reservation "
                        << file_path.c_str();
  }

  // Resize to required size, posix_fallocate will not shrink files so resize
  // is needed.
  std::error_code ec;
  std::filesystem::resize_file(file_path, size, ec);
  if (ec) {
    RemoveFileIfExists(file_path);
    return ErrnoError() << "Failed to resize file " << file_path.c_str()
                        << " : " << ec.message();
  }

  // Allocate blocks for the requested size.
  // resize_file will create sparse file with 0 blocks on filesystems that
  // supports sparse files.
  if ((errno = posix_fallocate(dest_fd.get(), 0, size))) {
    RemoveFileIfExists(file_path);
    return ErrnoError() << "Failed to allocate blocks for file "
                        << file_path.c_str();
  }

  return {};
}

// Adds block apexes if system property is set.
Result<int> AddBlockApex(ApexFileRepository& instance) {
  auto prop = GetProperty(gConfig->vm_payload_metadata_partition_prop, "");
  if (prop != "") {
    auto block_count = instance.AddBlockApex(prop);
    if (!block_count.ok()) {
      return Error() << "Failed to scan block APEX files: "
                     << block_count.error();
    }
    return block_count;
  } else {
    LOG(INFO) << "No block apex metadata partition found, not adding block "
              << "apexes";
  }
  return 0;
}

// When running in the VM mode, we follow the minimal start-up operations.
// - AddPreInstalledData: note that CAPEXes are not supported in the VM mode
// - AddBlockApex
// - ActivateApexPackages
// - setprop apexd.status: activated/ready
int OnStartInVmMode() {
  Result<void> loop_ready = WaitForFile("/dev/loop-control", 20s);
  if (!loop_ready.ok()) {
    LOG(ERROR) << loop_ready.error();
  }

  auto& instance = ApexFileRepository::GetInstance();

  if (auto status = AddPreinstalledData(instance); !status.ok()) {
    LOG(ERROR) << "Failed collect preinstalled data: " << status.error();
    return 1;
  }

  if (auto status = AddBlockApex(instance); !status.ok()) {
    LOG(ERROR) << "Failed to scan host APEX files: " << status.error();
    return 1;
  }

  ActivationContext ctx;
  auto result = ActivateApexPackages(
      ctx, instance.SelectApexForActivation(), ActivationMode::kVmMode,
      /*revert_on_error=*/false, /*fallback_on_error=*/false);
  if (!result.ok()) {
    LOG(ERROR) << "Failed to activate apex packages : " << result.error();
    return 1;
  }
  EmitApexInfoList(result.activated, /*is_bootstrap=*/false);

  OnAllPackagesActivated();
  // In VM mode, we don't run a separate --snapshotde mode.
  // Instead, we mark apexd.status "ready" right now.
  OnAllPackagesReady();
  return 0;
}

int OnOtaChrootBootstrap(bool also_include_staged_apexes) {
  auto& instance = ApexFileRepository::GetInstance();
  if (auto status = AddPreinstalledData(instance); !status.ok()) {
    LOG(ERROR) << "Failed to scan preinstalled data: " << status.error();
    return 1;
  }
  if (also_include_staged_apexes) {
    // Scan staged dirs, and then scan the active dir. If a module is in both
    // a staged dir and the active dir, the APEX with a higher version will be
    // picked. If the versions are equal, the APEX in staged dir will be
    // picked.
    //
    // The result is an approximation of what the active dir will actually
    // have after the reboot. In case of a downgrade install, it differs from
    // the actual, but this is not a supported case.
    for (const ApexSession& session :
         gSessionManager->GetSessionsInState(SessionState::STAGED)) {
      std::vector<std::string> dirs_to_scan =
          session.GetStagedApexDirs(gConfig->staged_session_dir);
      for (const std::string& dir_to_scan : dirs_to_scan) {
        if (auto status = instance.AddDataApex(dir_to_scan); !status.ok()) {
          LOG(ERROR) << "Failed to scan staged apexes from " << dir_to_scan;
          return 1;
        }
      }
    }
  }

  if constexpr (flags::mount_before_data()) {
    auto data_apexes = ScanDataApexFiles(GetImageManager());
    instance.AddDataApexFiles(std::move(data_apexes));
  }

  if (auto status = instance.AddDataApex(gConfig->active_apex_data_dir);
      !status.ok()) {
    LOG(ERROR) << "Failed to scan upgraded apexes from "
               << gConfig->active_apex_data_dir;
    // Fail early because we know we will be wasting cycles generating garbage
    // if we continue.
    return 1;
  }

  ActivationContext ctx;
  auto activate_status = ActivateApexPackages(
      ctx, instance.SelectApexForActivation(), ActivationMode::kOtaChrootMode,
      /*revert_on_error=*/false, /*fallback_on_error=*/true);
  EmitApexInfoList(activate_status.activated, /*is_bootstrap=*/false);
  if (auto status = RestoreconPath(kApexInfoList); !status.ok()) {
    LOG(ERROR) << "Can't restorecon " << kApexInfoList << ": "
               << status.error();
  }
  return 0;
}

android::apex::MountedApexDatabase& GetApexDatabaseForTesting() {
  return gMountedApexes;
}

// A version of apex verification that happens during non-staged APEX
// installation.
Result<VerificationResult> VerifyPackageNonStagedInstall(
    const ApexFile& apex_file, bool force) {
  OR_RETURN(VerifyPackageBoot(apex_file));

  auto sessions = gSessionManager->GetSessions();

  // Check overlapping: reject if the same package is already staged
  OR_RETURN(VerifyNoOverlapInSessions(Single(apex_file), sessions));

  auto check_fn =
      [&apex_file,
       &force](const std::string& mount_point) -> Result<VerificationResult> {
    if (force) {
      return {};
    }
    VerificationResult result;
    if (access((mount_point + "/app").c_str(), F_OK) == 0) {
      return Error() << apex_file.GetPath() << " contains app inside";
    }
    if (access((mount_point + "/priv-app").c_str(), F_OK) == 0) {
      return Error() << apex_file.GetPath() << " contains priv-app inside";
    }
    result.apex_hals =
        OR_RETURN(CheckVintf(Single(apex_file), Single(mount_point)));
    return result;
  };
  return RunVerifyFnInsideTempMount(apex_file, check_fn);
}

Result<void> CheckSupportsNonStagedInstall(const ApexFile& new_apex,
                                           bool force) {
  const auto& new_manifest = new_apex.GetManifest();

  if (!force) {
    if (!new_manifest.supportsrebootlessupdate()) {
      return Error() << new_apex.GetPath()
                     << " does not support non-staged update";
    }

    // Check if update will impact linkerconfig.

    // This APEX provides native libs to other parts of the platform. It can
    // only be updated via staged install flow.
    if (new_manifest.providenativelibs_size() > 0) {
      return Error() << new_apex.GetPath() << " provides native libs";
    }

    // We don't allow non-staged updates of APEXES that have java libs inside.
    if (new_manifest.jnilibs_size() > 0) {
      return Error() << new_apex.GetPath() << " requires JNI libs";
    }
  }

  // Brand-new apexes are not supported.
  if (ApexFileRepository::IsBrandNewApexEnabled()) {
    // Make sure that the new apex has the preinstall one.
    auto preinstalled = ApexFileRepository::GetInstance().GetPreInstalledApex(
        new_manifest.name());
    if (!preinstalled.has_value()) {
      return Error() << "No preinstalled apex found for package "
                     << new_manifest.name();
    }
  }
  return {};
}

Result<size_t> ComputePackageIdMinor(const ApexFile& apex) {
  static constexpr size_t kMaxVerityDevicesPerApexName = 3u;
  DeviceMapper& dm = DeviceMapper::Instance();
  std::vector<DeviceMapper::DmBlockDevice> dm_devices;
  if (!dm.GetAvailableDevices(&dm_devices)) {
    return Error() << "Failed to list dm devices";
  }
  size_t devices = 0;
  size_t next_minor = 1;
  for (const auto& dm_device : dm_devices) {
    std::string_view dm_name(dm_device.name());
    // Skip .payload and .apex dm-linear devices
    if (dm_name.ends_with(kDmLinearPayloadSuffix) ||
        dm_name.ends_with(kDmLinearApexSuffix)) {
      continue;
    }
    // Format is <module_name>@<version_code>[_<minor>]
    if (!ConsumePrefix(&dm_name, apex.GetManifest().name())) {
      continue;
    }
    devices++;
    auto pos = dm_name.find_last_of('_');
    if (pos == std::string_view::npos) {
      continue;
    }
    size_t minor;
    if (!ParseUint(std::string(dm_name.substr(pos + 1)), &minor)) {
      return Error() << "Unexpected dm device name " << dm_device.name();
    }
    if (next_minor < minor + 1) {
      next_minor = minor + 1;
    }
  }
  if (devices > kMaxVerityDevicesPerApexName) {
    return Error() << "There are too many (" << devices
                   << ") dm block devices associated with package "
                   << apex.GetManifest().name();
  }
  while (true) {
    std::string target_file =
        StringPrintf("%s/%s_%zu.apex", gConfig->active_apex_data_dir,
                     GetPackageId(apex.GetManifest()).c_str(), next_minor);
    if (access(target_file.c_str(), F_OK) == 0) {
      next_minor++;
    } else {
      break;
    }
  }

  return next_minor;
}

// TODO(b/238820991) Handle failures
Result<void> UnloadApexFromInit(const std::string& apex_name) {
  if (!SetProperty(kCtlApexUnloadSysprop, apex_name)) {
    // When failed to SetProperty(), there's nothing we can do here.
    // Log error and return early to avoid indefinite waiting for ack.
    return Error() << "Failed to set " << kCtlApexUnloadSysprop << " to "
                   << apex_name;
  }
  SetProperty("apex." + apex_name + ".ready", "false");
  return {};
}

// TODO(b/238820991) Handle failures
Result<void> LoadApexFromInit(const std::string& apex_name) {
  if (!SetProperty(kCtlApexLoadSysprop, apex_name)) {
    // When failed to SetProperty(), there's nothing we can do here.
    // Log error and return early to avoid indefinite waiting for ack.
    return Error() << "Failed to set " << kCtlApexLoadSysprop << " to "
                   << apex_name;
  }
  SetProperty("apex." + apex_name + ".ready", "true");
  return {};
}

Result<ApexFile> InstallPackage(const std::string& package_path, bool force)
    REQUIRES(!gInstallLock) {
  auto install_guard = std::scoped_lock{gInstallLock};
  auto event = InstallRequestedEvent(InstallType::NonStaged,
                                     /*is_rollback=*/false);

  auto temp_apex = ApexFile::Open(package_path);
  if (!temp_apex.ok()) {
    return temp_apex.error();
  }

  event.AddFiles(Single(*temp_apex));

  const std::string& module_name = temp_apex->GetManifest().name();
  // Don't allow non-staged update if there are no active versions of this
  // APEX.
  auto cur_mounted_data = gMountedApexes.GetLatestMountedApex(module_name);
  if (!cur_mounted_data.has_value()) {
    return Error() << "No active version found for package " << module_name;
  }

  auto cur_apex = ApexFile::Open(cur_mounted_data->full_path);
  if (!cur_apex.ok()) {
    return cur_apex.error();
  }

  // Do a quick check if this APEX can be installed without a reboot.
  // Note that passing this check doesn't guarantee that APEX will be
  // successfully installed.
  if (auto r = CheckSupportsNonStagedInstall(*temp_apex, force); !r.ok()) {
    return r.error();
  }

  // 1. Verify that APEX is correct. This is a heavy check that involves
  // mounting an APEX on a temporary mount point and reading the entire
  // dm-verity block device.
  auto result = OR_RETURN(VerifyPackageNonStagedInstall(*temp_apex, force));
  event.AddHals(result.apex_hals);

  // 2. Compute params for mounting new apex.
  auto new_id_minor = ComputePackageIdMinor(*temp_apex);
  if (!new_id_minor.ok()) {
    return new_id_minor.error();
  }

  std::string new_id = GetPackageId(temp_apex->GetManifest()) + "_" +
                       std::to_string(*new_id_minor);

  // Before unmounting the current apex, unload it from the init process:
  // terminates services started from the apex and init scripts read from the
  // apex.
  OR_RETURN(UnloadApexFromInit(module_name));

  // And then reload it from the init process whether it succeeds or not.
  auto reload_apex = android::base::make_scope_guard([&]() {
    if (auto status = LoadApexFromInit(module_name); !status.ok()) {
      LOG(ERROR) << "Failed to load apex " << module_name << " : "
                 << status.error().message();
    }
  });

  // We need a few ScopeGuards to recover the current state when something goes
  // wrong. Note that std::vector destroys elements from the end.
  std::vector<base::ScopeGuard<std::function<void()>>> guards;

  // 3. Unmount currently active APEX.
  OR_RETURN(UnmountPackage(*cur_apex,
                           /*deferred=*/true,
                           /*detach_mount_point=*/force));
  // Re-activate the current apex on error.
  guards.emplace_back(base::make_scope_guard([&]() {
    // We can't really rely on the fact that dm-verity device backing up
    // previously active APEX is still around. We need to create a new one.
    std::string old_new_id = GetPackageId(temp_apex->GetManifest()) + "_" +
                             std::to_string(*new_id_minor + 1);
    auto res = ActivatePackageImpl(*cur_apex, loop::kFreeLoopId, old_new_id,
                                   /* reuse_device= */ false);
    if (!res.ok()) {
      // At this point not much we can do... :(
      LOG(ERROR) << res.error();
    }
  }));

  // 4. Put the new file in "active" as |target_file|
  std::string target_file;
  if (IsMountBeforeDataEnabled()) {
    auto image_manager = GetImageManager();
    // Pin the new file first.
    auto image = OR_RETURN(image_manager->PinApexFiles(Single(*temp_apex)))[0];
    guards.emplace_back(base::make_scope_guard([=]() {
      if (auto st = image_manager->DeleteImage(image); !st.ok()) {
        LOG(ERROR) << st.error();
      }
    }));

    // Update "active" list with the new image.
    auto active_list =
        OR_RETURN(image_manager->GetApexList(ApexListType::ACTIVE));
    OR_RETURN(image_manager->UpdateApexList(
        ApexListType::ACTIVE,
        UpdateApexListWithNewEntries(
            active_list, std::vector{ApexListEntry{image, module_name}})));
    guards.emplace_back(base::make_scope_guard([=]() {
      if (auto st =
              image_manager->UpdateApexList(ApexListType::ACTIVE, active_list);
          !st.ok()) {
        LOG(ERROR) << st.error();
      }
    }));

    // Map the image so that we can access the pinned APEX
    target_file = OR_RETURN(image_manager->MapImage(image));
    guards.emplace_back(base::make_scope_guard([=]() {
      if (auto st = image_manager->UnmapImage(image); !st.ok()) {
        LOG(ERROR) << st.error();
      }
    }));
  } else {
    // Hard-link to final destination
    target_file = StringPrintf("%s/%s.apex", gConfig->active_apex_data_dir,
                               new_id.c_str());
    // At this point it should be safe to hard link |temp_apex| to
    // |params->target_file|. In case reboot happens during one of the stages
    // below, then on next boot apexd will pick up the new verified APEX.
    if (link(package_path.c_str(), target_file.c_str()) != 0) {
      return ErrnoError() << "Failed to link " << package_path << " to "
                          << target_file;
    }
    // Remove the target file on error
    guards.emplace_back(base::make_scope_guard([=]() {
      if (unlink(target_file.c_str()) != 0 && errno != ENOENT) {
        PLOG(ERROR) << "Failed to unlink " << target_file;
      }
    }));
  }

  // Reopen ApexFile from the new location
  auto new_apex = ApexFile::Open(target_file);
  if (!new_apex.ok()) {
    return new_apex.error();
  }

  // 5. And activate new one.
  auto activate_status =
      ActivatePackageImpl(*new_apex, loop::kFreeLoopId, new_id,
                          /* reuse_device= */ false);
  if (!activate_status.ok()) {
    return activate_status.error();
  }

  // Accept the install. Disable all ScopeGuards.
  for (auto& guard : guards) guard.Disable();

  // 6. Now we can unlink old APEX if it's not pre-installed.
  if (!ApexFileRepository::GetInstance().IsPreInstalledApex(*cur_apex)) {
    if (auto image = GetImageManager()->FindPinnedApex(*cur_apex); image) {
      if (auto st = GetImageManager()->UnmapAndDeleteImage(*image); !st.ok()) {
        LOG(ERROR) << st.error();
      }
    } else {
      if (unlink(cur_mounted_data->full_path.c_str()) != 0) {
        PLOG(ERROR) << "Failed to unlink " << cur_mounted_data->full_path;
      }
    }
  }

  // 7. Update apex-info-list.xml
  auto active = GetActivePackages();
  std::vector<ApexFileRef> active_references;
  active_references.reserve(active.size());
  for (const auto& apex : active) {
    active_references.push_back(std::cref(apex));
  }
  EmitApexInfoList(active_references, /*is_bootstrap=*/false);

  event.MarkSucceeded();

  return new_apex;
}

std::set<std::string> GetChangedActiveApexes() {
  std::string content =
      GetProperty(gConfig->apexd_changed_active_apexes_sysprop, "");
  if (content.empty()) {
    return {};
  }
  return {std::from_range, base::Split(content, ":")};
}

void SaveChangedActiveApexes(
    const std::set<std::string>& changed_active_apexes) {
  if (changed_active_apexes.empty()) {
    return;
  }
  std::string content =
      base::Join(std::vector{std::from_range, changed_active_apexes}, ":");
  if (!SetProperty(gConfig->apexd_changed_active_apexes_sysprop, content)) {
    LOG(ERROR) << "Failed to set "
               << gConfig->apexd_changed_active_apexes_sysprop;
  }
}

Result<void> BackupActiveApexes() {
  if (IsMountBeforeDataEnabled()) {
    return GetImageManager()->BackupApexList();
  } else if (!gSupportsFsCheckpoints) {
    return BackupActivePackages();
  } else {
    return {};
  }
}

ApexSessionManager* GetSessionManager() { return gSessionManager; }

void RebootImpl() {
  LOG(INFO) << "Rebooting device";
  if (android_reboot(ANDROID_RB_RESTART2, 0, "apexd_initiated") != 0) {
    LOG(ERROR) << "Failed to reboot device";
  }
  // Wait for reboot to complete as we expect this to be a terminal
  // command. Crash apexd if reboot does not complete even after
  // waiting an arbitrary significant amount of time.
  std::this_thread::sleep_for(std::chrono::seconds(120));
  LOG(FATAL) << "Device did not reboot within 120 seconds";
}

// Use the real implementation by default. Unit tests need to override it.
void (*Reboot)() = &RebootImpl;

}  // namespace apex
}  // namespace android
