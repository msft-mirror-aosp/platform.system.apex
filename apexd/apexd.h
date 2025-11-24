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

#ifndef ANDROID_APEXD_APEXD_H_
#define ANDROID_APEXD_APEXD_H_

#include <android-base/macros.h>
#include <android-base/result.h>

#include <ostream>
#include <string>
#include <vector>

#include "apex_classpath.h"
#include "apex_constants.h"
#include "apex_database.h"
#include "apex_file.h"
#include "apex_file_repository.h"
#include "apexd_session.h"

namespace android {
namespace apex {

// A structure containing all the values that might need to be injected for
// testing (e.g. apexd status property, etc.)
//
// Ideally we want to introduce Apexd class and use dependency injection for
// such values, but that will require a sizeable refactoring. For the time being
// this config should do the trick.
struct ApexdConfig {
  const char* apex_status_sysprop;
  const char* apexd_changed_active_apexes_sysprop;
  std::unordered_map<ApexPartition, std::string> builtin_dirs;
  const char* active_apex_data_dir;
  const char* decompression_dir;
  const char* ota_reserved_dir;
  const char* staged_session_dir;
  // Overrides the path to the "metadata" partition which is by default
  // /dev/block/by-name/payload-metadata It should be a path pointing the first
  // partition of the VM payload disk. So, realpath() of this path is checked if
  // it has the suffix "1". For example, /test-dir/test-metadata-1 can be valid
  // and the subsequent numbers should point APEX files.
  const char* vm_payload_metadata_partition_prop;
  const char* active_apex_selinux_ctx;

  std::unordered_map<ApexPartition, std::string> brand_new_apex_config_dirs;

  // Path to the checkpoint file managed by vold: /metadata/vold/checkpoint
  const char* checkpoint_file;

