// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uapi/vds.h"
#include "unique_fd.hh"
#include "vds/ds5_protocol.h"
#include "vds_bluez.hh"
#include "vds_bt.hh"
#include "vds_build_info.hh"
#include "vds_common.hh"
#include "vds_config.hh"
#include "vds_io.hh"
#include "vds_log.hh"
#include "vds_profile.hh"
#include "vds_protocol.hh"
#include "vds_udev.hh"
#include "vdsd_common.hh"

namespace {

using Clock = std::chrono::steady_clock;
using vds::duration_us;
using vds::hex_bytes;
using vds::hex_u8;

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";
constexpr const char *kVirtualPortProviderUnavailableReason =
    "virtual port provider unavailable";
constexpr const char *kLinuxVirtualPortProviderUnavailable =
    "vds_hcd kernel module is not loaded or no /dev/vds# ports are available";
constexpr std::size_t kTraceDumpMaxBytes = 96;
constexpr std::size_t kUsbInputButtonsOffset = 8;
constexpr std::size_t kUsbInputButtonsSize = 4;
constexpr std::size_t kUsbInputMuteButtonOffset = 10;
constexpr std::size_t kUsbInputHeadsetOffset = 54;
constexpr std::uint8_t kUsbInputHeadphonesPluggedMask = 0x01;
constexpr std::uint8_t kUsbInputMicPluggedMask = 0x02;
constexpr std::uint8_t kUsbInputMuteButtonMask = 0x04;
constexpr std::uint64_t kInputTraceSummaryInterval = 1000;
constexpr std::uint64_t kOutputTraceSummaryInterval = 250;
constexpr int kMaxPortFramesPerWake = 64;
constexpr int kMaxBtPacketsPerWake = 64;
constexpr int kPendingOutputPollMs = 2;
constexpr auto kInputTraceGapWarn = std::chrono::milliseconds(20);
constexpr auto kInputTraceSlowWriteWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceSlowWarn = std::chrono::milliseconds(5);
constexpr auto kOutputTraceFeatureSlowWarn = std::chrono::milliseconds(20);
/*
 * 0x36 carries both a haptics block and a 10 ms Opus speaker block. Pace the
 * combined packet at the speaker frame interval; using the 64-byte haptics
 * duration here lets speaker audio drift into periodic underruns after a few
 * seconds.
 */
constexpr auto kAudioOutputInterval = std::chrono::milliseconds(10);
constexpr auto kHapticsOutputBlockedRetry = std::chrono::milliseconds(2);
constexpr auto kBluetoothPreemptWait = std::chrono::milliseconds(2000);
constexpr auto kBluetoothPreemptPoll = std::chrono::milliseconds(100);
constexpr int kInitialFeatureReportPollMs = 5000;
constexpr std::size_t kMaxPendingAudioChunks = 8;
constexpr std::size_t kFreshPendingAudioChunks = 4;
constexpr std::uint8_t kTestCommandReportId = 0x80;
constexpr std::uint8_t kTestCommandResultReportId = 0x81;
constexpr std::uint8_t kTestCommandCompleteStatus = 0x02;
constexpr std::uint8_t kTestCommandAudioDevice = 0x06;
constexpr std::uint8_t kTestCommandWaveoutPrepare = 0x04;
constexpr std::uint8_t kTestCommandWaveoutControl = 0x02;
constexpr std::uint8_t kTestCommandSpeakerParam = 0x08;
constexpr std::uint32_t kSpeakerWaveoutFrequencyHz = 1000;
constexpr std::uint32_t kSpeakerWaveoutPeriodFrames =
    VDS_AUDIO_SAMPLE_RATE / kSpeakerWaveoutFrequencyHz;
constexpr double kSpeakerWaveoutTwoPi = 6.28318530717958647692;
/* Arbitrary fixed amplitude used only for synthetic WebHID speaker waveout. */
constexpr std::int16_t kSpeakerWaveoutAmplitude = 12000;
constexpr std::array<std::uint8_t, 3> kInitialFeatureReportIds = {0x09, 0x20,
                                                                  0x05};

volatile sig_atomic_t g_stop_requested = 0;
volatile sig_atomic_t g_log_reopen_requested = 0;

struct Options {
  std::string socket = kDefaultControlSocket;
  std::string log_path;
  std::string db_path;
};

struct LatencyTraceStats {
  std::uint64_t count = 0;
  std::uint64_t total_us = 0;
  std::uint64_t max_us = 0;
};

struct TraceState {
  std::vector<std::uint8_t> last_hid_out;
  std::uint64_t dropped_usb_frame_count = 0;
  std::uint64_t dropped_audio_haptics_count = 0;
  std::uint64_t queue_dropped_audio_haptics_count = 0;
  std::uint64_t stale_audio_haptics_count = 0;
  std::uint64_t blocked_audio_haptics_count = 0;
  std::uint64_t deferred_bt_state_count = 0;
  std::uint64_t coalesced_bt_state_count = 0;
  std::uint64_t blocked_bt_state_count = 0;
  std::uint64_t audio_usb_frame_count = 0;
  std::uint64_t bt_input_count = 0;
  std::uint64_t bt_mic_packet_count = 0;
  std::uint64_t bt_mic_drop_count = 0;
  std::uint64_t bt_mic_decode_fail_count = 0;
  std::uint64_t audio_in_forward_count = 0;
  LatencyTraceStats output_hid_latency;
  LatencyTraceStats output_feature_latency;
  LatencyTraceStats output_audio_latency;
  LatencyTraceStats output_audio_extract_latency;
  LatencyTraceStats output_audio_send_latency;
  std::uint64_t input_write_count = 0;
  std::uint64_t input_write_total_us = 0;
  std::uint64_t input_write_max_us = 0;
  std::uint64_t input_gap_max_us = 0;
  Clock::time_point last_input_time{};
  bool have_last_input_time = false;
  std::array<std::uint8_t, kUsbInputButtonsSize> last_input_buttons{};
  bool have_last_input_buttons = false;
  bool audio_haptics_nonzero_seen = false;
  bool haptics_burst_active = false;
  std::uint64_t haptics_burst_chunks = 0;
};

using vds::kTraceInputAudio;
using vds::kTraceInputControl;
using vds::kTraceOutput;
using vds::trace_enabled;
using vds::trim_command;

struct VirtualPort {
  std::string path;
  vds::UniqueFd fd;
  vds::PcmAudioExtractor extractor;
  vds::PcmAudioExtractor waveout_extractor;
  vds::MicAudioDecoder mic_decoder;
  vds::HapticsPacketBuilder haptics_builder;
  vds::DsOutputState output_state;
  std::deque<vds::AudioChunk> pending_audio_chunks;
  std::optional<vds::DsState> last_sent_bt_state;
  std::optional<vds::DsState> pending_bt_state;
  std::optional<vds::BtStateReport> pending_bt_state_report;
  Clock::time_point next_haptics_send_time{};
  bool audio_out_stream_active = false;
  bool audio_in_stream_active = false;
  bool mic_muted = false;
  bool mute_button_down = false;
  bool headset_plugged = false;
  bool headset_mic_plugged = false;
  bool speaker_waveout_selected = true;
  bool speaker_waveout_active = false;
  std::uint32_t speaker_waveout_phase = 0;
  std::array<std::vector<std::uint8_t>, 256> feature_cache;
  std::array<bool, 256> feature_cached;
  std::vector<std::uint8_t> pending_feature_reports;
  TraceState trace_state;
};

struct ControllerRuntime {
  vds::ControllerConfig config;
  std::string device;
  std::optional<std::uint32_t> detected_profile;
  std::optional<vds::BtL2capBackend> backend;
  vds::UniqueFd pending_control_fd;
  vds::UniqueFd pending_interrupt_fd;
  bool virtual_connected = false;
  std::string last_error;
};

enum class EventType : std::uint32_t {
  Control = 1,
  Port = 2,
  BtControl = 3,
  BtInterrupt = 4,
  BtAcceptControl = 5,
  BtAcceptInterrupt = 6,
  Udev = 7,
};

struct EventSource {
  EventType type;
  std::size_t index;
};

class SocketPathGuard {
public:
  explicit SocketPathGuard(std::string path) : path_(std::move(path)) {}
  ~SocketPathGuard() { ::unlink(path_.c_str()); }

  SocketPathGuard(const SocketPathGuard &) = delete;
  SocketPathGuard &operator=(const SocketPathGuard &) = delete;

private:
  std::string path_;
};

void signal_handler(int signal) {
  if (signal == SIGHUP) {
    g_log_reopen_requested = 1;
    return;
  }
  g_stop_requested = 1;
}

void install_signal_handler(int signal) {
  struct sigaction action{};
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  if (::sigaction(signal, &action, nullptr) < 0) {
    throw std::runtime_error("sigaction failed: " +
                             std::string(std::strerror(errno)));
  }
}

Options parse_platform_args(int argc, char **argv) {
  Options options;
  vds::VdsdCommonOptions common = vds::default_vdsd_common_options();
  const std::string platform_options =
      std::string("[--socket ") + kDefaultControlSocket + "]";
  const auto next_value = [&](int &index, std::string_view option) {
    if (index + 1 >= argc) {
      throw std::runtime_error(std::string(option) + " requires a value");
    }
    return std::string(argv[++index]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (vds::parse_vdsd_common_option(argc, argv, i, common)) {
      if (common.help_requested) {
        vds::print_vdsd_usage(std::cerr, vds::kVersion, vds::kBuildYear,
                              platform_options);
        std::exit(0);
      }
    } else if (arg == "--socket") {
      options.socket = next_value(i, arg);
    } else {
      throw std::runtime_error("unknown argument: " + std::string(arg));
    }
  }
  options.db_path = common.db_path;
  options.log_path = common.log_path;
  return options;
}

int open_required(const std::string &path, int flags) {
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    throw std::runtime_error("failed to open " + path + ": " +
                             std::strerror(errno));
  }
  return fd;
}

void trace_output_latency(const std::string &device, std::string_view label,
                          LatencyTraceStats &stats, Clock::duration duration,
                          Clock::duration slow_threshold, vds::Logger &logger) {
  const std::uint64_t elapsed_us = duration_us(duration);
  ++stats.count;
  stats.total_us += elapsed_us;
  stats.max_us = std::max(stats.max_us, elapsed_us);

  if (duration >= slow_threshold) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               device + " slow " + std::string(label) +
                   " latency count=" + std::to_string(stats.count) +
                   " latency_us=" + std::to_string(elapsed_us));
  }
  if (stats.count == 1 || stats.count % kOutputTraceSummaryInterval == 0) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
               device + " " + std::string(label) +
                   " latency summary count=" + std::to_string(stats.count) +
                   " avg_us=" + std::to_string(stats.total_us / stats.count) +
                   " max_us=" + std::to_string(stats.max_us));
  }
}

