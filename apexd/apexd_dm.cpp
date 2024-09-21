/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "apexd_dm.h"

#include <ApexProperties.sysprop.h>
#include <android-base/logging.h>
#include <utils/Trace.h>

using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;

namespace android::apex {

DmDevice::~DmDevice() {
  if (!cleared_) {
    Result<void> ret = DeleteDmDevice(name_, /* deferred= */ false);
    if (!ret.ok()) {
      LOG(ERROR) << ret.error();
    }
  }
}

static Result<DmDevice> CreateDmDeviceInternal(
    DeviceMapper& dm, const std::string& name, const DmTable& table,
    const std::chrono::milliseconds& timeout) {
  std::string dev_path;
  if (!dm.CreateDevice(name, table, &dev_path, timeout)) {
    return Error() << "Couldn't create dm-device.";
  }
  return DmDevice(name, dev_path);
}

Result<DmDevice> CreateDmDevice(const std::string& name, const DmTable& table,
                                bool reuse_device) {
  ATRACE_NAME("CreateDmDevice");
  LOG(VERBOSE) << "Creating dm-device " << name;

  auto timeout = std::chrono::milliseconds(
      android::sysprop::ApexProperties::dm_create_timeout().value_or(1000));

  DeviceMapper& dm = DeviceMapper::Instance();

  auto state = dm.GetState(name);
  if (state == DmDeviceState::INVALID) {
    return CreateDmDeviceInternal(dm, name, table, timeout);
  }

  if (reuse_device) {
    if (state == DmDeviceState::ACTIVE) {
      LOG(WARNING) << "Deleting existing active dm-device " << name;
      OR_RETURN(DeleteDmDevice(name, /* deferred= */ false));
      return CreateDmDeviceInternal(dm, name, table, timeout);
    }
    if (!dm.LoadTableAndActivate(name, table)) {
      dm.DeleteDevice(name);
      return Error() << "Failed to activate dm-device " << name;
    }
    std::string path;
    if (!dm.WaitForDevice(name, timeout, &path)) {
      dm.DeleteDevice(name);
      return Error() << "Failed waiting for dm-device " << name;
    }
    return DmDevice(name, path);
  } else {
    // Delete dangling dm-device. This can happen if apexd fails to delete it
    // while unmounting an apex.
    LOG(WARNING) << "Deleting existing dm-device " << name;
    OR_RETURN(DeleteDmDevice(name, /* deferred= */ false));
    return CreateDmDeviceInternal(dm, name, table, timeout);
  }
}

// Deletes a device-mapper device with a given name and path
// Synchronizes on the device actually being deleted from userspace.
Result<void> DeleteDmDevice(const std::string& name, bool deferred) {
  DeviceMapper& dm = DeviceMapper::Instance();
  if (deferred) {
    if (!dm.DeleteDeviceDeferred(name)) {
      return ErrnoError() << "Failed to issue deferred delete of dm-device "
                          << name;
    }
    return {};
  }
  auto timeout = std::chrono::milliseconds(
      android::sysprop::ApexProperties::dm_delete_timeout().value_or(750));
  if (!dm.DeleteDevice(name, timeout)) {
    return Error() << "Failed to delete dm-device " << name;
  }
  return {};
}

}  // namespace android::apex