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

#include "erofs.h"

#include <android-base/result.h>
#include <android-base/scopeguard.h>
// ignore unused-parameter in headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <erofs/config.h>
#include <erofs/dir.h>
#include <erofs/inode.h>
#pragma GCC diagnostic pop

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using android::base::Error;
using android::base::make_scope_guard;
using android::base::Result;
namespace fs = std::filesystem;
using namespace std::placeholders;

namespace {

struct ReadDirContext {
  struct erofs_dir_context ctx;
  std::vector<std::string> names;
};

int ReadDirIter(struct erofs_dir_context* ctx) {
  std::string name{ctx->dname, ctx->dname + ctx->de_namelen};
  ((ReadDirContext*)ctx)->names.push_back(name);
  return 0;
}

Result<std::vector<std::string>> ReadDir(struct erofs_sb_info* sbi,
                                         const fs::path& path) {
  struct erofs_inode dir = {.sbi = sbi};
  auto err = erofs_ilookup(path.string().c_str(), &dir);
  if (err) {
    return Error(err) << "failed to read inode for " << path;
  }
  if (!S_ISDIR(dir.i_mode)) {
    return Error() << "failed to read dir: " << path << " is not a directory";
  }
  ReadDirContext ctx = {
      {
          .dir = &dir,
          .cb = ReadDirIter,
          .flags = EROFS_READDIR_VALID_PNID,
      },
  };
  err = erofs_iterate_dir(&ctx.ctx, false);
  if (err) {
    return Error(err) << "failed to read dir";
  }
  return ctx.names;
}

Result<Entry> ReadEntry(struct erofs_sb_info* sbi, const fs::path& path) {
  struct erofs_inode inode = {.sbi = sbi};
  auto err = erofs_ilookup(path.string().c_str(), &inode);
  if (err) {
    return Error(err) << "failed to read inode for " << path;
  }

  mode_t mode = inode.i_mode;

  std::string entry_path = path.string();
  // make sure dir path ends with '/'
  if (S_ISDIR(mode)) {
    entry_path += '/';
  }

  // read security context
  char security_context[256];
  err = erofs_getxattr(&inode, "security.selinux", security_context,
                       sizeof(security_context));
  if (err < 0) {
    return Error() << "failed to get security context of " << path << ": "
                   << erofs_strerror(err);
  }

  return Entry{mode, entry_path, security_context};
}

}  // namespace

Result<std::vector<Entry>> ErofsList(const std::string& image_path) {
  erofs_init_configure();
  auto configure = make_scope_guard(&erofs_exit_configure);

  // open image
  struct erofs_sb_info sbi;
  auto err = erofs_dev_open(&sbi, image_path.c_str(), O_RDONLY | O_TRUNC);
  if (err) {
    return Error(err) << "failed to open image file";
  }
  auto dev = make_scope_guard([&] { erofs_dev_close(&sbi); });

  // read superblock
  err = erofs_read_superblock(&sbi);
  if (err) {
    return Error(err) << "failed to read superblock";
  }
  auto superblock = make_scope_guard([&] { erofs_put_super(&sbi); });

  return List(std::bind(&ReadEntry, &sbi, _1), std::bind(&ReadDir, &sbi, _1));
}