void trace_hid_out_report(const std::string &device,
                          std::span<const std::uint8_t> payload,
                          TraceState &trace_state, vds::Logger &logger) {
  std::ostringstream line;
  line << device << " hid out";
  if (!payload.empty()) {
    line << " report=" << hex_u8(payload[0]);
  }
  if (payload.size() >= 5) {
    /*
     * USB report 0x02 carries SetStateData after the report ID. These fields
     * show whether a plain rumble request exists before the HD haptics audio
     * path is involved.
     */
    line << " flags0=" << hex_u8(payload[1]) << " flags1=" << hex_u8(payload[2])
         << " rumble_r=" << static_cast<unsigned>(payload[3])
         << " rumble_l=" << static_cast<unsigned>(payload[4]);
  }
  if (payload.size() >= 40) {
    line << " light_flags=" << hex_u8(payload[39]);
  }
  logger.log(vds::LogScope::Hid, vds::LogLevel::Debug, line.str());

  const bool hid_out_changed =
      trace_state.last_hid_out.size() != payload.size() ||
      !std::equal(trace_state.last_hid_out.begin(),
                  trace_state.last_hid_out.end(), payload.begin());
  if (!hid_out_changed) {
    return;
  }

  trace_state.last_hid_out.assign(payload.begin(), payload.end());
  std::ostringstream changed;
  changed << device
          << " hid out changed raw=" << hex_bytes(payload, kTraceDumpMaxBytes);
  if (payload.size() >= 33 && payload[0] == VDS_USB_OUTPUT_REPORT_ID) {
    /*
     * SetStateData begins after the report ID. Offsets 10..20 and 21..31 are
     * the right/left adaptive trigger blocks from the DualSense USB report.
     */
    changed << " right_trigger=" << hex_bytes(payload.subspan(11, 11), 11)
            << " left_trigger=" << hex_bytes(payload.subspan(22, 11), 11);
  }
  logger.log(vds::LogScope::Hid, vds::LogLevel::Debug, changed.str());
}

const char *usb_interface_type_name(std::uint8_t type) {
  switch (type) {
  case VDS_USB_INTERFACE_HID:
    return "hid";
  case VDS_USB_INTERFACE_AUDIO_OUT:
    return "audio_out";
  case VDS_USB_INTERFACE_AUDIO_IN:
    return "audio_in";
  default:
    return "unknown";
  }
}

const char *profile_name(std::uint32_t profile) {
  if (profile == VDS_PROFILE_DSE) {
    return "dualsense-edge";
  }
  return "dualsense";
}

std::uint32_t detect_bluetooth_profile(const std::string &address) {
  const auto modalias = vds::bluez_device_modalias(address);
  if (!modalias) {
    throw std::runtime_error("failed to query BlueZ modalias for " + address);
  }
  const auto profile = vds::controller_profile_from_modalias(*modalias);
  if (!profile) {
    throw std::runtime_error("failed to detect Bluetooth device ID for " +
                             address);
  }
  return vds::usb_profile_from_controller_profile(*profile);
}

std::uint32_t resolve_config_profile(const vds::ControllerConfig &controller) {
  switch (controller.profile) {
  case vds::ControllerProfile::Unspecified:
    return detect_bluetooth_profile(controller.address);
  case vds::ControllerProfile::Ds5:
  case vds::ControllerProfile::Dse:
    return vds::usb_profile_from_controller_profile(controller.profile);
  }
  throw std::runtime_error("unknown controller profile");
}

int open_control_socket(const std::string &path) {
  vds::UniqueFd fd(
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open control socket: " +
                             std::string(std::strerror(errno)));
  }

  ::unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("control socket path is too long: " + path);
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  if (::bind(fd.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to bind control socket " + path + ": " +
                             std::strerror(error));
  }
  (void)::chmod(path.c_str(), 0660);

  if (::listen(fd.get(), 4) < 0) {
    const int error = errno;
    throw std::runtime_error("failed to listen on control socket " + path +
                             ": " + std::strerror(error));
  }
  return fd.release();
}

bool write_vds_frame(VirtualPort &port, std::span<const std::uint8_t> bytes,
                     bool trace, vds::Logger &logger) {
  while (true) {
    const ssize_t written = ::write(port.fd.get(), bytes.data(), bytes.size());
    if (written == static_cast<ssize_t>(bytes.size())) {
      return true;
    }
    if (written >= 0) {
      throw std::runtime_error("short " + port.path + " frame write");
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOSPC) {
      ++port.trace_state.dropped_usb_frame_count;
      if (trace) {
        if (port.trace_state.dropped_usb_frame_count == 1 ||
            port.trace_state.dropped_usb_frame_count % 1000 == 0) {
          logger.log(
              vds::LogScope::Port, vds::LogLevel::Warn,
              port.path + " vDS write queue full count=" +
                  std::to_string(port.trace_state.dropped_usb_frame_count));
        }
      }
      return false;
    }
    throw std::runtime_error("write failed: " +
                             std::string(std::strerror(errno)));
  }
}

void cache_feature_report(VirtualPort &port,
                          std::span<const std::uint8_t> report, bool trace,
                          vds::Logger &logger) {
  if (report.empty()) {
    return;
  }

  const std::uint8_t report_id = report[0];
  port.feature_cache[report_id] =
      std::vector<std::uint8_t>(report.begin(), report.end());
  port.feature_cached[report_id] = true;

  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                      port.feature_cache[report_id]);
  (void)write_vds_frame(port, frame, trace, logger);
}

bool flush_pending_bt_state_report(VirtualPort &port,
                                   vds::BtL2capBackend &bt_backend,
                                   std::uint32_t trace_flags,
                                   vds::Logger &logger) {
  if (!port.pending_bt_state_report || !port.pending_bt_state) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (bt_backend.try_send_output_report(*port.pending_bt_state_report)) {
    port.last_sent_bt_state = *port.pending_bt_state;
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " flushed pending BT 0x31 state report");
    }
    return true;
  }

  ++port.trace_state.blocked_bt_state_count;
  if (output_trace && (port.trace_state.blocked_bt_state_count == 1 ||
                       port.trace_state.blocked_bt_state_count % 1000 == 0)) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Warn,
               port.path +
                   " pending BT 0x31 state report still blocked "
                   "count=" +
                   std::to_string(port.trace_state.blocked_bt_state_count));
  }
  return false;
}

void forward_bt_state_if_changed(VirtualPort &port,
                                 vds::BtL2capBackend &bt_backend,
                                 std::uint32_t trace_flags, vds::Logger &logger,
                                 std::string_view reason) {
  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);

  (void)flush_pending_bt_state_report(port, bt_backend, trace_flags, logger);

  const vds::DsState state = port.output_state.state();
  if (port.last_sent_bt_state && *port.last_sent_bt_state == state) {
    if (port.pending_bt_state) {
      port.pending_bt_state.reset();
      port.pending_bt_state_report.reset();
      ++port.trace_state.coalesced_bt_state_count;
    }
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " skipped: BT state unchanged");
    }
    return;
  }
  if (port.pending_bt_state && *port.pending_bt_state == state) {
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " skipped: BT state already pending");
    }
    return;
  }

  const auto packet = port.output_state.build_bt_state_report();
  if (bt_backend.try_send_output_report(packet)) {
    port.last_sent_bt_state = state;
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " " + std::string(reason) +
                     " forwarded as BT 0x31 state report");
    }
    return;
  }

  if (port.pending_bt_state) {
    ++port.trace_state.coalesced_bt_state_count;
  }
  ++port.trace_state.deferred_bt_state_count;
  port.pending_bt_state = state;
  port.pending_bt_state_report = packet;
  if (output_trace && (port.trace_state.deferred_bt_state_count == 1 ||
                       port.trace_state.deferred_bt_state_count % 1000 == 0)) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Warn,
               port.path + " deferred BT 0x31 state report count=" +
                   std::to_string(port.trace_state.deferred_bt_state_count) +
                   " coalesced=" +
                   std::to_string(port.trace_state.coalesced_bt_state_count));
  }
}

void ioctl_noarg(int fd, unsigned long request, const char *name) {
  if (::ioctl(fd, request) < 0) {
    throw std::runtime_error(std::string(name) +
                             " failed: " + std::strerror(errno));
  }
}

void ioctl_set_profile(int fd, std::uint32_t profile) {
  vds_profile_config config{
      .profile = profile,
      .polling_rate_mode = 0,
  };
  if (::ioctl(fd, VDS_IOC_SET_PROFILE, &config) < 0) {
    throw std::runtime_error("VDS_IOC_SET_PROFILE failed: " +
                             std::string(std::strerror(errno)));
  }
}

void reset_virtual_port(VirtualPort &port) {
  port.extractor = vds::PcmAudioExtractor{};
  port.waveout_extractor = vds::PcmAudioExtractor{};
  port.mic_decoder = vds::MicAudioDecoder{};
  port.haptics_builder = vds::HapticsPacketBuilder{};
  port.output_state = vds::DsOutputState{};
  port.pending_audio_chunks.clear();
  port.last_sent_bt_state.reset();
  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  port.next_haptics_send_time = {};
  port.audio_out_stream_active = false;
  port.audio_in_stream_active = false;
  port.mic_muted = false;
  port.mute_button_down = false;
  port.headset_plugged = false;
  port.headset_mic_plugged = false;
  port.speaker_waveout_selected = true;
  port.speaker_waveout_active = false;
  port.speaker_waveout_phase = 0;
  port.feature_cache = {};
  port.feature_cached = {};
  port.pending_feature_reports.clear();
  port.trace_state = TraceState{};
}

