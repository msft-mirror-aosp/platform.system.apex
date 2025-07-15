/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "apex_database.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/result.h>
#include <android-base/strings.h>
#include <libdm/dm.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>

#include "apex_constants.h"
#include "apex_file.h"
#include "apexd_utils.h"
#include "string_log.h"

using android::base::ConsumeSuffix;
using android::base::EndsWith;
using android::base::ErrnoError;
using android::base::Error;
using android::base::ParseInt;
using android::base::ReadFileToString;
using android::base::Result;
using android::base::Split;
using android::base::StartsWith;
using android::base::Trim;
using android::dm::DeviceMapper;

namespace fs = std::filesystem;

namespace android {
namespace apex {

namespace {

using MountedApexData = MountedApexDatabase::MountedApexData;

enum BlockDeviceType {
  UnknownDevice,
  LoopDevice,
  DeviceMapperDevice,
};

const fs::path kDevBlock = "/dev/block";
const fs::path kSysBlock = "/sys/block";

class BlockDevice {
  std::string name;  // loopN, dm-N, ...
 public:
  explicit BlockDevice(const fs::path& path) { name = path.filename(); }

  BlockDeviceType GetType() const {
    if (StartsWith(name, "loop")) return LoopDevice;
    if (StartsWith(name, "dm-")) return DeviceMapperDevice;
    return UnknownDevice;
  }

  fs::path SysPath() const { return kSysBlock / name; }

  fs::path DevPath() const { return kDevBlock / name; }

  Result<std::string> GetProperty(const std::string& property) const {
    auto property_file = SysPath() / property;
    std::string property_value;
    if (!ReadFileToString(property_file, &property_value)) {
      return ErrnoError() << "Fail to read";
    }
    return Trim(property_value);
  }

