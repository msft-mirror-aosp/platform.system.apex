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

#include "apex_blocklist.h"

#include <android-base/file.h>
#include <google/protobuf/util/json_util.h>

#include <memory>
#include <string>

using android::base::Error;
using android::base::Result;
using ::apex::proto::ApexBlocklist;

namespace android::apex {

Result<ApexBlocklist> ParseBlocklist(const std::string& content) {
  ApexBlocklist apex_blocklist;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  auto parse_result = google::protobuf::util::JsonStringToMessage(
      content, &apex_blocklist, options);
  if (!parse_result.ok()) {
    return Error() << "Can't parse APEX blocklist: " << parse_result.message();
  }

  for (const auto& apex : apex_blocklist.blocked_apex()) {
    // Verifying required fields.
    // name
    if (apex.name().empty()) {
      return Error() << "Missing required field \"name\" from APEX blocklist.";
    }

    // version
    if (apex.version() <= 0) {
      return Error() << "Missing positive value for field \"version\" "
                        "from APEX blocklist.";
    }
  }

  return apex_blocklist;
}

Result<ApexBlocklist> ReadBlocklist(const std::string& path) {
  std::string content;
  if (!android::base::ReadFileToString(path, &content)) {
    return Error() << "Failed to read blocklist file: " << path;
  }
  return ParseBlocklist(content);
}

}  // namespace android::apex