void disconnect_virtual_port(VirtualPort &port, vds::Logger &logger) {
  port.pending_audio_chunks.clear();
  port.next_haptics_send_time = {};
  port.audio_out_stream_active = false;
  port.audio_in_stream_active = false;
  port.mic_muted = false;
  port.mute_button_down = false;
  port.speaker_waveout_active = false;
  port.speaker_waveout_phase = 0;
  try {
    ioctl_noarg(port.fd.get(), VDS_IOC_DISCONNECT, "VDS_IOC_DISCONNECT");
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
               port.path + " virtual USB disconnected");
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
               port.path + " disconnect failed: " + error.what());
  }
}

void handle_frame(const vds_frame_header &header,
                  std::span<const std::uint8_t> payload,
                  vds::BtL2capBackend *bt_backend, std::uint32_t trace_flags,
                  VirtualPort &port, vds::Logger &logger) {
  if (header.type == VDS_FRAME_USB_INTERFACE) {
    if (payload.size() != sizeof(vds_usb_interface_event)) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 port.path + " malformed USB interface event len=" +
                     std::to_string(payload.size()));
      return;
    }

    vds_usb_interface_event event{};
    std::memcpy(&event, payload.data(), sizeof(event));

    std::ostringstream line;
    line << port.path << " USB set_interface type="
         << usb_interface_type_name(event.interface_type)
         << " number=" << static_cast<unsigned>(event.interface_number)
         << " alt=" << static_cast<unsigned>(event.altsetting);
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT ||
        event.interface_type == VDS_USB_INTERFACE_AUDIO_IN) {
      line << " stream=" << (event.altsetting != 0 ? "on" : "off");
    }
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info, line.str());
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT) {
      port.audio_out_stream_active = event.altsetting != 0;
      if (!port.audio_out_stream_active) {
        port.pending_audio_chunks.clear();
        port.extractor = vds::PcmAudioExtractor{};
      } else {
        port.output_state.set_audio_out_stream_active(true,
                                                      port.headset_plugged);
        if (bt_backend) {
          forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                      "audio out interface");
        }
      }
    } else if (event.interface_type == VDS_USB_INTERFACE_AUDIO_IN) {
      port.audio_in_stream_active = event.altsetting != 0;
      port.mic_decoder = vds::MicAudioDecoder{};
      if (bt_backend) {
        const auto state_report = port.output_state.build_bt_mic_state_report(
            port.audio_in_stream_active, port.mic_muted);
        bt_backend->try_send_output_report(state_report);
        const auto report =
            port.output_state.build_bt_mic_report(port.audio_in_stream_active);
        const bool sent = bt_backend->try_send_output_report(report);
        if (trace_enabled(trace_flags, kTraceInputAudio)) {
          logger.log(
              vds::LogScope::InputAudio, vds::LogLevel::Info,
              port.path + " mic " +
                  std::string(port.audio_in_stream_active ? "open" : "close") +
                  " sent=" + (sent ? "yes" : "no"));
        }
      }
    }
    return;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    std::ostringstream line;
    line << port.path << " frame " << vds::frame_type_name(header.type)
         << " len=" << header.length << " seq=" << header.sequence;

    bool emit_trace = true;
    if (header.type == VDS_FRAME_USB_AUDIO_OUT &&
        payload.size() >= VDS_AUDIO_CHANNELS * sizeof(std::int16_t)) {
      std::array<int, VDS_AUDIO_CHANNELS> peaks{};
      const std::size_t frame_size = VDS_AUDIO_CHANNELS * sizeof(std::int16_t);
      const std::size_t frames = payload.size() / frame_size;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t *base = payload.data() + frame * frame_size;
        for (std::size_t channel = 0; channel < VDS_AUDIO_CHANNELS; ++channel) {
          const auto low =
              static_cast<std::uint16_t>(base[channel * sizeof(std::int16_t)]);
          const auto high = static_cast<std::uint16_t>(
                                base[channel * sizeof(std::int16_t) + 1])
                            << 8;
          const auto sample = static_cast<std::int16_t>(low | high);
          peaks[channel] =
              std::max(peaks[channel], std::abs(static_cast<int>(sample)));
        }
      }

      ++port.trace_state.audio_usb_frame_count;
      const bool haptics_nonzero = peaks[2] != 0 || peaks[3] != 0;
      emit_trace =
          port.trace_state.audio_usb_frame_count == 1 ||
          port.trace_state.audio_usb_frame_count % 250 == 0 ||
          (haptics_nonzero && !port.trace_state.audio_haptics_nonzero_seen);
      port.trace_state.audio_haptics_nonzero_seen |= haptics_nonzero;
      line << " pcm_peak_ch0=" << peaks[0] << " pcm_peak_ch1=" << peaks[1]
           << " pcm_peak_ch2=" << peaks[2] << " pcm_peak_ch3=" << peaks[3];
    }

    if (emit_trace) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Debug, line.str());
    }
  }

  if (header.type == VDS_FRAME_USB_HID_OUT) {
    const auto start = Clock::now();
    if (output_trace) {
      trace_hid_out_report(port.path, payload, port.trace_state, logger);
    }
    if (!port.output_state.apply_usb_output_report(payload)) {
      if (output_trace) {
        logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                   port.path + " hid out ignored: unsupported output report");
      }
      return;
    }
    if (port.audio_out_stream_active || port.speaker_waveout_active) {
      port.output_state.set_audio_out_stream_active(true, port.headset_plugged);
    }
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " hid out accepted for BT state update");
    }

    if (bt_backend) {
      forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                  "hid out");
    }

    if (output_trace) {
      trace_output_latency(port.path, "hid_out",
                           port.trace_state.output_hid_latency,
                           Clock::now() - start, kOutputTraceSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_GET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    const unsigned requested_length =
        payload.size() >= 3
            ? static_cast<unsigned>(payload[1] |
                                    (static_cast<unsigned>(payload[2]) << 8))
            : (payload.size() >= 2 ? static_cast<unsigned>(payload[1]) : 0);
    const bool cache_hit = port.feature_cached[report_id];
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature get report=" + hex_u8(report_id) +
                     " request_len=" + std::to_string(requested_length) +
                     " cache=" + (cache_hit ? "hit" : "miss") + " forwarded=" +
                     ((bt_backend && !cache_hit) ? "yes" : "no"));
    }
    if (cache_hit) {
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY,
                                          port.feature_cache[report_id]);
      (void)write_vds_frame(port, frame, output_trace, logger);
      if (output_trace) {
        trace_output_latency(
            port.path, "feature_get", port.trace_state.output_feature_latency,
            Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
      }
      return;
    }
    if (bt_backend) {
      port.pending_feature_reports.push_back(report_id);
      bt_backend->send_feature_get(report_id);
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_get", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type == VDS_FRAME_USB_FEATURE_SET) {
    const auto start = Clock::now();
    if (payload.empty()) {
      return;
    }
    const std::uint8_t report_id = payload[0];
    if (output_trace) {
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature set report=" + hex_u8(report_id) +
                     " len=" + std::to_string(payload.size()) +
                     " forwarded=" + (bt_backend ? "yes" : "no"));
      logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                 port.path + " feature set payload=" +
                     hex_bytes(payload, kTraceDumpMaxBytes));
    }
    if (payload.size() >= 3 && payload[0] == kTestCommandReportId) {
      std::size_t command_offset = 1;
      if (payload.size() > command_offset &&
          payload[command_offset] == kTestCommandReportId) {
        ++command_offset;
      }
      if (payload.size() > command_offset + 1) {
        const std::uint8_t command_device = payload[command_offset];
        const std::uint8_t command_action = payload[command_offset + 1];
        const std::size_t command_data_offset = command_offset + 2;

        std::array<std::uint8_t, VDS_USB_INPUT_REPORT_SIZE> test_result{};
        test_result[0] = kTestCommandResultReportId;
        test_result[1] = command_device;
        test_result[2] = command_action;
        test_result[3] = kTestCommandCompleteStatus;
        cache_feature_report(port, test_result, output_trace, logger);
        if (output_trace) {
          logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
                     port.path + " cached WebHID test result device=" +
                         hex_u8(command_device) + " action=" +
                         hex_u8(command_action) + " status=complete");
        }

        if (command_device == kTestCommandAudioDevice &&
            command_action == kTestCommandWaveoutPrepare) {
          port.speaker_waveout_selected =
              payload.size() > command_data_offset + 2 &&
              payload[command_data_offset + 2] == kTestCommandSpeakerParam;
          if (output_trace) {
            logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                       port.path + " WebHID waveout target=" +
                           std::string(port.speaker_waveout_selected
                                           ? "speaker"
                                           : "headphone"));
          }
        } else if (command_device == kTestCommandAudioDevice &&
                   command_action == kTestCommandWaveoutControl &&
                   payload.size() > command_data_offset) {
          const bool enable = payload[command_data_offset] != 0;
          const bool speaker_waveout = enable && port.speaker_waveout_selected;
          port.speaker_waveout_active = speaker_waveout;
          port.speaker_waveout_phase = 0;
          port.waveout_extractor = vds::PcmAudioExtractor{};
          port.output_state.set_audio_out_stream_active(speaker_waveout,
                                                        port.headset_plugged);
          port.audio_out_stream_active = speaker_waveout;
          if (!speaker_waveout) {
            port.pending_audio_chunks.clear();
          }
          if (bt_backend) {
            forward_bt_state_if_changed(port, *bt_backend, trace_flags, logger,
                                        speaker_waveout
                                            ? "WebHID speaker waveout route on"
                                            : "WebHID waveout route off");
          }
          if (output_trace) {
            logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                       port.path + " WebHID waveout " +
                           std::string(enable ? "on" : "off") + " target=" +
                           std::string(port.speaker_waveout_selected
                                           ? "speaker"
                                           : "headphone") +
                           " synthesized=" + (speaker_waveout ? "yes" : "no"));
          }
        }
      }
    }
    if (bt_backend) {
      bt_backend->send_feature_set(payload);
    }
    if (output_trace) {
      trace_output_latency(
          port.path, "feature_set", port.trace_state.output_feature_latency,
          Clock::now() - start, kOutputTraceFeatureSlowWarn, logger);
    }
    return;
  }

  if (header.type != VDS_FRAME_USB_AUDIO_OUT) {
    return;
  }

  if (!bt_backend) {
    return;
  }

  std::size_t queued_chunks = 0;
  std::size_t dropped_chunks = 0;
  const auto audio_start = Clock::now();
  const auto extract_start = Clock::now();
  const auto chunks = port.extractor.push_usb_audio(payload);
  const auto extract_duration = Clock::now() - extract_start;
  for (const auto &chunk : chunks) {
    if (output_trace) {
      if (chunk.has_haptics_signal) {
        if (!port.trace_state.haptics_burst_active) {
          logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                     port.path + " haptics burst start");
          port.trace_state.haptics_burst_active = true;
          port.trace_state.haptics_burst_chunks = 0;
        }
        ++port.trace_state.haptics_burst_chunks;
      } else if (port.trace_state.haptics_burst_active) {
        logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
                   port.path + " haptics burst end chunks=" +
                       std::to_string(port.trace_state.haptics_burst_chunks));
        port.trace_state.haptics_burst_active = false;
        port.trace_state.haptics_burst_chunks = 0;
      }
    }

    /*
     * USB isochronous URBs can arrive in bursts. Queue completed 0x36 audio
     * chunks here and let flush_pending_outputs() send them at the 10 ms
     * speaker frame interval, otherwise speaker/haptics audio turns into
     * audible bursts.
     */
    if (port.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      port.pending_audio_chunks.pop_front();
      ++dropped_chunks;
      ++port.trace_state.dropped_audio_haptics_count;
      ++port.trace_state.queue_dropped_audio_haptics_count;
    }

    port.pending_audio_chunks.push_back(chunk);
    ++queued_chunks;
  }
  if (dropped_chunks > 0 &&
      (port.trace_state.dropped_audio_haptics_count == dropped_chunks ||
       port.trace_state.dropped_audio_haptics_count % 1000 == 0)) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Warn,
        port.path + " dropped BT 0x36 haptics packets count=" +
            std::to_string(port.trace_state.dropped_audio_haptics_count) +
            " queue=" +
            std::to_string(port.trace_state.queue_dropped_audio_haptics_count) +
            " blocked=" +
            std::to_string(port.trace_state.blocked_audio_haptics_count));
  }
  if (output_trace && queued_chunks > 0) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Debug,
        port.path + " audio out queued " + std::to_string(queued_chunks) +
            " BT 0x36 haptics packets pending=" +
            std::to_string(port.pending_audio_chunks.size()) +
            " extract_encode_us=" +
            std::to_string(duration_us(extract_duration)) + " total_us=" +
            std::to_string(duration_us(Clock::now() - audio_start)));
  }
  if (output_trace) {
    trace_output_latency(
        port.path, "audio_out", port.trace_state.output_audio_latency,
        Clock::now() - audio_start, kOutputTraceSlowWarn, logger);
    trace_output_latency(port.path, "audio_extract_encode",
                         port.trace_state.output_audio_extract_latency,
                         extract_duration, kOutputTraceSlowWarn, logger);
  }
}

