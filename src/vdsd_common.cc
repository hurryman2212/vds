// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "jsonl.hh"
#include "vds_log.hh"
#include "vdsd_common.hh"

namespace vds {

namespace {

#ifdef _WIN32
std::string windows_program_data_file(std::string_view filename) {
  const DWORD required = GetEnvironmentVariableA("ProgramData", nullptr, 0);
  if (required == 0) {
    throw std::runtime_error("ProgramData is not available");
  }

  std::string program_data(required, '\0');
  const DWORD length =
      GetEnvironmentVariableA("ProgramData", program_data.data(), required);
  if (length == 0 || length >= required) {
    throw std::runtime_error("ProgramData is not available");
  }
  program_data.resize(length);

  return (std::filesystem::path(program_data) / "vDS" / filename).string();
}
#endif

} // namespace

VdsdCommonOptions default_vdsd_common_options() {
#ifdef _WIN32
  return VdsdCommonOptions{
      .db_path = windows_program_data_file("vdsd.db"),
      .log_path = windows_program_data_file("vdsd.log"),
      .help_requested = false,
  };
#else
  return VdsdCommonOptions{
      .db_path = "/var/lib/vds/vdsd.db",
      .log_path = "/var/log/vdsd.log",
      .help_requested = false,
  };
#endif
}

std::string vdsd_usage(std::string_view version, std::string_view build_year,
                       std::string_view platform_options) {
  const VdsdCommonOptions defaults = default_vdsd_common_options();
  std::string text = "vdsd (";
  text += version;
  text += "): vDS daemon - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsd [--db-path ";
  text += defaults.db_path;
  text += "] [--log ";
  text += defaults.log_path;
  text += "]";
  if (!platform_options.empty()) {
    text += " ";
    text += platform_options;
  }
  text += "\n";
  return text;
}

void print_vdsd_usage(std::ostream &out, std::string_view version,
                      std::string_view build_year,
                      std::string_view platform_options) {
  out << vdsd_usage(version, build_year, platform_options);
}

bool parse_vdsd_common_option(int argc, char **argv, int &index,
                              VdsdCommonOptions &options) {
  const std::string_view arg = argv[index];
  if (arg == "--db-path") {
    if (index + 1 >= argc) {
      throw std::runtime_error("--db-path requires a path");
    }
    options.db_path = argv[++index];
    return true;
  }
  if (arg == "--log") {
    if (index + 1 >= argc) {
      throw std::runtime_error("--log requires a path");
    }
    options.log_path = argv[++index];
    return true;
  }
  if (arg == "--help" || arg == "-h") {
    options.help_requested = true;
    return true;
  }
  return false;
}

std::string trim_command(std::string command) {
  while (!command.empty() &&
         (command.back() == '\n' || command.back() == '\r' ||
          command.back() == ' ' || command.back() == '\t')) {
    command.pop_back();
  }
  return command;
}

std::uint32_t parse_trace_scope(std::string_view scope) {
  std::uint32_t flags = 0;
  while (true) {
    const std::size_t separator = scope.find(',');
    const std::string_view item = scope.substr(0, separator);
    if (item == "input" || item == "all") {
      flags |= kTraceInput;
      if (item == "all") {
        flags |= kTraceOutput;
      }
    } else if (item == "input-audio") {
      flags |= kTraceInputAudio;
    } else if (item == "input-control") {
      flags |= kTraceInputControl;
    } else if (item == "output") {
      flags |= kTraceOutput;
    } else {
      throw std::runtime_error(
          "trace scope must be all, input, input-audio, input-control, output, "
          "or comma-separated scopes");
    }
    if (separator == std::string_view::npos) {
      return flags;
    }
    scope.remove_prefix(separator + 1);
  }
}

std::string trace_scope_name(std::uint32_t scope) {
  if (scope == kTraceAll) {
    return "all";
  }
  if (scope == kTraceInput) {
    return "input";
  }
  if (scope == kTraceInputAudio) {
    return "input-audio";
  }
  if (scope == kTraceInputControl) {
    return "input-control";
  }
  if (scope == kTraceOutput) {
    return "output";
  }
  std::string name;
  if (trace_enabled(scope, kTraceInputAudio)) {
    name += "input-audio";
  }
  if (trace_enabled(scope, kTraceInputControl)) {
    if (!name.empty()) {
      name += ',';
    }
    name += "input-control";
  }
  if (trace_enabled(scope, kTraceOutput)) {
    if (!name.empty()) {
      name += ',';
    }
    name += "output";
  }
  return name.empty() ? "none" : name;
}

std::string format_controller_config(const ControllerConfig &config) {
  std::string text = config.address;
  text += " profile=\"";
  text += controller_profile_name(config.profile);
  text += "\" ports=";
  if (config.ports.empty()) {
    text += "[]";
  } else {
    text += '[';
    text += format_ports(config.ports);
    text += ']';
  }
  return text;
}

std::string active_trace_name(std::uint32_t trace_flags) {
  trace_flags &= kTraceAll;
  if (trace_flags == 0) {
    return "none";
  }
  return trace_scope_name(trace_flags);
}

bool trace_enabled(std::uint32_t trace_flags, std::uint32_t target) {
  return (trace_flags & target) != 0;
}

bool remember_vdsd_warning(std::vector<std::string> &warnings,
                           std::string_view key) {
  const auto existing =
      std::find_if(warnings.begin(), warnings.end(),
                   [&](std::string_view item) { return item == key; });
  if (existing != warnings.end()) {
    return false;
  }
  warnings.emplace_back(key);
  return true;
}

void forget_vdsd_warning(std::vector<std::string> &warnings,
                         std::string_view key) {
  warnings.erase(
      std::remove_if(warnings.begin(), warnings.end(),
                     [&](std::string_view item) { return item == key; }),
      warnings.end());
}

bool remember_vdsd_warning_state(std::vector<std::string> &warnings,
                                 std::string_view key, std::string_view state) {
  std::string prefix{key};
  prefix += '\n';
  std::string entry = prefix;
  entry += state;
  if (std::find(warnings.begin(), warnings.end(), entry) != warnings.end()) {
    return false;
  }

  warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                [&](std::string_view existing) {
                                  return existing.rfind(prefix, 0) == 0;
                                }),
                 warnings.end());
  warnings.push_back(std::move(entry));
  return true;
}

