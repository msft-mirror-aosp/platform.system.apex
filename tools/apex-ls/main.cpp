//
// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/result.h>
#include <android-base/unique_fd.h>
#include <apex_file.h>
#include <fcntl.h>
#include <sys/sendfile.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "erofs.h"
#include "ext4.h"
#include "list.h"

using android::apex::ApexFile;
using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::base::unique_fd;

using namespace std::string_literals;

struct Args {
  std::string apex_file;
  bool show_security_context;
};

Result<void> PrintList(Args args) {
  auto apex_file = OR_RETURN(ApexFile::Open(args.apex_file));

  // If the apex is .capex, decompress it first.
  TemporaryDir temp_dir;
  if (apex_file.IsCompressed()) {
    auto original_apex_path = std::string(temp_dir.path) + "/original.apex";
    OR_RETURN(apex_file.Decompress(original_apex_path));
    apex_file = OR_RETURN(ApexFile::Open(original_apex_path));
  }

  if (!apex_file.GetFsType().has_value())
    return Error() << "Invalid apex: no fs type";
  if (!apex_file.GetImageSize().has_value())
    return Error() << "Invalid apex: no image size";
  if (!apex_file.GetImageOffset().has_value())
    return Error() << "Invalid apex: no image offset";

  std::map lister = {
      std::pair{"ext4"s, &Ext4List},
      std::pair{"erofs"s, &ErofsList},
  };
  auto fs_type = *apex_file.GetFsType();
  if (!lister.contains(fs_type)) {
    return Error() << "Invalid filesystem type: " << fs_type;
  }

  // Extract apex_payload.img
  TemporaryFile temp_file;
  {
    unique_fd src(open(apex_file.GetPath().c_str(), O_RDONLY | O_CLOEXEC));
    off_t offset = *apex_file.GetImageOffset();
    size_t size = *apex_file.GetImageSize();
    if (sendfile(temp_file.fd, src, &offset, size) < 0) {
      return ErrnoError()
             << "Failed to create a temporary file for apex payload";
    }
  }

  for (const auto& entry : OR_RETURN(lister[fs_type](temp_file.path))) {
    std::cout << entry.path;
    if (args.show_security_context) {
      std::cout << " " << entry.security_context;
    }
    std::cout << "\n";
  }
  return {};
}

Result<Args> ParseArgs(std::vector<std::string> args) {
  if (args.size() == 2) {
    return Args{
        .apex_file = args[1],
        .show_security_context = false,
    };
  }
  if (args.size() == 3 && args[1] == "-Z") {
    return Args{
        .apex_file = args[2],
        .show_security_context = true,
    };
  }
  return Error() << "Invalid args\n"
                 << "usage: " << args[0] << " [-Z] APEX_FILE\n";
}

Result<void> TryMain(std::vector<std::string> args) {
  auto parse_args = OR_RETURN(ParseArgs(args));
  OR_RETURN(PrintList(parse_args));
  return {};
}

int main(int argc, char** argv) {
  android::base::SetMinimumLogSeverity(android::base::ERROR);
  if (auto st = TryMain(std::vector<std::string>{argv, argv + argc});
      !st.ok()) {
    std::cerr << st.error() << "\n";
    return 1;
  }
  return 0;
}