bool handle_vds_frame(VirtualPort &port, vds::BtL2capBackend *bt_backend,
                      std::uint32_t trace_flags, vds::Logger &logger) {
  std::array<std::uint8_t, sizeof(vds_frame_header) + VDS_FRAME_MAX_PAYLOAD>
      frame;
  const ssize_t got = ::read(port.fd.get(), frame.data(), frame.size());
  if (got == 0) {
    throw std::runtime_error(port.path + " closed");
  }
  if (got < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return false;
    }
    throw std::runtime_error(
        port.path + " read failed: " + std::string(std::strerror(errno)));
  }
  if (static_cast<std::size_t>(got) < sizeof(vds_frame_header)) {
    throw std::runtime_error(port.path + " short VDS frame header");
  }
  vds_frame_header header{};
  std::memcpy(&header, frame.data(), sizeof(header));
  if (header.length > VDS_FRAME_MAX_PAYLOAD) {
    throw std::runtime_error(port.path + " oversized VDS frame");
  }
  if (static_cast<std::size_t>(got) != sizeof(header) + header.length) {
    throw std::runtime_error(port.path + " short VDS frame payload");
  }
  const std::span payload(frame.data() + sizeof(header), header.length);
  handle_frame(header, payload, bt_backend, trace_flags, port, logger);
  return true;
}

bool handle_bt_input(VirtualPort &port, vds::BtL2capBackend &bt_backend,
                     std::uint32_t trace_flags, vds::Logger &logger) {
  const auto read_time = Clock::now();
  const auto packet = bt_backend.read_interrupt_packet();
  if (!packet) {
    return false;
  }

  if (vds::bt_input_payload_type(*packet) == vds::BtInputPayloadType::Audio) {
    const bool input_audio_trace = trace_enabled(trace_flags, kTraceInputAudio);
    ++port.trace_state.bt_mic_packet_count;
    if (!port.audio_in_stream_active) {
      ++port.trace_state.bt_mic_drop_count;
      if (input_audio_trace &&
          (port.trace_state.bt_mic_drop_count == 1 ||
           port.trace_state.bt_mic_drop_count % 1000 == 0)) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Debug,
                   port.path + " dropped mic packet count=" +
                       std::to_string(port.trace_state.bt_mic_drop_count) +
                       " reason=audio_in_stream_off");
      }
      return true;
    }

    const auto opus_payload = vds::bt_mic_opus_payload(*packet);
    if (!opus_payload) {
      ++port.trace_state.bt_mic_decode_fail_count;
      if (input_audio_trace) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Warn,
                   port.path + " malformed mic packet len=" +
                       std::to_string(packet->size()));
      }
      return true;
    }

    try {
      auto pcm = port.mic_decoder.decode(*opus_payload);
      const auto frame = vds::frame_bytes(VDS_FRAME_USB_AUDIO_IN, pcm);
      const bool wrote =
          write_vds_frame(port, frame, input_audio_trace, logger);
      ++port.trace_state.audio_in_forward_count;
      if (input_audio_trace && (port.trace_state.audio_in_forward_count == 1 ||
                                port.trace_state.audio_in_forward_count %
                                        kInputTraceSummaryInterval ==
                                    0)) {
        logger.log(vds::LogScope::InputAudio, vds::LogLevel::Debug,
                   port.path + " mic forwarded count=" +
                       std::to_string(port.trace_state.audio_in_forward_count) +
                       " len=" + std::to_string(pcm.size()) +
                       " written=" + (wrote ? "yes" : "no"));
      }
    } catch (const std::exception &error) {
      ++port.trace_state.bt_mic_decode_fail_count;
      if (input_audio_trace) {
        logger.log(
            vds::LogScope::InputAudio, vds::LogLevel::Warn,
            port.path + " mic decode failed count=" +
                std::to_string(port.trace_state.bt_mic_decode_fail_count) +
                ": " + error.what());
      }
    }
    return true;
  }

  const auto report = vds::bt_input_to_usb_input(*packet);
  if (!report) {
    return true;
  }

  const bool input_trace = trace_enabled(trace_flags, kTraceInputControl);
  if (input_trace) {
    ++port.trace_state.bt_input_count;
    if (port.trace_state.have_last_input_time) {
      const auto gap = read_time - port.trace_state.last_input_time;
      const auto gap_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(gap).count());
      port.trace_state.input_gap_max_us =
          std::max(port.trace_state.input_gap_max_us, gap_us);
      if (gap >= kInputTraceGapWarn) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Warn,
                   port.path + " input gap count=" +
                       std::to_string(port.trace_state.bt_input_count) +
                       " gap_us=" + std::to_string(gap_us));
      }
    }
    port.trace_state.last_input_time = read_time;
    port.trace_state.have_last_input_time = true;

    const std::span buttons(report->data() + kUsbInputButtonsOffset,
                            kUsbInputButtonsSize);
    const bool buttons_changed =
        !port.trace_state.have_last_input_buttons ||
        !std::equal(buttons.begin(), buttons.end(),
                    port.trace_state.last_input_buttons.begin());
    if (buttons_changed) {
      std::copy(buttons.begin(), buttons.end(),
                port.trace_state.last_input_buttons.begin());
      port.trace_state.have_last_input_buttons = true;
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Debug,
                 port.path + " buttons changed raw=" +
                     hex_bytes(buttons, kUsbInputButtonsSize));
    }
  }

  const std::uint8_t headset_status = report->at(kUsbInputHeadsetOffset);
  const bool headset_plugged =
      (headset_status & kUsbInputHeadphonesPluggedMask) != 0;
  const bool headset_mic_plugged =
      (headset_status & kUsbInputMicPluggedMask) != 0;
  const bool mute_button_down =
      (report->at(kUsbInputMuteButtonOffset) & kUsbInputMuteButtonMask) != 0;
  bool headset_mic_changed = false;
  bool mic_mute_changed = false;
  bool output_state_changed = false;
  if (mute_button_down && !port.mute_button_down) {
    port.mic_muted = !port.mic_muted;
    mic_mute_changed = true;
  }
  port.mute_button_down = mute_button_down;
  if (headset_mic_plugged != port.headset_mic_plugged) {
    port.headset_mic_plugged = headset_mic_plugged;
    headset_mic_changed = true;
  }
  if (mic_mute_changed ||
      (headset_mic_changed && port.audio_in_stream_active)) {
    const auto report = port.output_state.build_bt_mic_state_report(
        port.audio_in_stream_active, port.mic_muted);
    bt_backend.try_send_output_report(report);
    if (input_trace) {
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                 port.path + " mic " +
                     std::string(port.mic_muted ? "muted" : "unmuted"));
    }
  }
  if (headset_plugged != port.headset_plugged) {
    port.headset_plugged = headset_plugged;
    if (port.audio_out_stream_active || port.speaker_waveout_active) {
      port.output_state.set_audio_out_stream_active(true, port.headset_plugged);
      output_state_changed = true;
    }
    if (input_trace) {
      logger.log(
          vds::LogScope::InputControl, vds::LogLevel::Info,
          port.path + " headset " +
              std::string(port.headset_plugged ? "plugged" : "unplugged"));
    }
  }
  if (output_state_changed) {
    forward_bt_state_if_changed(port, bt_backend, trace_flags, logger,
                                "headset route change");
  }
  if (headset_mic_changed && input_trace) {
    logger.log(
        vds::LogScope::InputControl, vds::LogLevel::Info,
        port.path + " headset mic " +
            std::string(port.headset_mic_plugged ? "plugged" : "unplugged"));
  }

  vds_frame_header header{};
  header.type = VDS_FRAME_USB_HID_IN;
  header.length = static_cast<std::uint32_t>(report->size());

  std::array<std::uint8_t, sizeof(header) + vds::kUsbInputReportSize> frame{};
  std::memcpy(frame.data(), &header, sizeof(header));
  std::memcpy(frame.data() + sizeof(header), report->data(), report->size());
  const std::span bytes(frame.data(), sizeof(header) + report->size());

  const auto write_start = Clock::now();
  const bool wrote = write_vds_frame(port, bytes, input_trace, logger);
  const auto write_duration = Clock::now() - write_start;
  if (input_trace) {
    const auto write_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(write_duration)
            .count());
    ++port.trace_state.input_write_count;
    port.trace_state.input_write_total_us += write_us;
    port.trace_state.input_write_max_us =
        std::max(port.trace_state.input_write_max_us, write_us);

    if (write_duration >= kInputTraceSlowWriteWarn) {
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Warn,
                 port.path + " slow input write count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " write_us=" + std::to_string(write_us) +
                     " written=" + (wrote ? "yes" : "no"));
    }
    if (port.trace_state.bt_input_count == 1 ||
        port.trace_state.bt_input_count % kInputTraceSummaryInterval == 0) {
      const std::uint64_t avg_write_us =
          port.trace_state.input_write_count == 0
              ? 0
              : port.trace_state.input_write_total_us /
                    port.trace_state.input_write_count;
      logger.log(vds::LogScope::InputControl, vds::LogLevel::Debug,
                 port.path + " input summary count=" +
                     std::to_string(port.trace_state.bt_input_count) +
                     " len=" + std::to_string(report->size()) +
                     " avg_write_us=" + std::to_string(avg_write_us) +
                     " max_write_us=" +
                     std::to_string(port.trace_state.input_write_max_us) +
                     " max_gap_us=" +
                     std::to_string(port.trace_state.input_gap_max_us));
    }
  }
  return true;
}