void forget_vdsd_warning_state(std::vector<std::string> &warnings,
                               std::string_view key) {
  std::string prefix{key};
  prefix += '\n';
  warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                [&](std::string_view existing) {
                                  return existing.rfind(prefix, 0) == 0;
                                }),
                 warnings.end());
}

std::vector<VdsdControlPortStatus> build_vdsd_control_port_statuses(
    std::span<const VdsdControlPortCandidate> candidates,
    std::span<const VdsdControlPortBinding> bindings) {
  std::vector<VdsdControlPortStatus> statuses;
  statuses.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    const auto binding =
        std::find_if(bindings.begin(), bindings.end(), [&](const auto &item) {
          return item.port == candidate.port;
        });
    statuses.push_back(VdsdControlPortStatus{
        .port = candidate.port,
        .path = candidate.path,
        .occupied = binding != bindings.end(),
        .address = binding != bindings.end() ? binding->address : "",
        .device_address =
            binding != bindings.end() ? binding->device_address : "",
    });
  }
  return statuses;
}

VdsdWorkerLaunchDecision select_vdsd_worker_launch_decision(
    const ControllerConfig &config, std::span<const unsigned> available_ports,
    std::span<const unsigned> reserved_ports, std::string_view device_address,
    bool virtual_port_provider_available,
    std::span<const std::string> reserved_device_addresses) {
  if (!virtual_port_provider_available) {
    return VdsdWorkerLaunchDecision{
        .status = VdsdWorkerLaunchStatus::VirtualPortProviderUnavailable,
        .config = config,
        .port = 0,
        .device_address = std::string(device_address),
    };
  }

  if (std::find(reserved_device_addresses.begin(),
                reserved_device_addresses.end(),
                device_address) != reserved_device_addresses.end()) {
    return VdsdWorkerLaunchDecision{
        .status = VdsdWorkerLaunchStatus::DeviceAddressReserved,
        .config = config,
        .port = 0,
        .device_address = std::string(device_address),
    };
  }

  const auto port =
      select_controller_config_port(config, available_ports, reserved_ports);
  if (!port) {
    return VdsdWorkerLaunchDecision{
        .status = VdsdWorkerLaunchStatus::NoAvailablePort,
        .config = config,
        .port = 0,
        .device_address = std::string(device_address),
    };
  }

  ControllerConfig worker_config = config;
  worker_config.ports = {*port};
  return VdsdWorkerLaunchDecision{
      .status = VdsdWorkerLaunchStatus::Ready,
      .config = std::move(worker_config),
      .port = *port,
      .device_address = std::string(device_address),
  };
}

