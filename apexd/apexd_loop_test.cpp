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
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <linux/loop.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "apex_file.h"
#include "apexd_test_utils.h"
#include "apexd_utils.h"
#include "com_android_apex_flags.h"

namespace flags = com::android::apex::flags;
using android::base::unique_fd;
using android::base::testing::Ok;
using ::testing::Not;
using ::testing::internal::CaptureStderr;
using ::testing::internal::GetCapturedStderr;
using namespace std::chrono_literals;

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

TEST(Loop, CreateDeviceNodeIfMissing) {
  if constexpr (!flags::mount_before_data()) {
    GTEST_SKIP() << "mount_before_data disabled";
  }
  // This test verifies that CreateAndConfigureLoopDevice can successfully
  // create a loop device even if the corresponding /dev/block/loop[num] node
  // is missing. This is the scenario the change is addressing (to avoid
  // waiting for ueventd).

  // 1. Find a free loop device number. This will cause the kernel to create
  //    the sysfs entries for it.
  unique_fd ctl_fd(open("/dev/loop-control", O_RDWR | O_CLOEXEC));
  ASSERT_TRUE(ctl_fd.ok()) << strerror(errno);
  int num = ioctl(ctl_fd.get(), LOOP_CTL_GET_FREE);
  ASSERT_NE(num, -1) << strerror(errno);
  auto free_loop = base::make_scope_guard(
      [&]() { ioctl(ctl_fd.get(), LOOP_CTL_REMOVE, num); });

  // 2. ueventd will create the device node. We wait for it and then delete it
  //    to simulate a race condition where apexd runs before ueventd has
  //    created the node.
  std::string dev_path = base::StringPrintf("/dev/block/loop%d", num);
  ASSERT_THAT(WaitForFile(dev_path, 5s), Ok());
  ASSERT_EQ(unlink(dev_path.c_str()), 0) << strerror(errno);
  ASSERT_NE(access(dev_path.c_str(), F_OK), 0);

  // 3. The /sys entry should still exist, as it's managed by the kernel.
  std::string sys_path = base::StringPrintf("/sys/block/loop%d/dev", num);
  ASSERT_EQ(access(sys_path.c_str(), F_OK), 0) << strerror(errno);

  // 4. Call CreateAndConfigureLoopDevice with the specific loop id. It should
  //    detect the missing device node, read the major/minor from sysfs,
  //    create the node itself, and then successfully configure the loop device.
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());

  auto loop = loop::CreateAndConfigureLoopDevice(
      apex->GetPath(), apex->GetImageOffset().value(),
      apex->GetImageSize().value(), num);
  ASSERT_THAT(loop, Ok());
  EXPECT_EQ(loop->name, dev_path);

  // 5. Check that the device node was indeed created and is a block device.
  struct stat st;
  ASSERT_EQ(stat(dev_path.c_str(), &st), 0) << strerror(errno);
  ASSERT_TRUE(S_ISBLK(st.st_mode));
}

TEST(Loop, NoSuchFile) {
  CaptureStderr();
  {
    auto loop = loop::CreateAndConfigureLoopDevice("invalid_path", 0, 0);
    ASSERT_THAT(loop, Not(Ok()));
  }
  ASSERT_EQ(GetCapturedStderr(), "");
}

TEST(Loop, CreateAndConfigureLoopDevice_MultiThreaded) {
  auto apex = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_THAT(apex, Ok());

  constexpr int kNumThreads = 5;
  std::vector<std::thread> threads;
  std::vector<android::base::Result<loop::LoopbackDeviceUniqueFd>> results(
      kNumThreads);

  // This test exercises the thread-safety of creating loop devices, which
  // relies on a static mutex. It also indirectly tests other functions with
  // static variables (for caching, one-time initialization) under concurrent
  // access. This is relevant to the change that added [[clang::no_destroy]] to
  // these static variables, ensuring their functionality is not broken.
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, i]() {
      results[i] = loop::CreateAndConfigureLoopDevice(
          apex->GetPath(), apex->GetImageOffset().value(),
          apex->GetImageSize().value());
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  for (int i = 0; i < kNumThreads; ++i) {
    ASSERT_THAT(results[i], Ok()) << "Thread " << i << " failed";
    // The LoopbackDeviceUniqueFd in results[i] will be destructed at the end
    // of the test, automatically cleaning up the loop device.
  }
}
}  // namespace android::apex