bool handle_bt_control(VirtualPort &port, vds::BtL2capBackend &bt_backend,
                       std::uint32_t trace_flags, vds::Logger &logger) {
  const auto report = bt_backend.read_feature_report();
  if (!report) {
    return false;
  }
  if (report->empty()) {
    return true;
  }

  const std::uint8_t report_id = (*report)[0];
  port.feature_cache[report_id] = *report;
  port.feature_cached[report_id] = true;

  const auto pending = std::find(port.pending_feature_reports.begin(),
                                 port.pending_feature_reports.end(), report_id);
  const bool usb_waiting = pending != port.pending_feature_reports.end();
  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  if (output_trace) {
    logger.log(vds::LogScope::Hid, vds::LogLevel::Debug,
               port.path + " bt feature cached report=" + hex_u8(report_id) +
                   " len=" + std::to_string(report->size()) +
                   " usb_waiting=" + (usb_waiting ? "yes" : "no"));
  }
  if (!usb_waiting) {
    return true;
  }

  port.pending_feature_reports.erase(pending);
  const auto frame = vds::frame_bytes(VDS_FRAME_USB_FEATURE_REPLY, *report);
  (void)write_vds_frame(port, frame, output_trace, logger);
  return true;
}

bool is_bluetooth_error(const std::exception &error) {
  return std::string_view(error.what()).rfind("Bluetooth L2CAP", 0) == 0 ||
         std::string_view(error.what())
                 .rfind("failed to connect Bluetooth", 0) == 0;
}

void initialize_bt_controller(vds::BtL2capBackend &bt_backend,
                              VirtualPort &port, vds::Logger &logger) {
  for (const std::uint8_t report_id : kInitialFeatureReportIds) {
    bt_backend.send_feature_get(report_id);

    while (true) {
      pollfd pfd{.fd = bt_backend.control_fd(), .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kInitialFeatureReportPollMs);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        throw std::runtime_error("timed out waiting for initial Bluetooth "
                                 "feature report " +
                                 hex_u8(report_id));
      }
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("Bluetooth control channel closed during "
                                 "initial feature report fetch");
      }
      if ((pfd.revents & POLLIN) == 0) {
        continue;
      }

      const auto report = bt_backend.read_feature_report();
      if (!report || report->empty()) {
        continue;
      }

      cache_feature_report(port, *report, false, logger);
      if ((*report)[0] == report_id) {
        break;
      }
    }
  }

  logger.log(vds::LogScope::Hid, vds::LogLevel::Info,
             port.path + " primed initial Bluetooth feature cache");
}

std::optional<std::size_t> find_port_index(std::span<const VirtualPort> ports,
                                           const std::string &path) {
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (ports[i].path == path) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<unsigned> present_port_indices(std::span<const VirtualPort> ports) {
  std::vector<unsigned> indices;
  indices.reserve(ports.size());
  for (const auto &port : ports) {
    if (const auto index = vds::port_index_from_path(port.path)) {
      indices.push_back(*index);
    }
  }
  return indices;
}

bool controller_uses_port(const ControllerRuntime &controller) {
  return controller.backend || controller.pending_control_fd ||
         controller.pending_interrupt_fd || controller.virtual_connected;
}

bool port_used_by_other_controller(
    std::span<const ControllerRuntime> controllers,
    const ControllerRuntime &self, const std::string &device) {
  for (const auto &controller : controllers) {
    if (&controller == &self) {
      continue;
    }
    if (controller.device == device && controller_uses_port(controller)) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t>
available_port_index(std::span<const VirtualPort> ports,
                     std::span<const ControllerRuntime> controllers,
                     const ControllerRuntime &controller) {
  const std::vector<unsigned> candidate_ports = present_port_indices(ports);
  std::vector<unsigned> occupied_ports;
  occupied_ports.reserve(controllers.size());
  for (const auto &other : controllers) {
    if (&other == &controller || !controller_uses_port(other)) {
      continue;
    }
    if (const auto port = vds::port_index_from_path(other.device)) {
      occupied_ports.push_back(*port);
    }
  }

  const auto selected_port = vds::select_controller_config_port(
      controller.config, candidate_ports, occupied_ports);
  if (!selected_port) {
    return std::nullopt;
  }

  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (vds::port_index_from_path(ports[i].path) == selected_port) {
      return i;
    }
  }
  return std::nullopt;
}

bool controller_has_present_allowed_port(std::span<const VirtualPort> ports,
                                         const vds::ControllerConfig &config) {
  return vds::controller_config_has_candidate_port(config,
                                                   present_port_indices(ports));
}

ControllerRuntime *
controller_for_port(std::vector<ControllerRuntime> &controllers,
                    const std::string &device) {
  for (auto &controller : controllers) {
    if (controller.device == device && controller_uses_port(controller)) {
      return &controller;
    }
  }
  return nullptr;
}

ControllerRuntime *
controller_for_address(std::vector<ControllerRuntime> &controllers,
                       const std::string &address) {
  for (auto &controller : controllers) {
    if (controller.config.address == address) {
      return &controller;
    }
  }
  return nullptr;
}

void drop_bt_backend(ControllerRuntime &controller, VirtualPort &port,
                     const std::string &reason, vds::Logger &logger) {
  if (!controller.backend && !controller.virtual_connected) {
    return;
  }

  port.pending_bt_state.reset();
  port.pending_bt_state_report.reset();
  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
             "backend disconnected address=" + controller.config.address +
                 " device=" + controller.device + " reason=" + reason);
  controller.backend.reset();
  controller.pending_control_fd.reset();
  controller.pending_interrupt_fd.reset();
  controller.virtual_connected = false;
  controller.device.clear();
  disconnect_virtual_port(port, logger);
}

void sync_virtual_ports(std::vector<VirtualPort> &ports,
                        std::span<const std::string> devices,
                        vds::Logger &logger) {
  std::vector<bool> moved(ports.size(), false);
  std::vector<VirtualPort> updated;
  updated.reserve(devices.size());

  for (const auto &device : devices) {
    auto it =
        std::find_if(ports.begin(), ports.end(), [&](const VirtualPort &port) {
          return port.path == device;
        });
    if (it != ports.end()) {
      const std::size_t old_index =
          static_cast<std::size_t>(std::distance(ports.begin(), it));
      moved[old_index] = true;
      updated.push_back(std::move(*it));
      continue;
    }

    vds::UniqueFd fd(open_required(device, O_RDWR | O_NONBLOCK | O_CLOEXEC));
    vds_driver_info driver_info{};
    std::string driver_message = "driver connected name=vds_hcd path=" + device;
    vds::LogLevel driver_log_level = vds::LogLevel::Info;
    if (::ioctl(fd.get(), VDS_IOC_GET_DRIVER_INFO, &driver_info) < 0) {
      driver_log_level = vds::LogLevel::Warn;
      driver_message =
          "driver version unavailable name=vds_hcd path=" + device +
          " detail=" + std::strerror(errno);
    } else if (driver_info.version != VDS_DRIVER_INFO_VERSION ||
               driver_info.size != sizeof(driver_info) ||
               driver_info.driver_version[0] == '\0' ||
               driver_info.driver_version[VDS_DRIVER_VERSION_MAX - 1] != '\0') {
      driver_log_level = vds::LogLevel::Warn;
      driver_message =
          "driver version unavailable name=vds_hcd path=" + device +
          " detail=invalid driver version reply";
    } else {
      driver_message = "driver connected name=vds_hcd version=" +
                       std::string(driver_info.driver_version) +
                       " path=" + device;
    }
    logger.log(vds::LogScope::Port, driver_log_level, driver_message);
    updated.push_back(VirtualPort{
        .path = device,
        .fd = std::move(fd),
        .extractor = vds::PcmAudioExtractor{},
        .waveout_extractor = vds::PcmAudioExtractor{},
        .haptics_builder = {},
        .output_state = {},
        .pending_audio_chunks = {},
        .last_sent_bt_state = std::nullopt,
        .pending_bt_state = std::nullopt,
        .pending_bt_state_report = std::nullopt,
        .next_haptics_send_time = {},
        .speaker_waveout_selected = true,
        .speaker_waveout_active = false,
        .speaker_waveout_phase = 0,
        .feature_cache = {},
        .feature_cached = {},
        .pending_feature_reports = {},
        .trace_state = {},
    });
  }

  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (!moved[i]) {
      logger.log(vds::LogScope::Port, vds::LogLevel::Info,
                 "closed " + ports[i].path);
    }
  }
  if (updated.empty()) {
    logger.log(vds::LogScope::Port, vds::LogLevel::Warn,
               "no /dev/vds* endpoints found");
  }
  ports = std::move(updated);
}