  std::vector<BlockDevice> GetSlaves() const {
    std::vector<BlockDevice> slaves;
    std::error_code ec;
    auto status = WalkDir(SysPath() / "slaves", [&](const auto& entry) {
      BlockDevice dev(entry);
      if (fs::is_block_file(dev.DevPath(), ec)) {
        slaves.push_back(dev);
      }
    });
    if (!status.ok()) {
      LOG(WARNING) << status.error();
    }
    return slaves;
  }
};

std::pair<fs::path, fs::path> ParseMountInfo(const std::string& mount_info) {
  const auto& tokens = Split(mount_info, " ");
  if (tokens.size() < 2) {
    return std::make_pair("", "");
  }
  return std::make_pair(tokens[0], tokens[1]);
}

std::pair<std::string, int> ParseMountPoint(const std::string& mount_point) {
  auto package_id = fs::path(mount_point).filename();
  auto split = Split(package_id, "@");
  if (split.size() == 2) {
    int version;
    if (!ParseInt(split[1], &version)) {
      version = -1;
    }
    return std::make_pair(split[0], version);
  }
  return std::make_pair(package_id, -1);
}

bool IsActiveMountPoint(const std::string& mount_point) {
  return (mount_point.find('@') == std::string::npos);
}

bool IsTempMountPoint(const std::string& mount_point) {
  return EndsWith(mount_point, ".tmp");
}

Result<BlockDevice> GetUnderlying(const BlockDevice& top_device) {
  std::vector<BlockDevice> slaves = top_device.GetSlaves();
  if (slaves.size() != 1) {
    return Error() << "dm device " << top_device.DevPath()
                   << " has unexpected number of slaves (should be 1) : "
                   << slaves.size();
  }
  return std::move(slaves[0]);
}

static Result<void> ValidateDm(const std::string& device_name,
                               const std::string& expected_type) {
  auto& dm = DeviceMapper::Instance();
  std::vector<DeviceMapper::TargetInfo> table;
  if (!dm.GetTableInfo(device_name, &table)) {
    return Error() << "Could not read device-mapper table for DM device: "
                   << device_name;
  }
  if (table.size() != 1) {
    return Error() << "Unexpected table info(size=" << table.size()
                   << ", expected=1) for DM device: " << device_name;
  }
  const auto& entry = table[0].spec;
  auto target_type = DeviceMapper::GetTargetType(entry);
  if (expected_type != target_type) {
    return Error() << "Unexpected table type (" << target_type
                   << ") for DM device: " << device_name
                   << " (expected: " << expected_type << ")";
  }
  return {};
}

// This is not the right place to do this normalization, but proper solution
// will require some refactoring first. :(
// TODO(b/158469911): introduce MountedApexDataBuilder and delegate all
//  building/normalization logic to it.
void NormalizeIfDeleted(MountedApexData* apex_data) {
  std::string_view full_path = apex_data->full_path;
  if (ConsumeSuffix(&full_path, "(deleted)")) {
    apex_data->deleted = true;
    auto it = full_path.rbegin();
    while (it != full_path.rend() && isspace(*it)) {
      it++;
    }
    full_path.remove_suffix(it - full_path.rbegin());
  } else {
    apex_data->deleted = false;
  }
  apex_data->full_path = full_path;
}

Result<MountedApexData> ResolveMountInfo(const BlockDevice& block,
                                         const std::string& mount_point) {
  MountedApexData result;
  result.mount_point = mount_point;

  // Now, see if it is dm-verity or loop mounted
  switch (block.GetType()) {
    case LoopDevice: {
      result.loop_name = block.DevPath();
      result.full_path = OR_RETURN(block.GetProperty("loop/backing_file"));
    } break;
    case DeviceMapperDevice: {
      result.verity_name = OR_RETURN(block.GetProperty("dm/name"));
      OR_RETURN(ValidateDm(result.verity_name, "verity"));
      auto underlying = OR_RETURN(GetUnderlying(block));
      switch (underlying.GetType()) {
        case LoopDevice: {
          result.loop_name = underlying.DevPath();
          result.full_path =
              OR_RETURN(underlying.GetProperty("loop/backing_file"));
        } break;
        case DeviceMapperDevice: {
          result.linear_name = OR_RETURN(underlying.GetProperty("dm/name"));
          OR_RETURN(ValidateDm(result.linear_name, "linear"));
          result.full_path = OR_RETURN(GetUnderlying(underlying)).DevPath();
        } break;
        default:
          return Error() << "Unknown underlying device type for dm-verity:"
                         << underlying.DevPath();
      }
    } break;
    case UnknownDevice: {
      return Errorf("Can't resolve {}", block.DevPath().string());
    }
  }

  NormalizeIfDeleted(&result);
  return result;
}

}  // namespace

// Parses active APEX mounts from /proc/mounts and populates the DB.
//
// /apex/<package-id> can be mounted from
// - /dev/block/loopX : loop device
// - /dev/block/dm-X : dm-verity
//
// (For more information about APEX mounts, please refer to MountPackageImpl())
//
// In case of loop device, the original APEX file can be tracked
// by /sys/block/loopX/loop/backing_file.
//
// In case of dm-verity, its underlying block device can be
// either a loop device or a dm-linear device:
// - Loop device is backed by an APEX file (e.g. /data/apex/active/foo.apex)
// - Dm-linear device is created on top of another dm-linear device which
//   represents the APEX file
//
// Need to read /proc/mounts on startup since apexd can start
// at any time (It's a lazy service).
void MountedApexDatabase::PopulateFromMounts()
    REQUIRES(!mounted_apexes_mutex_) {
  LOG(INFO) << "Populating APEX database from mounts...";

  std::ifstream mounts("/proc/mounts");
  std::string line;
  std::lock_guard lock(mounted_apexes_mutex_);
  while (std::getline(mounts, line)) {
    auto [block, mount_point] = ParseMountInfo(line);
    // TODO(b/158469914): distinguish between temp and non-temp mounts
    if (fs::path(mount_point).parent_path() != kApexRoot) {
      continue;
    }
    if (IsActiveMountPoint(mount_point)) {
      continue;
    }
    if (IsTempMountPoint(mount_point)) {
      continue;
    }
    auto mount_data = ResolveMountInfo(BlockDevice(block), mount_point);
    if (!mount_data.ok()) {
      LOG(WARNING) << "Can't resolve mount info " << mount_data.error();
      continue;
    }

    auto [package, version] = ParseMountPoint(mount_point);
    mount_data->version = version;
    LOG(INFO) << "Found " << mount_point << " backed by"
              << (mount_data->deleted ? " deleted " : " ") << "file "
              << mount_data->full_path;
    AddMountedApexLocked(package, std::move(*mount_data));
  }
  LOG(INFO) << mounted_apexes_.size() << " packages restored.";
}

}  // namespace apex
}  // namespace android
