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

#include <android-base/file.h>
#include <android-base/logging.h>
#include <getopt.h>

#include <chrono>
#include <string>

#include "apex_constants.h"
#include "apex_file_repository.h"
#include "com_android_apex.h"

void usage(const char* cmd) {
  std::cout << "Usage: " << cmd << " --root_dir=<dir> --out_file=<file.xsd>"
            << std::endl;
}

// Create apex-info-list based on pre installed apexes
int main(int argc, char** argv) {
  android::base::InitLogging(argv, android::base::StdioLogger);
  auto severity = android::base::ERROR;
  static constexpr const char* kRootDir = "root_dir";
  static constexpr const char* kOutFile = "out_file";

  static struct option long_options[] = {{kRootDir, required_argument, 0, 0},
                                         {kOutFile, required_argument, 0, 0},
                                         {0, 0, 0, 0}};

  std::map<std::string, std::string> opts;
  int index = 0;
  for (;;) {
    int c = getopt_long(argc, argv, "hv", long_options, &index);

    if (c == -1) {
      break;
    }
    switch (c) {
      case 0:
        opts[long_options[index].name] = optarg;
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      case 'v':
        severity = android::base::VERBOSE;
        break;
      case '?':
      default:
        usage(argv[0]);
        return -1;
    }
  }

  if (opts.size() != 2) {
    usage(argv[0]);
    return -1;
  }
  android::base::SetMinimumLogSeverity(severity);
  std::string root_dir;
  if (!android::base::Realpath(opts[kRootDir], &root_dir)) {
    LOG(ERROR) << "Failed to resolve realpath for root directory "
               << opts[kRootDir];
    return -1;
  }

  const std::string apex_root =
      root_dir + std::string(::android::apex::kApexRoot);

  // Ignore duplicate definitions to support multi-installed APEXes, the first
  // found APEX package for a name is chosen.
  const bool ignore_duplicate_definitions = true;
  ::android::apex::ApexFileRepository repo(apex_root,
                                           ignore_duplicate_definitions);

  std::vector<std::string> prebuilt_dirs{
      ::android::apex::kApexPackageBuiltinDirs};

  // Add pre-installed apex directories
  for (auto& dir : prebuilt_dirs) {
    dir.insert(0, root_dir);
  }
  auto ret = repo.AddPreInstalledApex(prebuilt_dirs);
  if (!ret.ok()) {
    LOG(ERROR) << "Failed to add pre-installed apex directories";
    return -1;
  }

  std::vector<com::android::apex::ApexInfo> apex_infos;
  for (const auto& [name, files] : repo.AllApexFilesByName()) {
    if (files.size() != 1) {
      LOG(ERROR) << "Multiple APEXs found for " << name;
      return -1;
    }

    const android::apex::ApexFile& apex = files[0];

    // Remove leading path from module names
    std::optional<std::string> preinstalled_module_path;
    {
      auto preinstalled_path = repo.GetPreinstalledPath(name);
      if (preinstalled_path.ok()) {
        preinstalled_module_path =
            (*preinstalled_path).substr(root_dir.length());
      }
    }
    auto path = apex.GetPath().substr(root_dir.length());
    const bool is_active = true;
    const std::optional<int64_t> mtime;
    com::android::apex::ApexInfo apex_info(
        apex.GetManifest().name(), path, preinstalled_module_path,
        apex.GetManifest().version(), apex.GetManifest().versionname(),
        repo.IsPreInstalledApex(apex), is_active, mtime,
        apex.GetManifest().providesharedapexlibs());
    apex_infos.emplace_back(std::move(apex_info));
  }

  std::stringstream xml;
  com::android::apex::ApexInfoList apex_info_list(apex_infos);
  com::android::apex::write(xml, apex_info_list);

  const std::string file_name = opts[kOutFile];
  android::base::unique_fd fd(TEMP_FAILURE_RETRY(
      open(file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));

  if (fd.get() == -1) {
    PLOG(ERROR) << "Can't create " << file_name;
    return -1;
  }

  if (!android::base::WriteStringToFd(xml.str(), fd)) {
    PLOG(ERROR) << "Can't write to " << file_name;
    return -1;
  }

  return 0;
}
