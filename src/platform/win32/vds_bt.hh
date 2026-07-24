// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "uapi/vds.h"
#include "vds_io.hh"

namespace vds::win {

class BluetoothTransport {
public:
  virtual ~BluetoothTransport() = default;

  virtual Frame read_frame() = 0;
  virtual std::optional<std::vector<std::uint8_t>>
  read_feature_report(std::uint8_t report_id) {
    (void)report_id;
    return std::nullopt;
  }
  virtual void write_feature_report(std::span<const std::uint8_t> report) = 0;
  virtual void write_interrupt_packet(std::span<const std::uint8_t> packet) = 0;
  virtual bool
  try_write_interrupt_packet(std::span<const std::uint8_t> packet) = 0;
  virtual std::optional<std::string> take_output_diagnostics(bool force) {
    (void)force;
    return std::nullopt;
  }
  virtual void cancel() = 0;
  virtual std::string description() const = 0;
};

struct HidBluetoothDevice {
  std::string path;
  std::string instance_path;
  std::string address;
  std::string name;
  std::uint32_t profile = VDS_PROFILE_DS5;
  bool profile_valid = false;
  bool bluetooth_connected = false;
};

std::optional<HidBluetoothDevice>
find_hid_bluetooth_device(const std::string &address);
std::vector<HidBluetoothDevice> list_hid_bluetooth_devices();
std::vector<HidBluetoothDevice> list_bluetooth_controller_devices();
std::string describe_bluetooth_lookup(const std::string &address);
std::unique_ptr<BluetoothTransport>
make_hid_bluetooth_transport(const std::string &address);

} // namespace vds::win