bool vdsd_worker_config_is_stale(const ConfigDb &db, std::string_view address,
                                 ControllerProfile profile, unsigned port,
                                 bool port_enabled) {
  const ControllerConfig *config =
      find_controller_config_by_address(db, address);
  return config == nullptr || config->profile != profile ||
         !controller_config_allows_port(*config, port) || !port_enabled;
}

void record_vdsd_worker_failure(
    std::vector<VdsdWorkerFailureBackoff> &backoffs, std::string_view address,
    std::chrono::steady_clock::time_point retry_after) {
  const auto existing =
      std::find_if(backoffs.begin(), backoffs.end(),
                   [&](const auto &entry) { return entry.address == address; });
  if (existing == backoffs.end()) {
    backoffs.push_back(VdsdWorkerFailureBackoff{
        .address = std::string(address),
        .retry_after = retry_after,
    });
    return;
  }
  existing->retry_after = retry_after;
}

bool consume_vdsd_worker_retry(std::vector<VdsdWorkerFailureBackoff> &backoffs,
                               std::string_view address,
                               std::chrono::steady_clock::time_point now) {
  const auto existing =
      std::find_if(backoffs.begin(), backoffs.end(),
                   [&](const auto &entry) { return entry.address == address; });
  if (existing == backoffs.end()) {
    return true;
  }
  if (now < existing->retry_after) {
    return false;
  }
  backoffs.erase(existing);
  return true;
}

std::string format_control_list_reply(
    const ConfigDb &db,
    std::span<const VdsdControlControllerStatus> controllers) {
  const auto runtime_status = [&](std::string_view address) {
    for (const auto &controller : controllers) {
      if (controller.address == address) {
        return &controller;
      }
    }
    return static_cast<const VdsdControlControllerStatus *>(nullptr);
  };

  std::string reply;
  for (const auto &controller : db.controllers) {
    const VdsdControlControllerStatus *status =
        runtime_status(controller.address);
    const bool connected = status && status->connected;
    reply += "{";
    reply += jsonl_string_field("address", controller.address);
    reply += ',';
    reply += jsonl_bool_field("connected", connected);
    reply += ',';
    reply +=
        jsonl_string_field("path", connected && status ? status->path : "");
    reply += ',';
    reply += jsonl_string_field("profile",
                                controller_profile_name(controller.profile));
    reply += ',';
    reply += jsonl_unsigned_array_field("ports", controller.ports);
    reply += "}\n";
  }
  return reply;
}

const ControllerTarget *
find_controller_target_by_address(std::span<const ControllerTarget> targets,
                                  std::string_view address) {
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &candidate) {
        return candidate.address == address;
      });
  return target == targets.end() ? nullptr : &*target;
}

std::string format_control_target_jsonl(const ControllerTarget &target,
                                        bool registered) {
  std::string line = "{";
  line += jsonl_string_field("address", target.address);
  line += ',';
  line += jsonl_string_field("name", target.name);
  line += ',';
  line += jsonl_bool_field("online", target.online);
  line += ',';
  line += jsonl_bool_field("registered", registered);
  line += "}\n";
  return line;
}

std::string
format_control_list_targets_reply(const ConfigDb &db,
                                  std::span<const ControllerTarget> targets) {
  std::vector<ControllerTarget> sorted_targets(targets.begin(), targets.end());
  std::sort(sorted_targets.begin(), sorted_targets.end(),
            [](const ControllerTarget &left, const ControllerTarget &right) {
              return left.address < right.address;
            });

  std::string reply;
  for (const auto &target : sorted_targets) {
    reply += format_control_target_jsonl(
        target,
        find_controller_config_by_address(db, target.address) != nullptr);
  }
  return reply;
}

std::vector<std::string>
port_status_paths(std::span<const VdsdControlPortStatus> ports) {
  std::vector<std::string> paths;
  paths.reserve(ports.size());
  for (const auto &port : ports) {
    paths.push_back(port.path);
  }
  return paths;
}

std::string format_control_error_reply(std::string_view error) {
  std::string reply = "{";
  reply += jsonl_bool_field("OK", false);
  reply += ',';
  reply += jsonl_string_field("error", error);
  reply += "}\n";
  return reply;
}

std::string format_control_attach_reply(bool ok, std::string_view error,
                                        const ControllerConfig &config) {
  std::string reply = "{";
  reply += jsonl_bool_field("OK", ok);
  reply += ',';
  reply += jsonl_string_field("error", error);
  reply += ',';
  reply += jsonl_string_field("address", config.address);
  reply += ',';
  reply += jsonl_unsigned_array_field("ports", config.ports);
  reply += ',';
  reply +=
      jsonl_string_field("profile", controller_profile_name(config.profile));
  reply += "}\n";
  return reply;
}

