/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "apexd_loop.h"

#include <android-base/file.h>
#include <android-base/result-gmock.h>
#include <android-base/scopeguard.h>
#include <android-base/unique_fd.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <linux/loop.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "apex_file.h"
#include "apexd_test_utils.h"

using android::base::unique_fd;
using android::base::testing::Ok;
using ::testing::Not;
using ::testing::internal::CaptureStderr;
using ::testing::internal::GetCapturedStderr;

namespace android::apex {

static void AssertLoopIsCleared(const std::string& path) {
  unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
  ASSERT_TRUE(fd.ok());
  ASSERT_EQ(ioctl(fd, LOOP_CLR_FD, 0), -1);
  ASSERT_EQ(errno, ENXIO);
}

TEST(Loop, CreateWithApexFile) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());
  ASSERT_TRUE(apex->GetImageOffset().has_value());
  ASSERT_TRUE(apex->GetImageSize().has_value());

  auto loop = loop::CreateAndConfigureLoopDevice(apex->GetPath(),
                                                 apex->GetImageOffset().value(),
                                                 apex->GetImageSize().value());
  ASSERT_THAT(loop, Ok());
}

TEST(Loop, ClearedOnExit) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  std::string loop_name;
  {
    auto loop = loop::CreateAndConfigureLoopDevice(
        apex->GetPath(), apex->GetImageOffset().value(),
        apex->GetImageSize().value());
    ASSERT_THAT(loop, Ok());
    loop_name = loop->name;
  }
  AssertLoopIsCleared(loop_name);
}

TEST(Loop, ClearedOnCloseGood) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  std::string loop_name;
  {
    auto loop = loop::CreateAndConfigureLoopDevice(
        apex->GetPath(), apex->GetImageOffset().value(),
        apex->GetImageSize().value());
    ASSERT_THAT(loop, Ok());
    loop_name = loop->name;
    loop->CloseGood();
  }
  AssertLoopIsCleared(loop_name);
}

TEST(Loop, AliveWhileMounted) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  TemporaryDir temp_dir;
  auto umount = base::make_scope_guard(
      [&]() { umount2(temp_dir.path, UMOUNT_NOFOLLOW); });
  std::string loop_name;
  {
    // Create a loop for apex paylaod
    auto loop = loop::CreateAndConfigureLoopDevice(
        apex->GetPath(), apex->GetImageOffset().value(),
        apex->GetImageSize().value());
    ASSERT_THAT(loop, Ok());
    loop_name = loop->name;

    // Mount the payload filesystem
    uint32_t mount_flags = MS_NOATIME | MS_NODEV | MS_DIRSYNC | MS_RDONLY;
    auto rc = mount(loop_name.c_str(), temp_dir.path,
                    apex->GetFsType().value().c_str(), mount_flags, nullptr);
    ASSERT_EQ(rc, 0) << strerror(errno);

    // CloseGood() prevents LOOP_CLR_FD on exit(dtor)
    loop->CloseGood();
  }

  std::string manifest_path = std::string(temp_dir.path) + "/apex_manifest.pb";
  ASSERT_EQ(access(manifest_path.c_str(), F_OK), 0);
  ASSERT_EQ(access(loop_name.c_str(), F_OK), 0);

  ASSERT_EQ(umount2(temp_dir.path, UMOUNT_NOFOLLOW), 0);
  umount.Disable();

  // loop is cleaned up automatically after unmount.
  ASSERT_NE(access(manifest_path.c_str(), F_OK), 0);
  AssertLoopIsCleared(loop_name);
}

TEST(Loop, NoSuchFile) {
  CaptureStderr();
  {
    auto loop = loop::CreateAndConfigureLoopDevice("invalid_path", 0, 0);
    ASSERT_THAT(loop, Not(Ok()));
  }
  ASSERT_EQ(GetCapturedStderr(), "");
}
}  // namespace android::apex