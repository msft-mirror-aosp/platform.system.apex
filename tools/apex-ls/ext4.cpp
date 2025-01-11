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

#include "ext4.h"

#include <android-base/result.h>
#include <android-base/scopeguard.h>
extern "C" {
#include <et/com_err.h>
#include <ext2fs/ext2_io.h>
#include <ext2fs/ext2fs.h>
}
#include <sys/stat.h>

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

Result<ext2_ino_t> PathToIno(ext2_filsys fs, const fs::path& path) {
  ext2_ino_t ino;
  auto err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO,
                          path.string().c_str(), &ino);
  if (err) {
    return Error() << "failed to resolve path" << path << ": "
                   << error_message(err);
  }
  return ino;
}

Result<std::string> GetXattr(ext2_filsys fs, ext2_ino_t ino,
                             const std::string& key) {
  struct ext2_xattr_handle* h;

  auto err = ext2fs_xattrs_open(fs, ino, &h);
  if (err) {
    return Error() << "failed to open xattr: " << error_message(err);
  }
  auto close = make_scope_guard([&] { ext2fs_xattrs_close(&h); });

  err = ext2fs_xattrs_read(h);
  if (err) {
    return Error() << "failed to read xattr: " << error_message(err);
  }

  char* buf = nullptr;
  size_t buflen;
  err = ext2fs_xattr_get(h, key.c_str(), (void**)&buf, &buflen);
  if (err) {
    return Error() << "failed to get xattr " << key << ": "
                   << error_message(err);
  }
  std::string value = buf;
  ext2fs_free_mem(&buf);

  return value;
}

struct ReadDirContext {
  fs::path dir;
  std::vector<std::string> names;
};

int ReadDirIter(ext2_ino_t dir, int entry, struct ext2_dir_entry* dirent,
                int offset, int blocksize, char* buf, void* priv_data) {
  (void)dir;
  (void)entry;
  (void)offset;
  (void)blocksize;
  (void)buf;

  ReadDirContext* ctx = (ReadDirContext*)priv_data;

  auto len = ext2fs_dirent_name_len(dirent);
  std::string name(dirent->name, dirent->name + len);
  // ignore ./lost+found
  if (ctx->dir == "." && name == "lost+found") {
    return 0;
  }
  ctx->names.push_back(name);
  return 0;
}

Result<std::vector<std::string>> ReadDir(ext2_filsys fs, const fs::path& path) {
  ext2_ino_t ino = OR_RETURN(PathToIno(fs, path));

  ReadDirContext ctx = {.dir = path};
  auto err = ext2fs_dir_iterate2(fs, ino, /*flag*/ 0,
                                 /*block_buf*/ nullptr, &ReadDirIter,
                                 /*priv_data*/ &ctx);
  if (err) {
    return Error() << "failed to read dir " << path << ": "
                   << error_message(err);
  }
  return ctx.names;
}

Result<Entry> ReadEntry(ext2_filsys fs, const fs::path& path) {
  ext2_ino_t ino = OR_RETURN(PathToIno(fs, path));

  struct ext2_inode inode;
  auto err = ext2fs_read_inode(fs, ino, &inode);
  if (err) {
    return Error() << "failed to read inode for " << path << ": "
                   << error_message(err);
  }

  mode_t mode = inode.i_mode;

  std::string entry_path = path.string();
  // make sure dir path ends with '/'
  if (S_ISDIR(mode)) {
    entry_path += '/';
  }

  // read security context
  auto security_context = OR_RETURN(GetXattr(fs, ino, "security.selinux"));

  return Entry{mode, entry_path, security_context};
}

}  // namespace

Result<std::vector<Entry>> Ext4List(const std::string& image_path) {
  // open image
  ext2_filsys fs;
  io_manager io_ptr = unix_io_manager;
  auto err = ext2fs_open(
      image_path.c_str(),
      EXT2_FLAG_SOFTSUPP_FEATURES | EXT2_FLAG_64BITS | EXT2_FLAG_THREADS,
      /* superblock */ 0, /* blocksize */ 0, io_ptr, &fs);
  if (err) {
    return Error() << "failed to open " << image_path << ": "
                   << error_message(err);
  }
  auto close = make_scope_guard([&] { ext2fs_close_free(&fs); });

  return List(std::bind(&ReadEntry, fs, _1), std::bind(&ReadDir, fs, _1));
}