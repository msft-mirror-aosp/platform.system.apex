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

#define LOG_TAG "apexd-dump"

#include <libdm/dm.h>

#include <format>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

#include "apex_constants.h"
#include "apexd.h"
#include "apexd_session.h"

namespace android::apex {

namespace {

std::string JoinValues(const auto& values) {
  std::ostringstream s;
  for (const auto& value : values) {
    s << value << ",";
  }
  std::string result = s.str();
  if (!result.empty()) {
    result.pop_back();
  }
  return result;
}

void DumpConfig(std::ostream& out) {
  const auto& config = GetConfig();
  out << "config:";
  out << " mount_before_data=" << std::boolalpha << config.mount_before_data;
  out << " uses_pinned_apex=" << std::boolalpha << config.uses_pinned_apex;
  out << "\n";
}

void DumpSessions(std::ostream& out) {
  auto manager = ApexSessionManager::Create(GetSessionsDir());
  for (const auto& session : manager->GetSessions()) {
    out << "session:";
    out << " id=" << session.GetId();
    out << " state=" << SessionState_State_Name(session.GetState());
    if (!session.GetChildSessionIds().empty()) {
      out << " child_session_ids=" << JoinValues(session.GetChildSessionIds());
    }
    out << " build_fingerprint=" << session.GetBuildFingerprint();
    out << " apex_names=" << JoinValues(session.GetApexNames());
    if (session.HasRollbackEnabled()) {
      out << " rollback_enabled=true";
    }
    if (session.IsRollback()) {
      out << " is_rollback=true";
      out << " rollback_id=" << session.GetRollbackId();
    }
    if (!session.GetCrashingNativeProcess().empty()) {
      out << " crashing_native_process=" << session.GetCrashingNativeProcess();
    }
    if (!session.GetErrorMessage().empty()) {
      out << " error_message=" << session.GetErrorMessage();
    }
    if (!session.GetApexImages().empty()) {
      out << " apex_images=" << JoinValues(session.GetApexImages());
    }
    out << " dir=" << session.GetSessionDir();
    out << "\n";
  }
}

void DumpMounts(std::ostream& out) {
  auto& dm = dm::DeviceMapper::Instance();
  auto dump_dm = [&](const std::string& type, const std::string& name) {
    std::string path;
    if (!dm.GetDmDevicePathByName(name, &path)) {
      path = "(err)";
    }
    out << std::format(" {}={}({})", type, path, name);
  };

  MountedApexDatabase db;
  db.PopulateFromMounts();
  db.ForallMountedApexes([&](auto, const auto& data, auto) {
    out << "mount:";
    out << " mount_point=" << data.mount_point;
    if (!data.verity_name.empty()) dump_dm("verity", data.verity_name);
    if (!data.linear_name.empty()) dump_dm("linear", data.linear_name);
    if (!data.loop_name.empty()) out << " loop=" << data.loop_name;
    out << " apex=" << data.full_path;
    out << "\n";
  });
}

}  // namespace

int OnDump(const std::vector<std::string>& args) {
  bool dump_all = args.empty();
  bool dump_config = std::ranges::contains(args, "config");
  bool dump_sessions = std::ranges::contains(args, "sessions");
  bool dump_mounts = std::ranges::contains(args, "mounts");

  if (dump_all || dump_config) {
    DumpConfig(std::cout);
  }
  if (dump_all || dump_sessions) {
    DumpSessions(std::cout);
  }
  if (dump_all || dump_mounts) {
    DumpMounts(std::cout);
  }

  // TODO(b/432328407)
  // repository
  // images
  // metadata
  // flags
  return 0;
}

}  // namespace android::apex