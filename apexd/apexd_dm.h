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

#pragma once

#include <android-base/result.h>
#include <libdm/dm.h>

#include <string>

namespace android::apex {

class DmDevice {
 public:
  DmDevice() : cleared_(true) {}
  explicit DmDevice(const std::string& name) : name_(name), cleared_(false) {}
  DmDevice(const std::string& name, const std::string& dev_path)
      : name_(name), dev_path_(dev_path), cleared_(false) {}

  DmDevice(DmDevice&& other) noexcept
      : name_(std::move(other.name_)),
        dev_path_(std::move(other.dev_path_)),
        cleared_(other.cleared_) {
    other.cleared_ = true;
  }

  DmDevice& operator=(DmDevice&& other) noexcept {
    name_ = other.name_;
    dev_path_ = other.dev_path_;
    cleared_ = other.cleared_;
    other.cleared_ = true;
    return *this;
  }

  ~DmDevice();

  const std::string& GetName() const { return name_; }
  const std::string& GetDevPath() const { return dev_path_; }

  void Release() { cleared_ = true; }

 private:
  std::string name_;
  std::string dev_path_;
  bool cleared_;
};

base::Result<DmDevice> CreateDmDevice(const std::string& name,
                                      const dm::DmTable& table,
                                      bool reuse_device);

base::Result<void> DeleteDmDevice(const std::string& name, bool deferred);

}  // namespace android::apex
