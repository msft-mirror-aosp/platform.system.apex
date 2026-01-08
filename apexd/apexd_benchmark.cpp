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

#include <android-base/properties.h>
#include <benchmark/benchmark.h>

#include "apex_file.h"
#include "apex_file_repository.h"
#include "apexd.h"
#include "apexd_loop.h"

using android::apex::ApexFile;
using android::apex::ApexFileRepository;
using android::apex::kBuiltinApexPackageDirs;
using android::base::SetProperty;

static void BM_ApexFile_Open(benchmark::State& state) {
  for (auto _ : state) {
    ApexFile::Open("/system/apex/com.android.apex.cts.shim.apex");
  }
}
BENCHMARK(BM_ApexFile_Open);

static void BM_ApexFileRepository_AddPreInstalledApex(benchmark::State& state) {
  for (auto _ : state) {
    ApexFileRepository instance;
    instance.AddPreInstalledApex(kBuiltinApexPackageDirs);
  }
}
BENCHMARK(BM_ApexFileRepository_AddPreInstalledApex);

static void BM_ApexFileRepository_GetPreInstalledApex(benchmark::State& state) {
  ApexFileRepository instance;
  instance.AddPreInstalledApex(kBuiltinApexPackageDirs);
  for (auto _ : state) {
    instance.GetPreInstalledApexFiles();
  }
}
BENCHMARK(BM_ApexFileRepository_GetPreInstalledApex);

static void BM_EmitApexInfoList(benchmark::State& state) {
  auto& instance = ApexFileRepository::GetInstance();
  instance.AddPreInstalledApex(kBuiltinApexPackageDirs);
  auto preinstalled = instance.GetPreInstalledApexFiles();
  for (auto _ : state) {
    android::apex::EmitApexInfoList(preinstalled, false);
  }
}
BENCHMARK(BM_EmitApexInfoList);

static void BM_ConfigureReadAheadSysfs(benchmark::State& state) {
  auto apex = ApexFile::Open("/system/apex/com.android.apex.cts.shim.apex");
  auto loop = android::apex::loop::CreateAndConfigureLoopDevice(
      apex->GetPath(), apex->GetImageOffset().value(),
      apex->GetImageSize().value());
  for (auto _ : state) {
    android::apex::loop::ConfigureReadAheadSysfs(loop->name);
  }
}
BENCHMARK(BM_ConfigureReadAheadSysfs);

static void BM_ConfigureReadAheadIoctl(benchmark::State& state) {
  auto apex = ApexFile::Open("/system/apex/com.android.apex.cts.shim.apex");
  auto loop = android::apex::loop::CreateAndConfigureLoopDevice(
      apex->GetPath(), apex->GetImageOffset().value(),
      apex->GetImageSize().value());
  for (auto _ : state) {
    android::apex::loop::ConfigureReadAheadIoctl(loop->device_fd);
  }
}
BENCHMARK(BM_ConfigureReadAheadIoctl);

BENCHMARK_MAIN();