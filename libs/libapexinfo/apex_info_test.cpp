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
#include "apex_info.h"
#include "apex_info_cache.h"

#include <string>
#include <unistd.h>
#include <utility>

#include <android-base/file.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "com_android_apex.h"

using namespace std::literals;

using ::android::apex::info::ApexInfoCache;
using ::android::apex::info::ApexType;
using ::android::base::WriteStringToFile;

// Create an ApexInfo object based on inputs, writing data to the apex_dir/name
// directory
com::android::apex::ApexInfo
CreateApex(const std::string &name, const std::string &pre_path, int version) {
  auto version_name = std::to_string(version);

  std::string path = "/apex/" + name;
  return (com::android::apex::ApexInfo(name, path, pre_path, version,
                                       version_name, false, true, 0, false));
}

// Write info file
void UpdateApexInfoList(
    const std::string &info_file,
    const std::vector<com::android::apex::ApexInfo> &apex_infos) {
  com::android::apex::ApexInfoList apex_info_list(apex_infos);
  std::stringstream xml;
  com::android::apex::write(xml, apex_info_list);
  ASSERT_TRUE(WriteStringToFile(xml.str(), info_file));
}

void CreateApexData(const std::string &apex_dir, const std::string &info_file,
                    std::vector<com::android::apex::ApexInfo> &apex_infos) {
  ASSERT_NE(-1, mkdir(apex_dir.c_str(), 0755) == -1)
      << "Failed to create apex directory" << apex_dir;

  // Create partition of a given APEX type and add to apex info
  auto ASSERT_CREATE_PARTITION_APEX = [&apex_dir, &apex_infos](
                                          std::set<std::string> &names,
                                          const std::string &partition) {
    for (const auto &name : names) {
      apex_infos.emplace_back(
          CreateApex(name, std::string(partition).append("/").append(name), 1));
    }
  };

  // Partition APEXes
  std::set<std::string> vendor{"com.test_vendor.1", "com.test_vendor.2",
                               "com.test_vendor.3"};
  std::set<std::string> system{"com.test_system.1"};
  std::set<std::string> system_ext{"com.test_system_ext.1"};
  std::set<std::string> product{"com.test_product.1"};
  std::set<std::string> odm{"com.test_odm.1"};

  ASSERT_CREATE_PARTITION_APEX(vendor, "/vendor/apex");
  ASSERT_CREATE_PARTITION_APEX(system, "/system/apex");
  ASSERT_CREATE_PARTITION_APEX(system_ext, "/system_ext/apex");
  ASSERT_CREATE_PARTITION_APEX(product, "/product/apex");
  ASSERT_CREATE_PARTITION_APEX(odm, "/odm/apex");

  // Write the info list
  UpdateApexInfoList(info_file, apex_infos);

  auto apex = android::apex::info::GetApexes(info_file);
  auto exp_size = system.size() + system_ext.size() + product.size() +
                  vendor.size() + odm.size();
  ASSERT_TRUE(apex.ok());
  ASSERT_EQ((*apex).size(), exp_size)
      << "Got apex size " << (*apex).size() << " expected size " << exp_size;

  for (const auto &obj : *apex) {
    switch (obj.Type()) {
    case (ApexType::kSystem):
      ASSERT_EQ(system.count(obj.ManifestName()) +
                    system_ext.count(obj.ManifestName()),
                1);
      system.erase(obj.ManifestName());
      system_ext.erase(obj.ManifestName());
      break;
    case (ApexType::kProduct):
      ASSERT_EQ(product.count(obj.ManifestName()), 1);
      product.erase(obj.ManifestName());
      break;
    case (ApexType::kVendor):
      ASSERT_EQ(vendor.count(obj.ManifestName()), 1);
      vendor.erase(obj.ManifestName());
      break;
    case (ApexType::kOdm):
      ASSERT_EQ(odm.count(obj.ManifestName()), 1);
      odm.erase(obj.ManifestName());
      break;
    }
  }
  // Assert all modules identified
  ASSERT_EQ(system.size(), 0);
  ASSERT_EQ(system_ext.size(), 0);
  ASSERT_EQ(product.size(), 0);
  ASSERT_EQ(vendor.size(), 0);
  ASSERT_EQ(odm.size(), 0);
}

