/*
 * Copyright (C) 2022 The Android Open Source Project
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

// Entry for microdroid-specific apexd. This should be kept as minimal as
// possible.

#include "apex_constants.h"
#define LOG_TAG "apexd-vm"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <sys/stat.h>
#include <unistd.h>  // For getpagesize()

#include "apexd.h"
#include "apexd_utils.h"
#include "com_android_apex_flags.h"

static bool IsFileBackedMountSupported() {
  return com::android::apex::flags::erofs_file_backed_mount() &&
         getpagesize() == 4096 && android::apex::IsKernelAtLeast(6, 12);
}

[[clang::no_destroy]] static const android::apex::ApexdConfig
    kMicrodroidConfig = {
        android::apex::kApexStatusSysprop,
        nullptr, /* apexd_changed_active_apexes_sysprop */
        {{android::apex::ApexPartition::System,
          android::apex::kApexPackageSystemDir}},
        // A bunch of things are not used in Microdroid, hence we use nullptr
        // instead of an actual value.
        nullptr, /* active_apex_data_dir */
        nullptr, /* decompression_dir */
        nullptr, /* ota_reserved_dir */
        nullptr, /* staged_session_dir */
        android::apex::kVmPayloadMetadataPartitionProp,
        nullptr,                      /* active_apex_selinux_ctx */
        {},                           /* brand_new_apex_config_dirs */
        nullptr,                      /* checkpoint_file */
        false,                        /* mount_before_data */
        false,                        /* uses_pinned_apex */
        IsFileBackedMountSupported(), /* file_backed_mount */
        nullptr,                      /* metadata_config_dir */
};

int main(int argc, char** argv) {
  android::base::InitLogging(argv);
  android::base::SetMinimumLogSeverity(android::base::INFO);

  // set umask to 022 so that files/dirs created are accessible to other
  // processes e.g.) /apex/apex-info-list.xml is supposed to be read by other
  // processes
  umask(022);

  android::apex::SetConfig(kMicrodroidConfig);

  if (android::base::GetBoolProperty("ro.debuggable", false)) {
    if (argc >= 2 && strcmp(argv[1], "--dump") == 0) {
      return android::apex::OnDump(
          std::vector<std::string>{argv + 2, argv + argc});
    }
  }
  return android::apex::OnStartInVmMode();
}