void preempt_default_bluetooth_owner(const std::string &address,
                                     vds::Logger &logger) {
  if (!vds::bluetooth_hid_device_present(address)) {
    return;
  }

  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
             "preempting default Bluetooth HID owner address=" + address);
  try {
    if (!vds::disconnect_bluez_device(address)) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
                 "BlueZ device not found for disconnect address=" + address);
    }
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
               "BlueZ disconnect failed address=" + address +
                   " error=" + error.what());
  }

  const auto deadline = Clock::now() + kBluetoothPreemptWait;
  while (Clock::now() < deadline) {
    if (!vds::bluetooth_hid_device_present(address)) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                 "default stack released address=" + address);
      return;
    }
    std::this_thread::sleep_for(kBluetoothPreemptPoll);
  }

  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
             "default Bluetooth HID owner still present address=" + address);
}

void complete_pending_controller(ControllerRuntime &controller,
                                 VirtualPort &port, vds::Logger &logger) {
  const std::uint32_t profile = resolve_config_profile(controller.config);
  preempt_default_bluetooth_owner(controller.config.address, logger);

  vds::BtL2capBackend candidate(controller.config.address,
                                std::move(controller.pending_control_fd),
                                std::move(controller.pending_interrupt_fd));

  reset_virtual_port(port);
  ioctl_noarg(port.fd.get(), VDS_IOC_DISCONNECT, "VDS_IOC_DISCONNECT");
  ioctl_set_profile(port.fd.get(), profile);
  initialize_bt_controller(candidate, port, logger);
  candidate.send_output_report(port.output_state.build_bt_init_report());
  port.last_sent_bt_state = port.output_state.state();
  logger.log(vds::LogScope::Hid, vds::LogLevel::Info,
             port.path + " sent initial Bluetooth state report");

  controller.backend = std::move(candidate);
  controller.detected_profile = profile;
  ioctl_noarg(port.fd.get(), VDS_IOC_CONNECT, "VDS_IOC_CONNECT");
  controller.virtual_connected = true;
  controller.last_error.clear();

  logger.log(
      vds::LogScope::Bluetooth, vds::LogLevel::Info,
      "raw L2CAP backend connected address=" + controller.config.address +
          " device=" + port.path + " profile=" + profile_name(profile));
}

void handle_bt_accept(std::vector<VirtualPort> &ports,
                      std::vector<ControllerRuntime> &controllers,
                      vds::BtL2capAcceptor &acceptor, bool control_channel,
                      vds::Logger &logger, bool &epoll_dirty) {
  while (true) {
    auto accepted = control_channel ? acceptor.accept_control()
                                    : acceptor.accept_interrupt();
    if (!accepted) {
      return;
    }

    ControllerRuntime *controller =
        controller_for_address(controllers, accepted->address);
    if (!controller) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
                 "rejected raw HID channel from unregistered address=" +
                     accepted->address);
      continue;
    }
    if (controller->backend) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Warn,
                 "rejected duplicate raw HID channel address=" +
                     accepted->address);
      continue;
    }

    std::optional<std::size_t> port_index;
    if (!controller->device.empty()) {
      port_index = find_port_index(ports, controller->device);
      if (port_index && port_used_by_other_controller(controllers, *controller,
                                                      controller->device)) {
        port_index.reset();
      }
    }
    if (!port_index) {
      if (ports.empty()) {
        controller->last_error = kLinuxVirtualPortProviderUnavailable;
        logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                   "rejected raw HID channel address=" + accepted->address +
                       " reason=" + kVirtualPortProviderUnavailableReason +
                       " detail=" + kLinuxVirtualPortProviderUnavailable);
        continue;
      }
      port_index = available_port_index(ports, controllers, *controller);
      if (!port_index) {
        controller->last_error = "no available virtual port";
        logger.log(vds::LogScope::Port, vds::LogLevel::Warn,
                   "rejected raw HID channel address=" + accepted->address +
                       " reason=no available virtual port");
        continue;
      }
      controller->device = ports[*port_index].path;
      logger.log(vds::LogScope::Config, vds::LogLevel::Info,
                 "controller assigned address=" + controller->config.address +
                     " device=" + controller->device + " profile=\"" +
                     vds::controller_profile_name(controller->config.profile) +
                     "\"");
    }

    if (control_channel) {
      controller->pending_control_fd = std::move(accepted->fd);
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                 "accepted raw HID control channel address=" +
                     accepted->address);
    } else {
      controller->pending_interrupt_fd = std::move(accepted->fd);
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                 "accepted raw HID interrupt channel address=" +
                     accepted->address);
    }

    if (!controller->pending_control_fd || !controller->pending_interrupt_fd) {
      continue;
    }

    port_index = find_port_index(ports, controller->device);
    if (!port_index) {
      controller->pending_control_fd.reset();
      controller->pending_interrupt_fd.reset();
      controller->device.clear();
      controller->last_error = "assigned virtual port disappeared";
      logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                 "controller inactive address=" + controller->config.address +
                     " reason=assigned port disappeared");
      continue;
    }

    try {
      complete_pending_controller(*controller, ports[*port_index], logger);
      epoll_dirty = true;
    } catch (const std::exception &error) {
      controller->backend.reset();
      controller->pending_control_fd.reset();
      controller->pending_interrupt_fd.reset();
      controller->virtual_connected = false;
      controller->device.clear();
      controller->last_error = error.what();
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Error,
                 "connect failed address=" + controller->config.address +
                     " device=" + ports[*port_index].path +
                     " error=" + error.what());
    }
  }
}

bool has_pending_bt_output(std::span<const VirtualPort> ports,
                           std::span<const ControllerRuntime> controllers) {
  for (const auto &controller : controllers) {
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (port_index && (ports[*port_index].pending_bt_state_report ||
                       !ports[*port_index].pending_audio_chunks.empty() ||
                       ports[*port_index].speaker_waveout_active)) {
      return true;
    }
  }
  return false;
}

int next_wakeup_timeout_ms(std::span<const VirtualPort> ports,
                           std::span<const ControllerRuntime> controllers) {
  const auto now = Clock::now();
  int timeout_ms = 60000;
  if (has_pending_bt_output(ports, controllers)) {
    timeout_ms = std::min(timeout_ms, kPendingOutputPollMs);
  }
  for (const auto &controller : controllers) {
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (!port_index || ports[*port_index].pending_audio_chunks.empty()) {
      continue;
    }

    const auto next_time = ports[*port_index].next_haptics_send_time;
    if (next_time == Clock::time_point{} || next_time <= now) {
      return 0;
    }
    const auto audio_wait =
        std::chrono::ceil<std::chrono::milliseconds>(next_time - now);
    timeout_ms = std::min(timeout_ms, static_cast<int>(audio_wait.count()));
  }
  return timeout_ms;
}

bool flush_pending_audio_chunk(VirtualPort &port,
                               vds::BtL2capBackend &bt_backend,
                               std::uint32_t trace_flags, vds::Logger &logger) {
  if (port.pending_audio_chunks.empty()) {
    return true;
  }

  const auto now = Clock::now();
  if (port.next_haptics_send_time != Clock::time_point{} &&
      now < port.next_haptics_send_time) {
    return true;
  }

  const bool output_trace = trace_enabled(trace_flags, kTraceOutput);
  std::size_t stale_dropped = 0;
  while (port.pending_audio_chunks.size() > kFreshPendingAudioChunks) {
    port.pending_audio_chunks.pop_front();
    ++stale_dropped;
    ++port.trace_state.dropped_audio_haptics_count;
    ++port.trace_state.queue_dropped_audio_haptics_count;
    ++port.trace_state.stale_audio_haptics_count;
  }
  if (stale_dropped > 0 &&
      (port.trace_state.stale_audio_haptics_count == stale_dropped ||
       port.trace_state.stale_audio_haptics_count % 1000 == 0)) {
    logger.log(
        vds::LogScope::Output, vds::LogLevel::Warn,
        port.path + " dropped stale BT 0x36 audio packets count=" +
            std::to_string(port.trace_state.stale_audio_haptics_count) +
            " pending=" + std::to_string(port.pending_audio_chunks.size()) +
            " dropped=" +
            std::to_string(port.trace_state.dropped_audio_haptics_count) +
            " blocked=" +
            std::to_string(port.trace_state.blocked_audio_haptics_count));
  }

  const auto &chunk = port.pending_audio_chunks.front();
  const auto packet = port.haptics_builder.build_packet(
      chunk.haptics, chunk.speaker, port.output_state.state(), true,
      port.headset_plugged);
  const auto send_start = Clock::now();
  if (!bt_backend.try_send_output_report(packet)) {
    ++port.trace_state.dropped_audio_haptics_count;
    ++port.trace_state.blocked_audio_haptics_count;
    port.pending_audio_chunks.pop_front();
    port.next_haptics_send_time = Clock::now() + kHapticsOutputBlockedRetry;
    if (port.trace_state.blocked_audio_haptics_count == 1 ||
        port.trace_state.blocked_audio_haptics_count % 1000 == 0) {
      logger.log(
          vds::LogScope::Output, vds::LogLevel::Warn,
          port.path +
              " dropped BT 0x36 audio packet after HID queue blocked "
              "count=" +
              std::to_string(port.trace_state.blocked_audio_haptics_count) +
              " pending=" + std::to_string(port.pending_audio_chunks.size()) +
              " dropped=" +
              std::to_string(port.trace_state.dropped_audio_haptics_count) +
              " stale=" +
              std::to_string(port.trace_state.stale_audio_haptics_count));
    }
    return false;
  }

  const auto send_duration = Clock::now() - send_start;
  port.pending_audio_chunks.pop_front();
  port.last_sent_bt_state = port.output_state.state();
  if (port.pending_bt_state &&
      *port.pending_bt_state == *port.last_sent_bt_state) {
    port.pending_bt_state.reset();
    port.pending_bt_state_report.reset();
  }
  port.next_haptics_send_time = now + kAudioOutputInterval;

  if (output_trace) {
    trace_output_latency(port.path, "audio_bt_send",
                         port.trace_state.output_audio_send_latency,
                         send_duration, kOutputTraceSlowWarn, logger);
  }
  return true;
}

