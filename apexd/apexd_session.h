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

#ifndef ANDROID_APEXD_APEXD_SESSION_H_
#define ANDROID_APEXD_APEXD_SESSION_H_

#include <android-base/result.h>

#include "apex_constants.h"

#include "session_state.pb.h"

#include <optional>

namespace android {
namespace apex {

// apexd uses the /metadata partition (kApexSessionsDir) as
// location for sessions-related information.
static constexpr const char* kApexSessionsDir = "/metadata/apex/sessions";

// Returns top-level directory to store sessions metadata in.
std::string GetSessionsDir();

// TODO(b/288309411): remove static functions in this class.
class ApexSession {
 public:
  ApexSession() = delete;

  const google::protobuf::RepeatedField<int> GetChildSessionIds() const;
  ::apex::proto::SessionState::State GetState() const;
  int GetId() const;
  const std::string& GetBuildFingerprint() const;
  const std::string& GetCrashingNativeProcess() const;
  const std::string& GetErrorMessage() const;
  bool IsFinalized() const;
  bool HasRollbackEnabled() const;
  bool IsRollback() const;
  int GetRollbackId() const;
  const google::protobuf::RepeatedPtrField<std::string> GetApexNames() const;
  const google::protobuf::RepeatedPtrField<std::string> GetApexFileHashes()
      const;
  const std::string& GetSessionDir() const;

  void SetChildSessionIds(const std::vector<int>& child_session_ids);
  void SetBuildFingerprint(const std::string& fingerprint);
  void SetHasRollbackEnabled(const bool enabled);
  void SetIsRollback(const bool is_rollback);
  void SetRollbackId(const int rollback_id);
  void SetCrashingNativeProcess(const std::string& crashing_process);
  void SetErrorMessage(const std::string& error_message);
  void AddApexName(const std::string& apex_name);
  void SetApexFileHashes(const std::vector<std::string>& hashes);

  android::base::Result<void> UpdateStateAndCommit(
      const ::apex::proto::SessionState::State& state);

  android::base::Result<void> DeleteSession() const;

  // Returns the directories containing the apexes staged for this session.
  std::vector<std::string> GetStagedApexDirs(
      const std::string& staged_session_dir) const;

  friend class ApexSessionManager;

 private:
  ApexSession(::apex::proto::SessionState state, std::string session_dir);
  ::apex::proto::SessionState state_;
  std::string session_dir_;
};

class ApexSessionManager {
 public:
  static std::unique_ptr<ApexSessionManager> Create(
      std::string sessions_base_dir);

  android::base::Result<ApexSession> CreateSession(int session_id);
  android::base::Result<ApexSession> GetSession(int session_id) const;
  std::vector<ApexSession> GetSessions() const;
  std::vector<ApexSession> GetSessionsInState(
      const ::apex::proto::SessionState::State& state) const;

  bool HasActiveSession();
  void DeleteFinalizedSessions();

 private:
  explicit ApexSessionManager(std::string sessions_base_dir);
  ApexSessionManager(const ApexSessionManager&) = delete;
  ApexSessionManager& operator=(const ApexSessionManager&) = delete;
  ApexSessionManager(ApexSessionManager&&) = delete;
  ApexSessionManager& operator=(ApexSessionManager&&) = delete;

  std::string sessions_base_dir_;
};

std::ostream& operator<<(std::ostream& out, const ApexSession& session);

}  // namespace apex
}  // namespace android

#endif  // ANDROID_APEXD_APEXD_SESSION_H