std::string format_control_detach_reply(bool ok, std::string_view error,
                                        std::string_view address) {
  std::string reply = "{";
  reply += jsonl_bool_field("OK", ok);
  reply += ',';
  reply += jsonl_string_field("error", error);
  reply += ',';
  reply += jsonl_string_field("address", address);
  reply += "}\n";
  return reply;
}

std::string format_control_trace_reply(bool ok, std::string_view error,
                                       std::uint32_t trace_flags) {
  std::string reply = "{";
  reply += jsonl_bool_field("OK", ok);
  reply += ',';
  reply += jsonl_string_field("error", error);
  reply += ',';
  reply += jsonl_bool_field("trace", (trace_flags & kTraceAll) != 0);
  reply += ",\"scope\":[";
  bool wrote_scope = false;
  if (trace_enabled(trace_flags, kTraceInputAudio)) {
    reply += jsonl_string_value("input-audio");
    wrote_scope = true;
  }
  if (trace_enabled(trace_flags, kTraceInputControl)) {
    if (wrote_scope) {
      reply += ',';
    }
    reply += jsonl_string_value("input-control");
    wrote_scope = true;
  }
  if (trace_enabled(trace_flags, kTraceOutput)) {
    if (wrote_scope) {
      reply += ',';
    }
    reply += jsonl_string_value("output");
  }
  reply += ']';
  reply += "}\n";
  return reply;
}

std::string handle_attach_control_request(
    std::span<const JsonlField> fields, const std::string &db_path,
    std::span<const VdsdControlPortStatus> ports,
    const std::function<std::vector<ControllerTarget>()> &list_targets,
    bool &reload_requested, Logger &logger) {
  constexpr std::string_view context = "control request";
  ControllerConfig config{
      .address = {},
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };
  try {
    const std::string address = normalize_bluetooth_address(
        require_jsonl_string(fields, "address", context));
    const ConfigDb current_db = load_config_db(db_path);
    if (const ControllerConfig *registered =
            find_controller_config_by_address(current_db, address)) {
      config = *registered;
    }

    static constexpr std::string_view expected[] = {
        "command",
        "address",
        "profile",
        "ports",
    };
    reject_unknown_jsonl_fields(fields, expected, context);

    if (!config.address.empty()) {
      const std::string error = "address is already registered";
      logger.log(vds::LogScope::Control, LogLevel::Warn,
                 "attach rejected " + error + ": " + config.address);
      return format_control_attach_reply(false, error, config);
    }

    config.address = address;
    config.profile = parse_controller_profile(
        require_jsonl_string(fields, "profile", context));
    config.ports = require_jsonl_unsigned_array(fields, "ports", context);

    const std::vector<ControllerTarget> targets = list_targets();
    if (find_controller_target_by_address(targets, config.address) == nullptr) {
      const std::string error =
          "address is not paired, not supported, or not attachable; run "
          "vdsctl list-targets";
      logger.log(vds::LogScope::Control, LogLevel::Warn,
                 "attach rejected " + error);
      config = {};
      return format_control_attach_reply(false, error, config);
    }

    const std::vector<std::string> devices = port_status_paths(ports);
    bool already_registered = false;
    update_config_db(db_path, [&](ConfigDb &db) {
      if (const ControllerConfig *registered =
              find_controller_config_by_address(db, config.address)) {
        config = *registered;
        already_registered = true;
        return;
      }
      upsert_controller_config(db, config);
      validate_config_assignments(db, devices);
    });
    if (already_registered) {
      const std::string error = "address is already registered";
      logger.log(vds::LogScope::Control, LogLevel::Warn,
                 "attach rejected " + error + ": " + config.address);
      return format_control_attach_reply(false, error, config);
    }
  } catch (const std::exception &error) {
    return format_control_attach_reply(false, error.what(), config);
  }

  reload_requested = true;
  logger.log(vds::LogScope::Control, LogLevel::Info,
             "attached controller config " + format_controller_config(config));
  return format_control_attach_reply(true, "", config);
}