void enqueue_speaker_waveout_chunk(VirtualPort &port, std::uint32_t trace_flags,
                                   vds::Logger &logger) {
  if (!port.speaker_waveout_active || !port.pending_audio_chunks.empty()) {
    return;
  }

  std::array<std::uint8_t,
             vds::kPcmWindowFrames * VDS_AUDIO_CHANNELS * sizeof(std::int16_t)>
      pcm{};
  for (std::size_t frame = 0; frame < vds::kPcmWindowFrames; ++frame) {
    const double angle = kSpeakerWaveoutTwoPi *
                         static_cast<double>(port.speaker_waveout_phase) /
                         static_cast<double>(kSpeakerWaveoutPeriodFrames);
    const auto sample = static_cast<std::int16_t>(
        std::sin(angle) * static_cast<double>(kSpeakerWaveoutAmplitude));
    port.speaker_waveout_phase =
        (port.speaker_waveout_phase + 1) % kSpeakerWaveoutPeriodFrames;

    for (std::size_t channel = 0; channel < vds::kSpeakerChannels; ++channel) {
      const std::size_t offset =
          (frame * VDS_AUDIO_CHANNELS + channel) * sizeof(std::int16_t);
      const auto value = static_cast<std::uint16_t>(sample);
      pcm[offset + 0] = static_cast<std::uint8_t>(value & 0xff);
      pcm[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }
  }

  const auto chunks = port.waveout_extractor.push_usb_audio(pcm);
  for (const auto &chunk : chunks) {
    if (port.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      break;
    }
    port.pending_audio_chunks.push_back(chunk);
  }
  if (trace_enabled(trace_flags, kTraceOutput) && !chunks.empty()) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Debug,
               port.path + " queued WebHID speaker waveout chunk pending=" +
                   std::to_string(port.pending_audio_chunks.size()));
  }
}

void flush_pending_outputs(std::vector<VirtualPort> &ports,
                           std::vector<ControllerRuntime> &controllers,
                           std::uint32_t trace_flags, vds::Logger &logger,
                           bool &epoll_dirty) {
  for (auto &controller : controllers) {
    if (!controller.backend) {
      continue;
    }
    const auto port_index = find_port_index(ports, controller.device);
    if (!port_index) {
      controller.backend.reset();
      controller.pending_control_fd.reset();
      controller.pending_interrupt_fd.reset();
      controller.virtual_connected = false;
      controller.device.clear();
      epoll_dirty = true;
      continue;
    }

    try {
      auto &port = ports[*port_index];
      if (!flush_pending_bt_state_report(port, *controller.backend, trace_flags,
                                         logger)) {
        continue;
      }
      enqueue_speaker_waveout_chunk(port, trace_flags, logger);
      (void)flush_pending_audio_chunk(port, *controller.backend, trace_flags,
                                      logger);
    } catch (const std::exception &error) {
      if (!is_bluetooth_error(error)) {
        throw;
      }
      drop_bt_backend(controller, ports[*port_index], error.what(), logger);
      epoll_dirty = true;
    }
  }
}

void reconcile_controller_configs(std::vector<VirtualPort> &ports,
                                  std::vector<ControllerRuntime> &controllers,
                                  const std::string &db_path,
                                  vds::Logger &logger) {
  const std::vector<std::string> devices = vds::discover_vds_devices();
  const bool virtual_port_provider_available = !devices.empty();
  for (auto &controller : controllers) {
    if (!controller_uses_port(controller) ||
        std::binary_search(devices.begin(), devices.end(), controller.device)) {
      continue;
    }
    if (const auto port_index = find_port_index(ports, controller.device)) {
      disconnect_virtual_port(ports[*port_index], logger);
    }
    controller.backend.reset();
    controller.pending_control_fd.reset();
    controller.pending_interrupt_fd.reset();
    controller.virtual_connected = false;
    controller.device.clear();
  }

  sync_virtual_ports(ports, devices, logger);
  if (!virtual_port_provider_available) {
    logger.log(vds::LogScope::Port, vds::LogLevel::Error,
               std::string(kVirtualPortProviderUnavailableReason) +
                   " detail=" + kLinuxVirtualPortProviderUnavailable);
  }
  const vds::ConfigDb db = vds::load_config_db(db_path);
  logger.log(vds::LogScope::Config, vds::LogLevel::Info,
             "loaded controller config count=" +
                 std::to_string(db.controllers.size()));

  std::vector<bool> preserved(controllers.size(), false);
  std::vector<std::string> active_devices;
  std::vector<ControllerRuntime> next_controllers;
  next_controllers.reserve(db.controllers.size());

  for (const auto &config : db.controllers) {
    if (!virtual_port_provider_available) {
      logger.log(vds::LogScope::Config, vds::LogLevel::Warn,
                 "controller binding unavailable address=" + config.address +
                     " reason=" + kVirtualPortProviderUnavailableReason);
    } else if (!controller_has_present_allowed_port(ports, config)) {
      logger.log(vds::LogScope::Config, vds::LogLevel::Warn,
                 "controller binding unavailable address=" + config.address +
                     " reason=no allowed virtual port present ports=[" +
                     vds::format_ports(config.ports) + "]");
    }
    if (virtual_port_provider_available) {
      preempt_default_bluetooth_owner(config.address, logger);
    }
    ControllerRuntime next{
        .config = config,
        .device = {},
        .detected_profile = std::nullopt,
        .backend = std::nullopt,
        .pending_control_fd = {},
        .pending_interrupt_fd = {},
        .virtual_connected = false,
        .last_error = {},
    };

    for (std::size_t old_index = 0; old_index < controllers.size();
         ++old_index) {
      auto &old = controllers[old_index];
      if (preserved[old_index] || old.config.address != config.address ||
          old.config.profile != config.profile) {
        continue;
      }
      const bool old_uses_port = controller_uses_port(old);
      const bool old_device_present =
          !old.device.empty() && find_port_index(ports, old.device);
      const bool old_device_allowed =
          old_device_present &&
          vds::controller_config_allows_path(config, old.device);
      const bool old_device_available =
          old_device_allowed &&
          std::find(active_devices.begin(), active_devices.end(), old.device) ==
              active_devices.end();

      next.detected_profile = old.detected_profile;
      next.last_error = std::move(old.last_error);
      if (old_uses_port && old_device_available) {
        next.device = std::move(old.device);
        next.backend = std::move(old.backend);
        next.pending_control_fd = std::move(old.pending_control_fd);
        next.pending_interrupt_fd = std::move(old.pending_interrupt_fd);
        next.virtual_connected = old.virtual_connected;
        active_devices.push_back(next.device);
      } else if (old_uses_port) {
        if (old.virtual_connected && old_device_present) {
          const auto old_port_index = find_port_index(ports, old.device);
          disconnect_virtual_port(ports[*old_port_index], logger);
        }
        if (old.backend) {
          logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
                     "raw backend removed address=" + old.config.address +
                         " device=" + old.device);
        }
        old.backend.reset();
        old.pending_control_fd.reset();
        old.pending_interrupt_fd.reset();
        old.virtual_connected = false;
        old.device.clear();
      }
      preserved[old_index] = true;
      break;
    }

    logger.log(vds::LogScope::Config, vds::LogLevel::Info,
               "controller registered address=" + config.address +
                   " profile=\"" +
                   vds::controller_profile_name(config.profile) + "\"");
    next_controllers.push_back(std::move(next));
  }

  for (std::size_t i = 0; i < controllers.size(); ++i) {
    if (preserved[i]) {
      continue;
    }
    if (controllers[i].virtual_connected) {
      if (const auto port_index =
              find_port_index(ports, controllers[i].device)) {
        disconnect_virtual_port(ports[*port_index], logger);
      }
    }
    if (controllers[i].backend) {
      logger.log(
          vds::LogScope::Bluetooth, vds::LogLevel::Info,
          "raw backend removed address=" + controllers[i].config.address +
              " device=" + controllers[i].device);
    }
  }

  controllers = std::move(next_controllers);
}

void handle_control_client(int control_fd, std::span<VirtualPort> ports,
                           std::span<ControllerRuntime> controllers,
                           const std::string &db_path,
                           std::uint32_t &trace_flags, bool &reload_requested,
                           vds::Logger &logger) {
  vds::UniqueFd client_fd(
      ::accept4(control_fd, nullptr, nullptr, SOCK_CLOEXEC));
  if (!client_fd) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return;
    }
    throw std::runtime_error("accept control client failed: " +
                             std::string(std::strerror(errno)));
  }

  std::array<char, 256> buffer{};
  ssize_t got = 0;
  do {
    got = ::read(client_fd.get(), buffer.data(), buffer.size() - 1);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    throw std::runtime_error("read control client failed: " +
                             std::string(std::strerror(errno)));
  }
  if (got <= 0) {
    return;
  }

  std::string reply;
  const std::string command =
      trim_command(std::string(buffer.data(), static_cast<std::size_t>(got)));
  std::vector<vds::VdsdControlControllerStatus> controller_statuses;
  controller_statuses.reserve(controllers.size());
  for (const auto &controller : controllers) {
    controller_statuses.push_back(vds::VdsdControlControllerStatus{
        .address = controller.config.address,
        .connected = controller.virtual_connected,
        .path = controller.virtual_connected ? controller.device : "",
    });
  }

  std::vector<vds::VdsdControlPortCandidate> port_candidates;
  port_candidates.reserve(ports.size());
  for (const auto &port : ports) {
    const auto port_index = vds::port_index_from_path(port.path);
    if (!port_index) {
      continue;
    }

    port_candidates.push_back(vds::VdsdControlPortCandidate{
        .port = *port_index,
        .path = port.path,
    });
  }

  std::vector<vds::VdsdControlPortBinding> port_bindings;
  port_bindings.reserve(controllers.size());
  for (const auto &controller : controllers) {
    if (!controller_uses_port(controller)) {
      continue;
    }
    const auto port = vds::port_index_from_path(controller.device);
    if (!port) {
      continue;
    }
    port_bindings.push_back(vds::VdsdControlPortBinding{
        .port = *port,
        .address = controller.config.address,
        .device_address = {},
    });
  }

  const std::vector<vds::VdsdControlPortStatus> port_statuses =
      vds::build_vdsd_control_port_statuses(port_candidates, port_bindings);

  reply = vds::handle_vdsd_control_command(
      command, db_path, controller_statuses, port_statuses,
      [] { return vds::list_bluez_controller_targets(); }, trace_flags,
      reload_requested, logger);

  try {
    vds::write_full(client_fd.get(), reply);
  } catch (const std::exception &error) {
    if (trace_flags != 0) {
      logger.log(vds::LogScope::Control, vds::LogLevel::Error,
                 std::string("reply failed: ") + error.what());
    }
  }
}