  // True if ALL apexes can be mounted in apexd-bootstrap (before /data)
  bool mount_before_data;
  // True if APEXes are pinned using ApexImageManager on installation.
  bool uses_pinned_apex;
  // True if preinstalled EROFS APEXes can be file-backed mount.
  bool file_backed_mount;
  const char* metadata_config_dir;
};

static const ApexdConfig kDefaultConfig = {
    kApexStatusSysprop,
    kApexdChangedActiveApexesSysprop,
    kBuiltinApexPackageDirs,
    kActiveApexPackagesDataDir,
    kApexDecompressedDir,
    kOtaReservedDir,
    kStagedSessionsDir,
    kVmPayloadMetadataPartitionProp,
    "u:object_r:staging_data_file",
    kBrandNewApexConfigDirs,
    kCheckpointFile,
    false, /* mount_before_data */
    false, /* uses_pinned_apex */
    false, /* file_backed_mount */
    kMetadataConfigDir,
};

class CheckpointInterface;

void SetConfig(const ApexdConfig& config);
const ApexdConfig& GetConfig();

// Exposed only for testing.
android::base::Result<void> Unmount(
    const MountedApexDatabase::MountedApexData& data, bool deferred);

android::base::Result<void> ResumeRevertIfNeeded();

android::base::Result<void> StagePackages(
    const std::vector<std::string>& tmpPaths) WARN_UNUSED;
android::base::Result<void> UnstagePackages(
    const std::vector<std::string>& paths) WARN_UNUSED;

android::base::Result<std::vector<ApexFile>> SubmitStagedSession(
    const int session_id, const std::vector<int>& child_session_ids,
    const bool has_rollback_enabled, const bool is_rollback,
    const int rollback_id) WARN_UNUSED;
android::base::Result<std::vector<ApexFile>> GetStagedApexFiles(
    const int session_id,
    const std::vector<int>& child_session_ids) WARN_UNUSED;
android::base::Result<ClassPath> MountAndDeriveClassPath(
    const std::vector<ApexFile>&) WARN_UNUSED;
android::base::Result<void> MarkStagedSessionReady(const int session_id)
    WARN_UNUSED;
android::base::Result<void> MarkStagedSessionSuccessful(const int session_id)
    WARN_UNUSED;
// Only only of the parameters should be passed during revert
android::base::Result<void> RevertActiveSessions(
    const std::string& crashing_native_process,
    const std::string& error_message);
// Only only of the parameters should be passed during revert
android::base::Result<void> RevertActiveSessionsAndReboot(
    const std::string& crashing_native_process,
    const std::string& error_message);

android::base::Result<void> ActivatePackage(const std::string& full_path)
    WARN_UNUSED;
android::base::Result<void> DeactivatePackage(const std::string& full_path)
    WARN_UNUSED;

android::base::Result<void> BackupActiveApexes();

std::vector<ApexFile> GetActivePackages();

std::vector<ApexFile> GetFactoryPackages();

android::base::Result<void> AbortStagedSession(const int session_id);

android::base::Result<void> SnapshotCeData(const int user_id,
                                           const int rollback_id,
                                           const std::string& apex_name);
android::base::Result<void> RestoreCeData(const int user_id,
                                          const int rollback_id,
                                          const std::string& apex_name);

android::base::Result<void> DestroyDeSnapshots(const int rollback_id);
android::base::Result<void> DestroyCeSnapshots(const int user_id,
                                               const int rollback_id);
android::base::Result<void> DestroyCeSnapshotsNotSpecified(
    int user_id, const std::vector<int>& retain_rollback_ids);

int OnBootstrap();
// Sets the values of gVoldService and gInFsCheckpointMode.
void InitializeVold(CheckpointInterface* checkpoint_service);
// Sets the value of gSessionManager.
void InitializeSessionManager(ApexSessionManager* session_manager);
// Initializes in-memory state (e.g. pre-installed data, activated apexes).
// Must be called first before calling any other boot sequence related function.
void Initialize(CheckpointInterface* checkpoint_service);
// Apex activation logic. Scans staged apex sessions and activates apexes.
// Must only be called during boot (i.e apexd.status is not "ready" or
// "activated").
void OnStart();

int OnDump(const std::vector<std::string>& args);

android::base::Result<ApexFile> ProcessCompressedApex(const ApexFile& capex,
                                                      bool is_ota_chroot);
// Validate |apex| is same as |capex|
android::base::Result<void> ValidateDecompressedApex(const ApexFile& capex,
                                                     const ApexFile& apex);
// Notifies system that apexes are activated by setting apexd.status property to
// "activated".
// Must only be called during boot (i.e. apexd.status is not "ready" or
// "activated").
void OnAllPackagesActivated();
// Notifies system that apexes are ready by setting apexd.status property to
// "ready".
// Must only be called during boot (i.e. apexd.status is not "ready" or
// "activated").
void OnAllPackagesReady();
void MarkBootCompleted();

// Removes inactivate apexes on /data after activation.
// This can happen when prebuilt APEXes are newer than /data apexes with OTA.
// Exposed for testing.
void RemoveInactiveDataApex();

void BootCompletedCleanup();
int SnapshotOrRestoreDeUserData();

// Unmounts all apex mounts from /proc/mounts
int UnmountAll();

// Exposed for unit tests
bool ShouldAllocateSpaceForDecompression(const std::string& new_apex_name,
                                         int64_t new_apex_version,
                                         const ApexFileRepository& instance,
                                         const MountedApexDatabase& db);

int64_t CalculateSizeForCompressedApex(
    const std::vector<std::tuple<std::string, int64_t, int64_t>>&
        compressed_apexes);

// Exposed for benchmark
void EmitApexInfoList(const std::vector<ApexFileRef>& active,
                      bool is_bootstrap);
void CollectApexInfoList(std::ostream& os,
                         const std::vector<ApexFileRef>& active_apexs,
                         const std::vector<ApexFileRef>& inactive_apexs);

// Reserve |size| bytes in |dest_dir| by creating a zero-filled file
android::base::Result<void> ReserveSpaceForCompressedApex(
    int64_t size, const std::string& dest_dir);

// Entry point when running in the VM mode (with --vm arg)
int OnStartInVmMode();

// Activates apexes in otapreot_chroot environment.
// If `also_include_staged_apexes` is true, it's for Pre-reboot Dexopt.
int OnOtaChrootBootstrap(bool also_include_staged_apexes);

android::apex::MountedApexDatabase& GetApexDatabaseForTesting();

// Performs a non-staged install of an APEX specified by |package_path|.
// TODO(ioffe): add more documentation.
android::base::Result<ApexFile> InstallPackage(const std::string& package_path,
                                               bool force);

std::set<std::string> GetChangedActiveApexes();

// Supposed to be called only once in OnBootstrap() or OnStart() to set the
// ro.apexd.changed_active_apexes property.
void SaveChangedActiveApexes(
    const std::set<std::string>& changed_active_apexes);

ApexSessionManager* GetSessionManager();

}  // namespace apex
}  // namespace android

#endif  // ANDROID_APEXD_APEXD_H_