TEST(ApexInfo, ApexInfo) {
  TemporaryDir td;
  const std::string apex_dir = td.path + "/apex/"s;
  const std::string info_file =
      apex_dir + ::android::apex::info::kApexInfoFileName;

  std::vector<com::android::apex::ApexInfo> apex_infos;
  CreateApexData(apex_dir, info_file, apex_infos);
}

TEST(ApexInfo, ApexInfoCache) {
  // Monitor based on Update()
  TemporaryDir td;
  const std::string apex_dir = td.path + "/apex/"s;
  const std::string info_file =
      apex_dir + ::android::apex::info::kApexInfoFileName;

  ASSERT_EQ(mkdir(apex_dir.c_str(), 0755), 0)
      << "Failed to create apex directory" << apex_dir;

  ApexInfoCache m(info_file);
  m.SetApexReady();
  auto ASSERT_NEW_DATA = [&m](bool bExp) {
    auto ret = m.HasNewData();
    ASSERT_EQ(ret.ok(), true);
    ASSERT_EQ(*ret, bExp);
  };
  auto ASSERT_UPDATE = [&m](bool bExp) {
    auto ret = m.Update();
    ASSERT_EQ(ret.ok(), true);
    ASSERT_EQ(*ret, bExp);
  };
  auto ASSERT_UPDATE_ERROR = [&m]() {
    auto ret = m.Update();
    ASSERT_EQ(ret.ok(), false);
  };

  ASSERT_UPDATE_ERROR();

  // Need to setup actual data to use update
  std::vector<com::android::apex::ApexInfo> apex_infos;
  CreateApexData(apex_dir, info_file, apex_infos);

  // validate new data is available
  ASSERT_NEW_DATA(true);
  ASSERT_UPDATE(true);

  // No new updates
  ASSERT_NEW_DATA(false);
  ASSERT_UPDATE(false);

  // Check existing file update
  sleep(1); // make sure timestamps change
  UpdateApexInfoList(info_file, apex_infos);
  ASSERT_NEW_DATA(true);
  ASSERT_UPDATE(true);

  // Check that new data can be detected directly with update
  sleep(1); // make sure timestamps change
  UpdateApexInfoList(info_file, apex_infos);
  ASSERT_UPDATE(true);
  ASSERT_NEW_DATA(false); // new data should be consumed by above call
}

struct ResetValueForTesting {
  std::string *var;
  std::string old_value;
  ResetValueForTesting(std::string *var, const std::string &value_for_testing)
      : var(var), old_value(*var) {
    *var = value_for_testing;
  }
  ~ResetValueForTesting() { *var = old_value; }
};

TEST(ApexInfo, ApexInfoSymlinkedPartitions) {
  using namespace android::apex::info::details;
  ResetValueForTesting set_real_path_for_vendor{&kVendorRealPath,
                                                "/system/vendor"};
  ResetValueForTesting set_real_path_for_system_ext{&kSystemExtRealPath,
                                                    "/system/system_ext"};
  std::vector<com::android::apex::ApexInfo> apex_infos{
      CreateApex("com.android.foo",
                 "/system/system_ext/apex/com.android.foo.apex", 1),
      CreateApex("com.android.vendor",
                 "/system/vendor/apex/com.android.vendor.apex", 1),
  };
  TemporaryFile tf;
  UpdateApexInfoList(tf.path, apex_infos);

  auto apex = android::apex::info::GetApexes(tf.path);
  ASSERT_TRUE(apex.ok()) << apex.error();
  ASSERT_EQ(apex_infos.size(), apex->size());
  ASSERT_EQ(::android::apex::info::ApexType::kSystem, apex->at(0).Type());
  ASSERT_EQ(::android::apex::info::ApexType::kVendor, apex->at(1).Type());
}