std::uint64_t encode_event(EventType type, std::size_t index) {
  return (static_cast<std::uint64_t>(type) << 32) |
         static_cast<std::uint32_t>(index);
}

EventSource decode_event(std::uint64_t data) {
  return EventSource{
      .type = static_cast<EventType>(data >> 32),
      .index = static_cast<std::uint32_t>(data),
  };
}

void add_epoll_fd(int epoll_fd, int fd, EventType type, std::size_t index) {
  epoll_event event{};
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  event.data.u64 = encode_event(type, index);
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    throw std::runtime_error("epoll add failed: " +
                             std::string(std::strerror(errno)));
  }
}

vds::UniqueFd rebuild_epoll(int control_fd, vds::BtL2capAcceptor &bt_acceptor,
                            const vds::VdsDeviceMonitor &vds_monitor,
                            std::span<VirtualPort> ports,
                            std::span<ControllerRuntime> controllers) {
  vds::UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    throw std::runtime_error("epoll_create1 failed: " +
                             std::string(std::strerror(errno)));
  }

  add_epoll_fd(epoll_fd.get(), control_fd, EventType::Control, 0);
  add_epoll_fd(epoll_fd.get(), vds_monitor.fd(), EventType::Udev, 0);
  add_epoll_fd(epoll_fd.get(), bt_acceptor.control_listener_fd(),
               EventType::BtAcceptControl, 0);
  add_epoll_fd(epoll_fd.get(), bt_acceptor.interrupt_listener_fd(),
               EventType::BtAcceptInterrupt, 0);
  for (std::size_t i = 0; i < ports.size(); ++i) {
    add_epoll_fd(epoll_fd.get(), ports[i].fd.get(), EventType::Port, i);
  }
  for (std::size_t i = 0; i < controllers.size(); ++i) {
    if (!controllers[i].backend) {
      continue;
    }
    add_epoll_fd(epoll_fd.get(), controllers[i].backend->control_fd(),
                 EventType::BtControl, i);
    add_epoll_fd(epoll_fd.get(), controllers[i].backend->interrupt_fd(),
                 EventType::BtInterrupt, i);
  }
  return epoll_fd;
}

void disconnect_all(std::vector<VirtualPort> &ports,
                    std::vector<ControllerRuntime> &controllers,
                    vds::Logger &logger) {
  for (auto &controller : controllers) {
    if (!controller.virtual_connected) {
      continue;
    }
    if (const auto port_index = find_port_index(ports, controller.device)) {
      disconnect_virtual_port(ports[*port_index], logger);
    }
    controller.virtual_connected = false;
  }
  controllers.clear();
}

} // namespace

int main(int argc, char **argv) {
  try {
    ::signal(SIGPIPE, SIG_IGN);
    install_signal_handler(SIGINT);
    install_signal_handler(SIGTERM);
    install_signal_handler(SIGHUP);

    const Options options = parse_platform_args(argc, argv);
    vds::Logger logger(options.log_path);
    logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
               "started socket=" + options.socket + " log=" + options.log_path +
                   " db=" + options.db_path);
    if (vds::discover_vds_devices().empty()) {
      logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                 std::string(kVirtualPortProviderUnavailableReason) +
                     " detail=" + kLinuxVirtualPortProviderUnavailable);
      throw std::runtime_error(kLinuxVirtualPortProviderUnavailable);
    }

    vds::UniqueFd control_fd(open_control_socket(options.socket));
    vds::BtL2capAcceptor bt_acceptor;
    vds::VdsDeviceMonitor vds_monitor;
    logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
               "listening for controller-initiated raw HID channels");
    SocketPathGuard control_socket_path(options.socket);
    std::vector<VirtualPort> ports;
    std::vector<ControllerRuntime> controllers;
    vds::UniqueFd epoll_fd;
    std::uint32_t trace_flags = 0;
    bool reload_requested = true;
    bool epoll_dirty = true;

    while (g_stop_requested == 0) {
      if (g_log_reopen_requested != 0) {
        g_log_reopen_requested = 0;
        try {
          logger.reopen();
          logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
                     "reopened log file");
        } catch (const std::exception &error) {
          logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                     std::string("log reopen failed: ") + error.what());
        }
      }

      if (reload_requested) {
        try {
          reconcile_controller_configs(ports, controllers, options.db_path,
                                       logger);
          epoll_dirty = true;
        } catch (const std::exception &error) {
          logger.log(vds::LogScope::Config, vds::LogLevel::Error,
                     std::string("reload failed: ") + error.what());
        }
        reload_requested = false;
      }

      if (epoll_dirty || !epoll_fd) {
        epoll_fd = rebuild_epoll(control_fd.get(), bt_acceptor, vds_monitor,
                                 ports, controllers);
        epoll_dirty = false;
      }

      std::array<epoll_event, 64> events{};
      const int timeout_ms = next_wakeup_timeout_ms(ports, controllers);
      const int ready =
          ::epoll_wait(epoll_fd.get(), events.data(),
                       static_cast<int>(events.size()), timeout_ms);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("epoll_wait failed: " +
                                 std::string(std::strerror(errno)));
      }
      if (ready == 0) {
        flush_pending_outputs(ports, controllers, trace_flags, logger,
                              epoll_dirty);
        continue;
      }

      for (int i = 0; i < ready; ++i) {
        const EventSource source = decode_event(events[i].data.u64);
        const std::uint32_t revents = events[i].events;

        if (source.type == EventType::Control) {
          if ((revents & EPOLLIN) != 0) {
            handle_control_client(control_fd.get(), ports, controllers,
                                  options.db_path, trace_flags,
                                  reload_requested, logger);
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            throw std::runtime_error("control socket epoll error");
          }
          continue;
        }

        if (source.type == EventType::BtAcceptControl ||
            source.type == EventType::BtAcceptInterrupt) {
          if ((revents & EPOLLIN) != 0) {
            handle_bt_accept(ports, controllers, bt_acceptor,
                             source.type == EventType::BtAcceptControl, logger,
                             epoll_dirty);
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            throw std::runtime_error("Bluetooth listener epoll error");
          }
          continue;
        }

        if (source.type == EventType::Udev) {
          if ((revents & EPOLLIN) != 0 && vds_monitor.drain()) {
            logger.log(vds::LogScope::Port, vds::LogLevel::Info,
                       "virtual endpoint udev event; reloading");
            reload_requested = true;
            epoll_dirty = true;
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            throw std::runtime_error("udev monitor epoll error");
          }
          continue;
        }

        if (source.type == EventType::Port) {
          if (source.index >= ports.size()) {
            continue;
          }
          auto &port = ports[source.index];
          ControllerRuntime *controller =
              controller_for_port(controllers, port.path);
          vds::BtL2capBackend *backend = controller && controller->backend
                                             ? &*controller->backend
                                             : nullptr;
          if ((revents & EPOLLIN) != 0) {
            for (int frame = 0; frame < kMaxPortFramesPerWake; ++frame) {
              try {
                if (!handle_vds_frame(port, backend, trace_flags, logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error) || !controller) {
                  throw;
                }
                drop_bt_backend(*controller, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                       port.path + " epoll error; reloading ports");
            reload_requested = true;
            epoll_dirty = true;
          }
          continue;
        }

        if (source.index >= controllers.size() ||
            !controllers[source.index].backend) {
          continue;
        }
        auto &controller = controllers[source.index];
        const auto port_index = find_port_index(ports, controller.device);
        if (!port_index) {
          controller.backend.reset();
          controller.virtual_connected = false;
          epoll_dirty = true;
          continue;
        }
        auto &port = ports[*port_index];

        if (source.type == EventType::BtControl) {
          if ((revents & EPOLLIN) != 0) {
            for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
              try {
                if (!handle_bt_control(port, *controller.backend, trace_flags,
                                       logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error)) {
                  throw;
                }
                drop_bt_backend(controller, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            drop_bt_backend(controller, port,
                            "Bluetooth L2CAP control epoll error", logger);
            epoll_dirty = true;
          }
          continue;
        }

        if (source.type == EventType::BtInterrupt) {
          if ((revents & EPOLLIN) != 0) {
            for (int packet = 0; packet < kMaxBtPacketsPerWake; ++packet) {
              try {
                if (!handle_bt_input(port, *controller.backend, trace_flags,
                                     logger)) {
                  break;
                }
              } catch (const std::exception &error) {
                if (!is_bluetooth_error(error)) {
                  throw;
                }
                drop_bt_backend(controller, port, error.what(), logger);
                epoll_dirty = true;
                break;
              }
            }
          }
          if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            drop_bt_backend(controller, port,
                            "Bluetooth L2CAP interrupt epoll error", logger);
            epoll_dirty = true;
          }
        }
      }

      flush_pending_outputs(ports, controllers, trace_flags, logger,
                            epoll_dirty);
    }

    logger.log(vds::LogScope::Daemon, vds::LogLevel::Info, "stopping");
    disconnect_all(ports, controllers, logger);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsd: " << error.what() << "\n";
    return 1;
  }
}
