// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <memory>
#include <string>

namespace vds {
class Logger;
}

namespace vds::win::hidhide {

bool provider_available();
std::string provider_path();
void register_daemon();

class DeviceGuard {
public:
  DeviceGuard(std::string instance_path, Logger &logger);
  ~DeviceGuard();

  DeviceGuard(const DeviceGuard &) = delete;
  DeviceGuard &operator=(const DeviceGuard &) = delete;

private:
  std::string instance_path_;
  Logger &logger_;
  bool hide_added_ = false;
};

std::unique_ptr<DeviceGuard> hide_device(std::string instance_path,
                                         Logger &logger);

} // namespace vds::win::hidhide