std::string handle_detach_control_request(std::span<const JsonlField> fields,
                                          const std::string &db_path,
                                          bool &reload_requested,
                                          Logger &logger) {
  constexpr std::string_view context = "control request";
  std::string address;
  bool removed = false;
  try {
    address = normalize_bluetooth_address(
        require_jsonl_string(fields, "address", context));
    const ConfigDb current_db = load_config_db(db_path);
    if (find_controller_config_by_address(current_db, address) == nullptr) {
      address = {};
    }

    static constexpr std::string_view expected[] = {
        "command",
        "address",
    };
    reject_unknown_jsonl_fields(fields, expected, context);

    if (address.empty()) {
      return format_control_detach_reply(false, "address is not registered",
                                         address);
    }
    update_config_db(db_path, [&](ConfigDb &db) {
      removed = remove_controller_config(db, address);
    });
  } catch (const std::exception &error) {
    return format_control_detach_reply(false, error.what(), address);
  }
  reload_requested = removed;
  logger.log(vds::LogScope::Control, LogLevel::Info,
             std::string(removed ? "detached " : "detach ignored ") + address);

  if (!removed) {
    address = {};
    return format_control_detach_reply(false, "address is not registered", "");
  }
  return format_control_detach_reply(true, "", address);
}

std::string handle_list_control_request(
    std::span<const JsonlField> fields, const std::string &db_path,
    std::span<const VdsdControlControllerStatus> controllers) {
  constexpr std::string_view context = "control request";
  static constexpr std::string_view expected[] = {
      "command",
  };
  reject_unknown_jsonl_fields(fields, expected, context);
  return format_control_list_reply(load_config_db(db_path), controllers);
}

std::string handle_list_targets_control_request(
    std::span<const JsonlField> fields, const std::string &db_path,
    const std::function<std::vector<ControllerTarget>()> &list_targets) {
  constexpr std::string_view context = "control request";
  static constexpr std::string_view expected[] = {
      "command",
  };
  reject_unknown_jsonl_fields(fields, expected, context);
  return format_control_list_targets_reply(load_config_db(db_path),
                                           list_targets());
}

std::string handle_trace_control_request(std::span<const JsonlField> fields,
                                         std::uint32_t &trace_flags,
                                         Logger &logger) {
  constexpr std::string_view context = "control request";
  bool enabled = false;
  std::uint32_t scope = 0;
  try {
    static constexpr std::string_view expected[] = {
        "command",
        "mode",
        "scope",
    };
    reject_unknown_jsonl_fields(fields, expected, context);

    const std::string mode = require_jsonl_string(fields, "mode", context);
    scope = parse_trace_scope(require_jsonl_string(fields, "scope", context));
    if (mode == "on") {
      trace_flags |= scope;
      enabled = true;
    } else if (mode == "off") {
      trace_flags &= ~scope;
    } else {
      throw std::runtime_error("trace mode must be on or off");
    }
  } catch (const std::exception &error) {
    return format_control_trace_reply(false, error.what(), trace_flags);
  }

  logger.log(vds::LogScope::Control, LogLevel::Info,
             "trace " + trace_scope_name(scope) + " " +
                 (enabled ? "enabled" : "disabled") +
                 " active=" + active_trace_name(trace_flags));
  return format_control_trace_reply(true, "", trace_flags);
}

std::string handle_vdsd_control_command(
    std::string_view request, const std::string &db_path,
    std::span<const VdsdControlControllerStatus> controllers,
    std::span<const VdsdControlPortStatus> ports,
    const std::function<std::vector<ControllerTarget>()> &list_targets,
    std::uint32_t &trace_flags, bool &reload_requested, Logger &logger) {
  try {
    constexpr std::string_view context = "control request";
    const std::vector<JsonlField> fields = parse_jsonl_object(request, context);
    const std::string command =
        require_jsonl_string(fields, "command", context);
    if (command == "attach") {
      return handle_attach_control_request(fields, db_path, ports, list_targets,
                                           reload_requested, logger);
    }
    if (command == "detach") {
      return handle_detach_control_request(fields, db_path, reload_requested,
                                           logger);
    }
    if (command == "list") {
      return handle_list_control_request(fields, db_path, controllers);
    }
    if (command == "list-targets") {
      return handle_list_targets_control_request(fields, db_path, list_targets);
    }
    if (command == "trace") {
      return handle_trace_control_request(fields, trace_flags, logger);
    }

    logger.log(vds::LogScope::Control, LogLevel::Warn,
               "unknown command: " + std::string(command));
    return format_control_error_reply("unknown command: " + command);
  } catch (const std::exception &error) {
    return format_control_error_reply(error.what());
  }
}

} // namespace vds
