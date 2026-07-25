// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "uapi/vds.h"

namespace vds {

inline constexpr unsigned kMinPortCount = VDS_MIN_PORT_COUNT;
inline constexpr unsigned kMaxPortCount = VDS_MAX_PORT_COUNT;

enum class ControllerProfile {
  Unspecified,
  Ds5,
  Dse,
};

struct ControllerConfig {
  std::string address;
  ControllerProfile profile = ControllerProfile::Unspecified;
  std::vector<unsigned> ports;
};

struct ControllerTarget {
  std::string address;
  std::string name;
  ControllerProfile profile = ControllerProfile::Unspecified;
  bool online = false;
};

struct ConfigDb {
  std::vector<ControllerConfig> controllers;
};

std::string normalize_bluetooth_address(std::string_view address);
ControllerProfile parse_controller_profile(std::string_view profile);
std::string controller_profile_name(ControllerProfile profile);
std::vector<unsigned> parse_ports(std::string_view text);
std::string format_ports(std::span<const unsigned> ports);
std::string port_path_for_index(unsigned port);
std::optional<unsigned> port_index_from_path(std::string_view path);
bool controller_config_allows_port(const ControllerConfig &config,
                                   unsigned port);
bool controller_config_allows_path(const ControllerConfig &config,
                                   std::string_view path);
std::vector<unsigned>
controller_config_port_targets(const ControllerConfig &config);
bool controller_config_has_candidate_port(
    const ControllerConfig &config, std::span<const unsigned> candidate_ports);
std::optional<unsigned>
select_controller_config_port(const ControllerConfig &config,
                              std::span<const unsigned> candidate_ports,
                              std::span<const unsigned> occupied_ports = {});
const ControllerConfig *
find_controller_config_by_address(const ConfigDb &db, std::string_view address);
ConfigDb load_config_db(const std::string &path);
ConfigDb update_config_db(const std::string &path,
                          const std::function<void(ConfigDb &)> &update);
void upsert_controller_config(ConfigDb &db, ControllerConfig config);
bool remove_controller_config(ConfigDb &db, std::string_view address);
void validate_config_assignments(const ConfigDb &db,
                                 std::span<const std::string> devices);

} // namespace vds
