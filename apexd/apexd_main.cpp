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

#define LOG_TAG "apexd"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <selinux/android.h>
#include <strings.h>
#include <sys/stat.h>

#include <memory>

#include "apex_file_repository.h"
#include "apexd.h"
#include "apexd_checkpoint_vold.h"
#include "apexd_image_manager.h"
#include "apexd_lifecycle.h"
#include "apexd_metrics_stats.h"
#include "apexservice.h"
#include "com_android_apex_flags.h"

namespace flags = com::android::apex::flags;

namespace {

using android::base::SetDefaultTag;

int HandleSubcommand(int argc, char** argv) {
  if (strcmp("--bootstrap", argv[1]) == 0) {
    SetDefaultTag("apexd-bootstrap");
    return android::apex::OnBootstrap();
  }

  if (strcmp("--unmount-all", argv[1]) == 0) {
    SetDefaultTag("apexd-unmount-all");
    return android::apex::UnmountAll();
  }

  if (strcmp("--otachroot-bootstrap", argv[1]) == 0) {
    SetDefaultTag("apexd-otachroot");
    bool also_include_staged_apexes =
        argc >= 3 && strcmp("--also-include-staged-apexes", argv[2]) == 0;
    return android::apex::OnOtaChrootBootstrap(also_include_staged_apexes);
  }

  if (strcmp("--snapshotde", argv[1]) == 0) {
    SetDefaultTag("apexd-snapshotde");
    // Need to know if checkpointing is enabled so that a prerestore snapshot
    // can be taken if it's not.
    android::base::Result<android::apex::VoldCheckpointInterface>
        vold_service_st = android::apex::VoldCheckpointInterface::Create();
    if (!vold_service_st.ok()) {
      LOG(ERROR) << "Could not retrieve vold service: "
                 << vold_service_st.error();
    } else {
      android::apex::InitializeVold(&*vold_service_st);
    }

    int result = android::apex::SnapshotOrRestoreDeUserData();

    if (result == 0) {
      // Notify other components (e.g. init) that all APEXs are ready to be used
      // Note that it's important that the binder service is registered at this
      // point, since other system services might depend on it.
      android::apex::OnAllPackagesReady();
    }
    return result;
  }

  if (strcmp("--dump", argv[1]) == 0) {
    SetDefaultTag("apexd-dump");
    return android::apex::OnDump(
        std::vector<std::string>{argv + 2, argv + argc});
  }

  if (strcmp("--vm", argv[1]) == 0) {
    SetDefaultTag("apexd-vm");
    return android::apex::OnStartInVmMode();
  }

  LOG(ERROR) << "Unknown subcommand: " << argv[1];
  return 1;
}

void InstallSigtermSignalHandler() {
  struct sigaction action = {};
  action.sa_handler = [](int /*signal*/) {
    // Handle SIGTERM gracefully.
    // By default, when SIGTERM is received a process will exit with non-zero
    // exit code, which will trigger reboot_on_failure handler if one is
    // defined. This doesn't play well with userspace reboot which might
    // terminate apexd with SIGTERM if apexd was running at the moment of
    // userspace reboot, hence this custom handler to exit gracefully.
    _exit(0);
  };
  sigaction(SIGTERM, &action, nullptr);
}

void InstallSelinuxLogging() {
  union selinux_callback cb;
  cb.func_log = selinux_log_callback;
  selinux_set_callback(SELINUX_CB_LOG, cb);
}

}  // namespace

int main(int argc, char** argv) {
  android::base::InitLogging(argv, &android::base::KernelLogger);
  // TODO(b/158468454): add a -v flag or an external setting to change severity.
  android::base::SetMinimumLogSeverity(android::base::INFO);

  const bool has_subcommand = argv[1] != nullptr;
  LOG(INFO) << "Started. subcommand = "
            << (has_subcommand ? argv[1] : "(null)");

  // set umask to 022 so that files/dirs created are accessible to other
  // processes e.g.) /apex/apex-info-list.xml is supposed to be read by other
  // processes
  umask(022);

  // In some scenarios apexd needs to adjust the selinux label of the files.
  // Install the selinux logging callback so that we can catch potential errors.
  InstallSelinuxLogging();

  InstallSigtermSignalHandler();

  auto config = android::apex::kDefaultConfig;
  if constexpr (flags::mount_before_data()) {
    config.uses_pinned_apex = true;
    if (android::base::GetIntProperty("ro.init.mnt_ns.count", 2) == 1) {
      config.mount_before_data = true;
    }
  }
  android::apex::SetConfig(config);

  // The sysprop flag `apexd.config.brand_new_apex` is used to control the
  // feature usage in individual targets.
  if (android::base::GetBoolProperty("apexd.config.brand_new_apex", false)) {
    android::apex::ApexFileRepository::EnableBrandNewApex();
  }

  android::apex::ApexdLifecycle& lifecycle =
      android::apex::ApexdLifecycle::GetInstance();
  bool booting = lifecycle.IsBooting();

  auto image_manager = android::apex::ApexImageManager::Create(
      android::apex::kMetadataImagesDir, android::apex::kDataImagesDir);
  android::apex::InitializeImageManager(image_manager.get());

  auto session_manager = android::apex::ApexSessionManager::Create(
      android::apex::GetSessionsDir());
  android::apex::InitializeSessionManager(session_manager.get());

  if (has_subcommand) {
    return HandleSubcommand(argc, argv);
  }

  android::base::Result<android::apex::VoldCheckpointInterface>
      vold_service_st = android::apex::VoldCheckpointInterface::Create();
  android::apex::VoldCheckpointInterface* vold_service = nullptr;
  if (!vold_service_st.ok()) {
    LOG(ERROR) << "Could not retrieve vold service: "
               << vold_service_st.error();
  } else {
    vold_service = &*vold_service_st;
  }
  android::apex::Initialize(vold_service);
  android::apex::InitMetrics(std::make_unique<android::apex::StatsLog>());

  if (booting) {
    android::apex::OnStart();
  }
  // start apexservice before ApexdLifecycle::WaitForBootStatus which waits for
  // IApexService::markBootComplete().
  android::apex::binder::CreateAndRegisterService();
  android::apex::binder::StartThreadPool();

  if (booting) {
    // Notify other components (e.g. init) that all APEXs are correctly mounted
    // and activated (but are not yet ready to be used). Configuration based on
    // activated APEXs may be performed at this point, but use of APEXs
    // themselves should wait for the ready status instead, which is set when
    // the "--snapshotde" subcommand is received and snapshot/restore is
    // complete.
    android::apex::OnAllPackagesActivated();
    lifecycle.WaitForBootStatus(session_manager->HasActiveSession());
    // Run cleanup routine on boot complete.
    // This should run before AllowServiceShutdown() to prevent
    // service_manager killing apexd in the middle of the cleanup.
    android::apex::BootCompletedCleanup();
  }

  android::apex::binder::AllowServiceShutdown();

  android::apex::binder::JoinThreadPool();
  return 1;
}
