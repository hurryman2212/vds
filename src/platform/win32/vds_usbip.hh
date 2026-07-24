// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
// Portions Copyright (C) 2026 Antonio <bleader93@gmail.com>
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace vds {
class Logger;
}

namespace vds::win::usbip {

constexpr std::uint16_t kProtocolVersion = 0x0111;
constexpr std::uint16_t kDefaultPort = 3240;

bool client_available();
std::string client_path();
std::string endpoint_for_port(unsigned port_index);

class VirtualPort {
public:
  VirtualPort(std::uint32_t profile, unsigned port_index, Logger &logger);
  ~VirtualPort();

  VirtualPort(const VirtualPort &) = delete;
  VirtualPort &operator=(const VirtualPort &) = delete;

  const std::string &pipe_path() const { return pipe_path_; }
  bool usb_attached() const;
  std::string_view busid() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::string pipe_path_;
};

std::unique_ptr<VirtualPort>
open_virtual_port(std::uint32_t profile, unsigned port_index, Logger &logger);

} // namespace vds::win::usbip
