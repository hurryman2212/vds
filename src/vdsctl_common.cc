// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "jsonl.hh"
#include "vdsctl_common.hh"
#ifdef _WIN32
#include "platform/win32/vds_tuning.hh"
#endif

namespace vds {

namespace {

std::string attach_request(const ControllerConfig &config) {
  std::string request = "{";
  request += jsonl_string_field("command", "attach");
  request += ',';
  request += jsonl_string_field("address", config.address);
  request += ',';
  request +=
      jsonl_string_field("profile", controller_profile_name(config.profile));
  request += ',';
  request += jsonl_unsigned_array_field("ports", config.ports);
  request += "}\n";
  return request;
}

std::string detach_request(std::string_view address) {
  std::string request = "{";
  request += jsonl_string_field("command", "detach");
  request += ',';
  request += jsonl_string_field("address", address);
  request += "}\n";
  return request;
}

std::string list_request() { return "{\"command\":\"list\"}\n"; }

std::string list_targets_request() {
  return "{\"command\":\"list-targets\"}\n";
}

std::string trace_request(const VdsctlTraceCommand &trace) {
  std::string request = "{";
  request += jsonl_string_field("command", "trace");
  request += ',';
  request += jsonl_string_field("mode", trace.enabled ? "on" : "off");
  request += ',';
  request += jsonl_string_field("scope", trace.scope);
  request += "}\n";
  return request;
}

} // namespace

std::string vdsctl_usage(std::string_view version,
                         std::string_view build_year) {
  std::string text = "vdsctl (";
  text += version;
  text += "): vDS management utility - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsctl attach <address> [--ports 0[,1...]|\"\"] "
          "[--profile ds5|dse|\"\"]\n"
          "  vdsctl detach <address>\n"
          "  vdsctl list\n"
          "  vdsctl list-targets\n"
          "  vdsctl trace on|off [--scope <scope>[,<scope>...]]\n"
#ifdef _WIN32
          "  vdsctl gain <0..300>  (percent; 100 = unchanged)\n"
          "  vdsctl tuning        (show persistent gain and realtime policy)\n"
#endif
          "\n"
          "trace scopes:\n"
          "  all, input, input-audio, input-control, output\n";
  return text;
}

VdsctlCommand parse_vdsctl_command(std::string_view command) {
  if (command == "attach") {
    return VdsctlCommand::Attach;
  }
  if (command == "detach") {
    return VdsctlCommand::Detach;
  }
  if (command == "list") {
    return VdsctlCommand::List;
  }
  if (command == "list-targets") {
    return VdsctlCommand::ListTargets;
  }
  if (command == "trace") {
    return VdsctlCommand::Trace;
  }
  throw std::runtime_error("unknown command: " + std::string(command));
}

int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year,
                   const VdsctlPlatform &platform) {
  try {
    if (argc < 2) {
      throw std::runtime_error("command is required");
    }

    if (std::string_view(argv[1]) == "-h" ||
        std::string_view(argv[1]) == "--help") {
      std::cerr << vdsctl_usage(version, build_year);
      return 0;
    }

#ifdef _WIN32
    if (std::string_view(argv[1]) == "gain" || std::string_view(argv[1]) == "tuning") {
      const bool setting = std::string_view(argv[1]) == "gain";
      require_vdsctl_arg_count(argc, setting ? 3 : 2);
      std::string request = "{" + jsonl_string_field("command", argv[1]);
      if (setting) request += "," + jsonl_string_field("value", std::to_string(win::tuning::parse_gain(argv[2])));
      const auto reply = platform.request_control(request + "}\n");
      std::cout << reply;
      return reply.find("\"OK\":false") == std::string::npos ? 0 : 1;
    }
#endif
    switch (parse_vdsctl_command(argv[1])) {
    case VdsctlCommand::Attach:
      std::cout << run_vdsctl_attach(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::Detach:
      std::cout << run_vdsctl_detach(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::List:
      std::cout << run_vdsctl_list(argc, platform.request_control);
      break;
    case VdsctlCommand::ListTargets:
      std::cout << run_vdsctl_list_targets(argc, platform.request_control);
      break;
    case VdsctlCommand::Trace:
      std::cout << run_vdsctl_trace(argc, argv, platform.request_control);
      break;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsctl: " << error.what() << "\n";
    return 1;
  }
}

void require_vdsctl_arg_count(int argc, int expected) {
  if (argc != expected) {
    throw std::runtime_error("unexpected argument");
  }
}

ControllerConfig parse_vdsctl_attach(int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error("attach requires an address");
  }

  ControllerConfig config{
      .address = normalize_bluetooth_address(argv[2]),
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next_arg_is_value = [&] {
      return i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0;
    };
    if (arg == "--profile") {
      config.profile =
          parse_controller_profile(next_arg_is_value() ? argv[++i] : "");
    } else if (arg == "--ports") {
      config.ports = parse_ports(next_arg_is_value() ? argv[++i] : "");
    } else {
      throw std::runtime_error("unknown attach option: " + std::string(arg));
    }
  }

  return config;
}

VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    throw std::runtime_error("trace requires on or off with optional --scope");
  }

  const std::string_view mode = argv[2];
  VdsctlTraceCommand command{
      .enabled = false,
      .scope = "all",
  };
  if (argc == 5) {
    if (std::string_view(argv[3]) != "--scope") {
      throw std::runtime_error("trace option must be --scope");
    }
    command.scope = argv[4];
  }

  if (mode == "on") {
    command.enabled = true;
  } else if (mode == "off") {
    command.enabled = false;
  } else {
    throw std::runtime_error("trace requires on or off");
  }

  return command;
}

std::string run_vdsctl_attach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  const ControllerConfig config = parse_vdsctl_attach(argc, argv);
  return request_control(attach_request(config));
}

std::string run_vdsctl_detach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 3);
  return request_control(detach_request(normalize_bluetooth_address(argv[2])));
}

std::string run_vdsctl_list(
    int argc,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 2);
  return request_control(list_request());
}

std::string run_vdsctl_list_targets(
    int argc,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 2);
  return request_control(list_targets_request());
}

std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(trace_request(parse_vdsctl_trace(argc, argv)));
}

} // namespace vds
