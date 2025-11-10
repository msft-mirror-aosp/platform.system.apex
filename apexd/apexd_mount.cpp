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

#define LOG_TAG "apexd"

#include "apexd_mount.h"

#include <android-base/parsebool.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>

#include "apexd_utils.h"

namespace android::apex {

// The property decides whether to use file-backed mount. Device makers can set
// the prop if they're certain that the device supports or doesn't support
// file-backed mount. If not set, apexd will perform a test mount and use the
// result to determine the appropriate action.
static constexpr const char* kFileBackedMountProp =
    "apexd.config.erofs_file_backed_mount";

// The property stores the results of apexd doing test file-backed mount at
// runtime. It'll only be used if kFileBackedMountProp is not set.
static constexpr const char* kFileBackedMountRuntimeProp =
    "apexd.config.runtime.erofs_file_backed_mount";

// The folder is for apexd to test mount an image. If not deleted successfully,
// it could be mistaken for an APEX mount by other system components. To prevent
// this, the folder is named using the following rules:
// . prefix: a hidden entry
// @: indicating this directory as a non canonical APEX mount
// .tmp suffix: this is a temporary mount
static constexpr const char* kApexTestMountFolder = "/apex/.test@0.tmp";
static constexpr const char* kTestMountImage =
    "/system/etc/apexd/empty_erofs.img";

bool GetFileBackedMountEnabled() {
  auto enabled = android::base::GetProperty(kFileBackedMountProp, "");
  if (enabled != "") {
    return android::base::ParseBool(enabled) ==
           android::base::ParseBoolResult::kTrue;
  }

  enabled = android::base::GetProperty(kFileBackedMountRuntimeProp, "");
  if (enabled != "") {
    return android::base::ParseBool(enabled) ==
           android::base::ParseBoolResult::kTrue;
  }

  // Test mount to see if the device supports file-backed mount by specifying
  // `fsoffset=` when mounting
  auto create_dir_result = CreateDirIfNeeded(kApexTestMountFolder, 0755);
  if (!create_dir_result.ok()) {
    LOG(ERROR) << "Failed to create the folder for test mounting "
               << kApexTestMountFolder
               << " , error: " << create_dir_result.error();
    android::base::SetProperty(kFileBackedMountRuntimeProp, "false");
    return false;
  }

  auto cleanup_mnt_folder = android::base::make_scope_guard([&]() {
    auto delete_result = DeleteDir(kApexTestMountFolder);
    if (!delete_result.ok()) {
      LOG(ERROR) << "Failed to delete the folder for test mounting "
                 << kApexTestMountFolder
                 << " , error: " << delete_result.error();
    }
  });

  uint32_t mount_flags = MS_NOATIME | MS_NODEV | MS_DIRSYNC | MS_RDONLY;
  if (mount(kTestMountImage, kApexTestMountFolder, "erofs", mount_flags,
            "fsoffset=0")) {
    android::base::SetProperty(kFileBackedMountRuntimeProp, "false");
    return false;
  }

  android::base::SetProperty(kFileBackedMountRuntimeProp, "true");

  // Try to umount. It returns error if not mounted (e.g. mount failed),
  // which is fine.
  if (umount2(kApexTestMountFolder, MNT_DETACH)) {
    PLOG(ERROR) << "Failed to umount " << kApexTestMountFolder;
  }

  return true;
}

}  // namespace android::apex
