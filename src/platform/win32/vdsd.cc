// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <mmsystem.h>

#include "jsonl.hh"
#include "uapi/vds.h"
#include "unique_handle.hh"
#include "vds/ds5_protocol.h"
#include "vds_bt.hh"
#include "vds_build_info.hh"
#include "vds_common.hh"
#include "vds_config.hh"
#include "vds_hidhide.hh"
#include "vds_io.hh"
#include "vds_log.hh"
#include "vds_profile.hh"
#include "vds_protocol.hh"
#include "vds_usbip.hh"
#include "vds_win32.hh"
#include "vdsd_common.hh"

namespace {

using Clock = std::chrono::steady_clock;
using vds::duration_us;
using vds::hex_bytes;
using vds::hex_u8;

constexpr const char *kDefaultControlPipe = R"(\\.\pipe\vdsd)";
constexpr const char *kVirtualPortProviderUnavailableReason =
    "virtual port provider unavailable";
constexpr const char *kWindowsVirtualPortProviderUnavailable =
    "usbip-win2 is not installed or usbip.exe is unavailable";
constexpr const char *kWindowsHidHideProviderUnavailable =
    "HidHide is not installed or HidHideCLI.exe is unavailable";
constexpr DWORD kDeviceShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr std::size_t kMaxPendingAudioChunks = 64;
constexpr std::size_t kAudioPrimeChunks = 24;
constexpr std::size_t kAudioFreshQueueChunks = 48;
constexpr std::size_t kAudioLowWatermarkChunks = 0;
constexpr std::size_t kAudioHighWatermarkChunks = 44;
constexpr std::size_t kWindowsPcmWindowFrames = vds::kPcmWindowFrames;
constexpr int kWindowsHapticsSampleMin = -128;
constexpr int kWindowsHapticsSampleMax = 127;
constexpr auto kAudioOutputBaseInterval = std::chrono::microseconds(
    (static_cast<std::int64_t>(kWindowsPcmWindowFrames) * 1000000 +
     VDS_AUDIO_SAMPLE_RATE / 2) /
    VDS_AUDIO_SAMPLE_RATE);
constexpr auto kAudioJitterBufferMaxDelay = std::chrono::milliseconds(300);
constexpr auto kAudioUnderflowKeepaliveDelay = std::chrono::milliseconds(25);
constexpr auto kAudioLowWatermarkPaceDelay = std::chrono::microseconds(0);
constexpr auto kAudioHighWatermarkCatchupDelay = std::chrono::microseconds(0);
constexpr auto kBridgeOpenRetryDelay = std::chrono::milliseconds(1000);
constexpr auto kBridgeWorkerFailureRetryDelay = std::chrono::milliseconds(1000);
constexpr auto kAudioFlushIdleSleep = std::chrono::milliseconds(2);
constexpr auto kAudioFlushMinimumSleep = std::chrono::microseconds(100);
constexpr auto kAudioWriteSlowWarn = std::chrono::milliseconds(5);
constexpr auto kAudioSendGapWarn = std::chrono::milliseconds(15);
constexpr auto kAudioSummaryInterval = std::chrono::seconds(1);
constexpr UINT kWindowsTimerResolutionMs = 1;
constexpr const char *kServiceName = "vdsd";
constexpr std::uint8_t kTestCommandReportId = 0x80;
constexpr std::uint8_t kTestCommandResultReportId = 0x81;
constexpr std::uint8_t kTestCommandCompleteStatus = 0x02;
constexpr std::uint8_t kTestCommandAudioDevice = 0x06;
constexpr std::uint8_t kTestCommandWaveoutPrepare = 0x04;
constexpr std::uint8_t kTestCommandWaveoutControl = 0x02;
constexpr std::uint8_t kTestCommandSpeakerParam = 0x08;
constexpr std::size_t kUsbInputMuteButtonOffset = 10;
constexpr std::size_t kUsbInputHeadsetOffset = 54;
constexpr std::uint8_t kUsbInputHeadphonesPluggedMask = 0x01;
constexpr std::uint8_t kUsbInputMicPluggedMask = 0x02;
constexpr std::uint8_t kUsbInputMuteButtonMask = 0x04;
constexpr std::uint32_t kSpeakerWaveoutFrequencyHz = 1000;
constexpr std::uint32_t kSpeakerWaveoutPeriodFrames =
    VDS_AUDIO_SAMPLE_RATE / kSpeakerWaveoutFrequencyHz;
constexpr double kSpeakerWaveoutTwoPi = 6.28318530717958647692;
constexpr std::int16_t kSpeakerWaveoutAmplitude = 12000;
std::atomic_bool g_stop_requested = false;
HANDLE g_stop_event = nullptr;
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
DWORD g_service_exit_code = ERROR_SUCCESS;

using vds::active_trace_name;
using vds::kTraceAll;
using vds::kTraceInputAudio;
using vds::kTraceInputControl;
using vds::kTraceOutput;
using vds::trace_enabled;
using vds::trace_scope_name;
using vds::trim_command;

using vds::bt_feature_to_usb_feature_reply;
using vds::bt_input_to_usb_input;
using vds::hidp_output_packet;
using vds::win::BluetoothTransport;
using vds::win::describe_bluetooth_lookup;
using vds::win::Frame;
using vds::win::HidBluetoothDevice;
using vds::win::list_bluetooth_controller_devices;
using vds::win::list_hid_bluetooth_devices;
using vds::win::make_hid_bluetooth_transport;
using vds::win::UniqueHandle;
using vds::win::win32_error_message;

struct Options {
  std::string db_path = vds::kDefaultDbPath;
  std::string log_path = vds::kDefaultLogPath;
  std::string pipe = kDefaultControlPipe;
};

class TimerResolutionGuard {
public:
  explicit TimerResolutionGuard(UINT period_ms) : period_ms_(period_ms) {
    active_ = timeBeginPeriod(period_ms_) == TIMERR_NOERROR;
  }

  ~TimerResolutionGuard() {
    if (active_) {
      timeEndPeriod(period_ms_);
    }
  }

  TimerResolutionGuard(const TimerResolutionGuard &) = delete;
  TimerResolutionGuard &operator=(const TimerResolutionGuard &) = delete;

  bool active() const { return active_; }

private:
  UINT period_ms_ = 0;
  bool active_ = false;
};

class HighResolutionSleeper {
public:
  HighResolutionSleeper() {
    timer_.reset(CreateWaitableTimerExA(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE));
    if (!timer_) {
      timer_.reset(CreateWaitableTimerExA(nullptr, nullptr, 0,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE));
    }
  }

  HighResolutionSleeper(const HighResolutionSleeper &) = delete;
  HighResolutionSleeper &operator=(const HighResolutionSleeper &) = delete;

  bool active() const { return static_cast<bool>(timer_); }

  void sleep_for(Clock::duration duration) {
    if (duration <= Clock::duration::zero()) {
      return;
    }
    if (!timer_) {
      std::this_thread::sleep_for(duration);
      return;
    }

    using HundredNanoseconds =
        std::chrono::duration<long long, std::ratio<1, 10000000>>;
    const auto ticks = std::max<long long>(
        1, std::chrono::duration_cast<HundredNanoseconds>(duration).count());
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -ticks;
    if (!SetWaitableTimerEx(timer_.get(), &due_time, 0, nullptr, nullptr,
                            nullptr, 0)) {
      std::this_thread::sleep_for(duration);
      return;
    }

    HANDLE wait_handles[2] = {timer_.get(), g_stop_event};
    const DWORD wait_count = g_stop_event == nullptr ? 1 : 2;
    const DWORD wait_result =
        WaitForMultipleObjects(wait_count, wait_handles, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1) {
      CancelWaitableTimer(timer_.get());
    }
  }

private:
  UniqueHandle timer_;
};

class AudioThreadPriorityGuard {
public:
  AudioThreadPriorityGuard() {
    previous_priority_ = GetThreadPriority(GetCurrentThread());
    priority_set_ =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    avrt_ = LoadLibraryA("avrt.dll");
    if (!avrt_) {
      return;
    }

    av_revert_ = reinterpret_cast<AvRevertMmThreadCharacteristicsProc>(
        GetProcAddress(avrt_, "AvRevertMmThreadCharacteristics"));
    const auto av_set = reinterpret_cast<AvSetMmThreadCharacteristicsAProc>(
        GetProcAddress(avrt_, "AvSetMmThreadCharacteristicsA"));
    if (av_set == nullptr || av_revert_ == nullptr) {
      return;
    }

    DWORD task_index = 0;
    mmcss_handle_ = av_set("Pro Audio", &task_index);
    if (mmcss_handle_ != nullptr) {
      mmcss_active_ = true;
    }
  }

  ~AudioThreadPriorityGuard() {
    if (mmcss_active_ && av_revert_ != nullptr) {
      av_revert_(mmcss_handle_);
    }
    if (priority_set_ && previous_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
      SetThreadPriority(GetCurrentThread(), previous_priority_);
    }
    if (avrt_ != nullptr) {
      FreeLibrary(avrt_);
    }
  }

  AudioThreadPriorityGuard(const AudioThreadPriorityGuard &) = delete;
  AudioThreadPriorityGuard &
  operator=(const AudioThreadPriorityGuard &) = delete;

  bool boosted() const { return priority_set_ || mmcss_active_; }

  std::string summary() const {
    return "audio flush thread priority priority_set=" +
           std::string(priority_set_ ? "yes" : "no") +
           " mmcss=" + (mmcss_active_ ? "yes" : "no");
  }

private:
  using AvSetMmThreadCharacteristicsAProc = HANDLE(WINAPI *)(LPCSTR, LPDWORD);
  using AvRevertMmThreadCharacteristicsProc = BOOL(WINAPI *)(HANDLE);

  HMODULE avrt_ = nullptr;
  HANDLE mmcss_handle_ = nullptr;
  AvRevertMmThreadCharacteristicsProc av_revert_ = nullptr;
  int previous_priority_ = THREAD_PRIORITY_ERROR_RETURN;
  bool priority_set_ = false;
  bool mmcss_active_ = false;
};

vds::AudioChunk make_silent_audio_chunk() {
  std::array<std::uint8_t, kWindowsPcmWindowFrames * VDS_AUDIO_CHANNELS *
                               sizeof(std::int16_t)>
      pcm{};
  vds::PcmAudioExtractor extractor(kWindowsPcmWindowFrames);
  std::vector<vds::AudioChunk> chunks = extractor.push_usb_audio(pcm);
  if (chunks.size() != 1) {
    throw std::runtime_error("failed to build silent audio keepalive chunk");
  }
  chunks.front().has_signal = false;
  chunks.front().has_haptics_signal = false;
  return chunks.front();
}

Clock::duration
audio_output_interval_for_pending(std::size_t pending_chunks,
                                  bool &low_watermark_pace,
                                  bool &high_watermark_catchup) {
  low_watermark_pace = false;
  high_watermark_catchup = false;
  if (pending_chunks <= kAudioLowWatermarkChunks) {
    low_watermark_pace = true;
    return kAudioOutputBaseInterval + kAudioLowWatermarkPaceDelay;
  }
  if (pending_chunks >= kAudioHighWatermarkChunks) {
    high_watermark_catchup = true;
    return kAudioOutputBaseInterval - kAudioHighWatermarkCatchupDelay;
  }
  return kAudioOutputBaseInterval;
}

struct BridgeState {
  std::mutex mutex;
  vds::DsOutputState output_state;
  vds::PcmAudioExtractor extractor{kWindowsPcmWindowFrames};
  vds::PcmAudioExtractor waveout_extractor{kWindowsPcmWindowFrames};
  vds::MicAudioDecoder mic_decoder;
  vds::HapticsPacketBuilder haptics_builder;
  vds::AudioChunk silent_audio_chunk = make_silent_audio_chunk();
  std::deque<vds::AudioChunk> pending_audio_chunks;
  std::optional<vds::AudioChunk> last_audio_chunk;
  std::array<std::vector<std::uint8_t>, 256> feature_cache;
  std::array<bool, 256> feature_cached{};
  Clock::time_point next_haptics_send_time{};
  Clock::time_point last_audio_send_time{};
  Clock::time_point last_audio_usb_frame_time{};
  Clock::time_point audio_jitter_buffer_start{};
  Clock::time_point audio_queue_empty_since{};
  Clock::time_point audio_queue_drained_time{};
  Clock::time_point audio_last_flush_time{};
  Clock::time_point audio_last_summary_time{};
  vds::DsState last_sent_state{};
  vds::UsbInputReport last_logged_input_report{};
  std::uint64_t usb_input_forward_count = 0;
  std::uint64_t usb_input_change_log_count = 0;
  std::uint64_t mic_input_forward_count = 0;
  std::uint64_t mic_input_decode_fail_count = 0;
  std::uint64_t audio_usb_frame_count = 0;
  std::uint64_t audio_queued_count = 0;
  std::uint64_t audio_sent_count = 0;
  std::uint64_t audio_concealed_count = 0;
  std::uint64_t audio_keepalive_sent_count = 0;
  std::uint64_t audio_dropped_count = 0;
  std::uint64_t audio_stale_dropped_count = 0;
  std::uint64_t audio_blocked_count = 0;
  std::uint64_t audio_keepalive_blocked_count = 0;
  std::uint64_t audio_usb_gap_warn_count = 0;
  std::uint64_t audio_send_gap_warn_count = 0;
  std::uint64_t audio_underflow_count = 0;
  std::uint64_t audio_underflow_refill_count = 0;
  std::uint64_t audio_flush_gap_warn_count = 0;
  std::uint64_t audio_low_watermark_pace_count = 0;
  std::uint64_t audio_last_summary_usb_frame_count = 0;
  std::uint64_t audio_last_summary_sent_count = 0;
  std::uint64_t audio_last_summary_concealed_count = 0;
  std::uint64_t audio_last_summary_keepalive_sent_count = 0;
  std::uint64_t audio_last_summary_dropped_count = 0;
  std::uint64_t audio_last_summary_blocked_count = 0;
  std::uint64_t audio_last_summary_underflow_count = 0;
  std::uint64_t audio_last_summary_usb_gap_warn_count = 0;
  std::uint64_t audio_last_summary_send_gap_warn_count = 0;
  std::uint64_t audio_last_summary_flush_gap_warn_count = 0;
  std::uint64_t audio_last_summary_low_watermark_pace_count = 0;
  Clock::duration audio_usb_max_gap{};
  Clock::duration audio_send_max_gap{};
  Clock::duration audio_underflow_max_gap{};
  Clock::duration audio_flush_max_gap{};
  bool have_last_sent_state = false;
  bool have_last_logged_input_report = false;
  bool have_last_audio_send_time = false;
  bool audio_jitter_buffering = true;
  bool audio_haptics_nonzero_seen = false;
  bool audio_haptics_sent_seen = false;
  bool audio_out_stream_active = false;
  bool audio_in_stream_active = false;
  bool mic_muted = false;
  bool mute_button_down = false;
  bool audio_pcm_stream_active = false;
  bool audio_stream_had_pcm = false;
  bool headset_plugged = false;
  bool headset_mic_plugged = false;
  bool speaker_waveout_selected = true;
  bool speaker_waveout_active = false;
  std::uint32_t speaker_waveout_phase = 0;
  bool audio_queue_empty_reported = false;
  bool audio_underflow_reported = false;
};

std::array<int, VDS_AUDIO_CHANNELS>
pcm_channel_peaks(std::span<const std::uint8_t> payload) {
  std::array<int, VDS_AUDIO_CHANNELS> peaks{};
  constexpr std::size_t frame_size = VDS_AUDIO_CHANNELS * sizeof(std::int16_t);
  const std::size_t frames = payload.size() / frame_size;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const std::uint8_t *base = payload.data() + frame * frame_size;
    for (std::size_t channel = 0; channel < VDS_AUDIO_CHANNELS; ++channel) {
      const auto low =
          static_cast<std::uint16_t>(base[channel * sizeof(std::int16_t)]);
      const auto high =
          static_cast<std::uint16_t>(base[channel * sizeof(std::int16_t) + 1])
          << 8;
      const auto sample = static_cast<std::int16_t>(low | high);
      const int value =
          sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
      peaks[channel] = std::max(peaks[channel], value);
    }
  }
  return peaks;
}

Options parse_platform_args(int argc, char **argv) {
  Options options;
  vds::VdsdCommonOptions common = vds::default_vdsd_common_options();
  const std::string platform_options =
      std::string("[--pipe ") + kDefaultControlPipe + "]";
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
    } else if (arg == "--pipe") {
      options.pipe = next_value(i, arg);
    } else {
      throw std::runtime_error("unknown option: " + std::string(arg));
    }
  }
  options.db_path = common.db_path;
  options.log_path = common.log_path;
  return options;
}

vds::ConfigDb load_config_db_for_attempt(const Options &options,
                                         vds::Logger &logger) {
  (void)logger;
  return vds::load_config_db(options.db_path);
}

std::optional<HidBluetoothDevice>
find_connected_bluetooth_device(const std::string &address,
                                std::span<const HidBluetoothDevice> devices,
                                std::string *failure_reason = nullptr) {
  for (const auto &device : devices) {
    if (vds::normalize_bluetooth_address(device.address) != address) {
      continue;
    }
    if (device.bluetooth_connected) {
      return device;
    }
    if (failure_reason != nullptr) {
      *failure_reason =
          "Windows Bluetooth reports address=" + address + " connected=no";
    }
    return std::nullopt;
  }

  if (failure_reason != nullptr) {
    *failure_reason = "Windows HID did not report a connected Bluetooth "
                      "device for address=" +
                      address + "; " + describe_bluetooth_lookup(address);
  }
  return std::nullopt;
}

std::optional<HidBluetoothDevice>
find_connected_bluetooth_device(const std::string &address,
                                std::string *failure_reason = nullptr) {
  try {
    return find_connected_bluetooth_device(
        address, list_hid_bluetooth_devices(), failure_reason);
  } catch (const std::exception &error) {
    if (failure_reason != nullptr) {
      *failure_reason =
          std::string("Windows HID query failed: ") + error.what();
    }
    return std::nullopt;
  }
}

std::vector<vds::ControllerTarget> list_windows_controller_targets() {
  std::vector<vds::ControllerTarget> targets;
  for (const auto &device : list_bluetooth_controller_devices()) {
    if (!device.profile_valid || device.address.empty()) {
      continue;
    }
    targets.push_back(vds::ControllerTarget{
        .address = vds::normalize_bluetooth_address(device.address),
        .name = device.name,
        .profile = device.profile == VDS_PROFILE_DSE
                       ? vds::ControllerProfile::Dse
                       : vds::ControllerProfile::Ds5,
        .online = device.bluetooth_connected,
    });
  }
  return targets;
}

bool virtual_port_is_enabled(unsigned port) {
  return port < vds::kMaxPortCount;
}

std::vector<unsigned> enabled_virtual_ports() {
  std::vector<unsigned> ports;
  ports.reserve(vds::kMaxPortCount);
  for (unsigned port = 0; port < vds::kMaxPortCount; ++port) {
    if (virtual_port_is_enabled(port)) {
      ports.push_back(port);
    }
  }
  return ports;
}

bool virtual_port_provider_available() {
  return vds::win::usbip::client_available();
}

bool virtual_port_is_available(unsigned port) {
  return virtual_port_is_enabled(port);
}

std::vector<unsigned> available_virtual_ports() {
  std::vector<unsigned> ports;
  ports.reserve(vds::kMaxPortCount);
  for (unsigned port = 0; port < vds::kMaxPortCount; ++port) {
    if (virtual_port_is_available(port)) {
      ports.push_back(port);
    }
  }
  return ports;
}

bool controller_has_enabled_virtual_port(
    const vds::ControllerConfig &config,
    std::span<const unsigned> enabled_ports) {
  return vds::controller_config_has_candidate_port(config, enabled_ports);
}

BOOL WINAPI console_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
      type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
    g_stop_requested = true;
    if (g_stop_event != nullptr) {
      SetEvent(g_stop_event);
    }
    return TRUE;
  }
  return FALSE;
}

void update_service_status(DWORD state, DWORD win32_exit_code = ERROR_SUCCESS,
                           DWORD wait_hint = 0) {
  if (g_service_status_handle == nullptr) {
    return;
  }

  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = state;
  g_service_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                               : 0;
  g_service_status.dwWin32ExitCode = win32_exit_code;
  g_service_status.dwServiceSpecificExitCode =
      win32_exit_code == ERROR_SERVICE_SPECIFIC_ERROR ? g_service_exit_code : 0;
  g_service_status.dwCheckPoint =
      state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 1 : 0;
  g_service_status.dwWaitHint = wait_hint;
  SetServiceStatus(g_service_status_handle, &g_service_status);
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    g_stop_requested = true;
    if (g_stop_event != nullptr) {
      SetEvent(g_stop_event);
    }
    update_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 5000);
    return ERROR_SUCCESS;
  }
  if (control == SERVICE_CONTROL_INTERROGATE) {
    SetServiceStatus(g_service_status_handle, &g_service_status);
    return ERROR_SUCCESS;
  }
  return ERROR_CALL_NOT_IMPLEMENTED;
}

UniqueHandle open_device(const std::string &path) {
  UniqueHandle handle(CreateFileA(
      path.c_str(), GENERIC_READ | GENERIC_WRITE, kDeviceShareMode, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
  if (!handle) {
    throw std::runtime_error("failed to open " + path + ": " +
                             win32_error_message(GetLastError()));
  }
  return handle;
}

vds::win::HandleIoCancellation handle_io_cancellation() {
  return {
      .stop_event = g_stop_event,
      .stop_requested = &g_stop_requested,
      .operation_aborted_is_stop = false,
  };
}

std::uint32_t resolve_virtual_port_profile(const vds::ControllerConfig &config,
                                           vds::Logger &logger) {
  if (config.profile != vds::ControllerProfile::Unspecified) {
    return vds::usb_profile_from_controller_profile(config.profile);
  }

  std::string discovery_error;
  const auto device =
      find_connected_bluetooth_device(config.address, &discovery_error);
  if (!device) {
    if (discovery_error.empty()) {
      discovery_error = "no matching HID device";
    }
    throw std::runtime_error("failed to detect Bluetooth controller profile: " +
                             discovery_error);
  }
  if (!device->profile_valid) {
    throw std::runtime_error("failed to detect Bluetooth controller profile: "
                             "Bluetooth transport did not report DS5/DSE "
                             "profile");
  }
  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "detected Bluetooth profile=" +
                 vds::usb_profile_name(device->profile));
  return device->profile;
}

bool input_controls_changed(const vds::UsbInputReport &left,
                            const vds::UsbInputReport &right) {
  for (std::size_t index = 1; index <= 10; ++index) {
    if (left[index] != right[index]) {
      return true;
    }
  }
  return false;
}

std::string input_report_summary(const vds::UsbInputReport &report) {
  return "lx=" + std::to_string(report[1]) +
         " ly=" + std::to_string(report[2]) +
         " rx=" + std::to_string(report[3]) +
         " ry=" + std::to_string(report[4]) +
         " l2=" + std::to_string(report[5]) +
         " r2=" + std::to_string(report[6]) +
         " seq=" + std::to_string(report[7]) + " b0=" + hex_u8(report[8]) +
         " b1=" + hex_u8(report[9]) + " b2=" + hex_u8(report[10]);
}

void cache_feature_report(BridgeState &state,
                          std::span<const std::uint8_t> report) {
  if (report.empty()) {
    return;
  }

  std::lock_guard guard(state.mutex);
  state.feature_cache[report[0]].assign(report.begin(), report.end());
  state.feature_cached[report[0]] = true;
}

std::optional<std::vector<std::uint8_t>>
cached_feature_report(BridgeState &state, std::uint8_t report_id) {
  std::lock_guard guard(state.mutex);
  if (!state.feature_cached[report_id]) {
    return std::nullopt;
  }
  return state.feature_cache[report_id];
}

bool try_send_bt_output_report(BluetoothTransport &bluetooth,
                               std::span<const std::uint8_t> report,
                               vds::Logger &logger, std::mutex &bluetooth_mutex,
                               std::string_view label) {
  const auto start = Clock::now();
  bool sent = false;
  try {
    std::lock_guard guard(bluetooth_mutex);
    const std::vector<std::uint8_t> packet = hidp_output_packet(report);
    if (label == "audio") {
      sent = bluetooth.try_write_interrupt_packet(packet);
    } else {
      bluetooth.write_interrupt_packet(packet);
      sent = true;
    }
  } catch (const std::exception &error) {
    logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
               std::string(label) + " output report ignored: " + error.what());
    return false;
  }

  const auto duration = Clock::now() - start;
  if (duration >= kAudioWriteSlowWarn) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               std::string(label) + " output write slow us=" +
                   std::to_string(duration_us(duration)));
  }
  return sent;
}

std::optional<vds::BtStateReport>
build_bt_state_if_changed(BridgeState &state) {
  std::lock_guard guard(state.mutex);
  const vds::DsState current = state.output_state.state();
  if (state.have_last_sent_state && state.last_sent_state == current) {
    return std::nullopt;
  }
  const auto report = state.output_state.build_bt_state_report();
  state.last_sent_state = current;
  state.have_last_sent_state = true;
  return report;
}

void send_bt_state_if_changed(BluetoothTransport &bluetooth, BridgeState &state,
                              vds::Logger &logger,
                              std::mutex &bluetooth_mutex) {
  const auto report = build_bt_state_if_changed(state);
  if (report) {
    (void)try_send_bt_output_report(bluetooth, *report, logger, bluetooth_mutex,
                                    "state");
  }
}

void request_bridge_restart(HANDLE virtual_device,
                            BluetoothTransport *bluetooth,
                            std::atomic_bool &bridge_restart_requested) {
  if (bridge_restart_requested.exchange(true)) {
    return;
  }
  if (virtual_device != INVALID_HANDLE_VALUE && virtual_device != nullptr) {
    CancelIoEx(virtual_device, nullptr);
  }
  if (bluetooth != nullptr) {
    bluetooth->cancel();
  }
}

void handle_virtual_frame(HANDLE virtual_device, BluetoothTransport &bluetooth,
                          BridgeState &state, const Frame &frame,
                          vds::Logger &logger, std::mutex &bluetooth_mutex,
                          std::atomic_bool &bridge_restart_requested) {
  if (frame.header.type == VDS_FRAME_USB_INTERFACE) {
    if (frame.payload.size() != sizeof(vds_usb_interface_event)) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 "malformed USB interface frame length=" +
                     std::to_string(frame.payload.size()));
      return;
    }

    vds_usb_interface_event event{};
    std::memcpy(&event, frame.payload.data(), sizeof(event));
    if (event.interface_type == VDS_USB_INTERFACE_AUDIO_OUT) {
      {
        std::lock_guard guard(state.mutex);
        const bool active = event.altsetting != 0;
        if (active) {
          state.speaker_waveout_active = false;
        }
        state.output_state.set_audio_out_stream_active(
            active || state.speaker_waveout_active, state.headset_plugged);
        state.audio_out_stream_active = active;
        state.audio_pcm_stream_active = false;
        state.audio_stream_had_pcm = false;
        state.audio_jitter_buffering = active;
        state.audio_jitter_buffer_start = {};
        state.audio_queue_empty_since = {};
        state.audio_queue_drained_time = {};
        state.have_last_audio_send_time = false;
        state.last_audio_usb_frame_time = {};
        state.audio_last_flush_time = {};
        state.audio_last_summary_time = {};
        state.last_audio_chunk.reset();
        state.audio_usb_max_gap = {};
        state.audio_send_max_gap = {};
        state.audio_underflow_max_gap = {};
        state.audio_flush_max_gap = {};
        state.audio_usb_gap_warn_count = 0;
        state.audio_send_gap_warn_count = 0;
        state.audio_flush_gap_warn_count = 0;
        state.audio_low_watermark_pace_count = 0;
        state.audio_last_summary_usb_frame_count = 0;
        state.audio_last_summary_sent_count = 0;
        state.audio_last_summary_concealed_count = 0;
        state.audio_last_summary_keepalive_sent_count = 0;
        state.audio_last_summary_dropped_count = 0;
        state.audio_last_summary_blocked_count = 0;
        state.audio_last_summary_underflow_count = 0;
        state.audio_last_summary_usb_gap_warn_count = 0;
        state.audio_last_summary_send_gap_warn_count = 0;
        state.audio_last_summary_flush_gap_warn_count = 0;
        state.audio_last_summary_low_watermark_pace_count = 0;
        state.audio_haptics_sent_seen = false;
        state.audio_queue_empty_reported = false;
        state.audio_underflow_reported = false;
        state.pending_audio_chunks.clear();
        state.next_haptics_send_time = {};
        if (active) {
          state.extractor = vds::PcmAudioExtractor{kWindowsPcmWindowFrames};
        }
        if (!active) {
          state.audio_queue_empty_since = {};
          state.audio_queue_drained_time = {};
        }
      }
      logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                 "audio out interface altsetting=" +
                     std::to_string(event.altsetting));
      if (event.altsetting != 0) {
        (void)bluetooth.take_output_diagnostics(true);
      }
      send_bt_state_if_changed(bluetooth, state, logger, bluetooth_mutex);
    } else if (event.interface_type == VDS_USB_INTERFACE_AUDIO_IN) {
      const bool active = event.altsetting != 0;
      bool active_audio_in_release = false;
      vds::BtInitReport report{};
      {
        std::lock_guard guard(state.mutex);
        if (active) {
          state.audio_in_stream_active = true;
        } else {
          active_audio_in_release = state.audio_in_stream_active;
          state.audio_in_stream_active = false;
        }
        state.mic_decoder = vds::MicAudioDecoder{};
        state.output_state.set_headset_mic_plugged(state.headset_mic_plugged);
        report = state.output_state.build_bt_mic_report(active);
      }
      vds::BtStateReport state_report{};
      {
        std::lock_guard guard(state.mutex);
        state_report = state.output_state.build_bt_mic_state_report(
            active, state.mic_muted);
      }
      try_send_bt_output_report(bluetooth, state_report, logger,
                                bluetooth_mutex, "mic state");
      const bool sent = try_send_bt_output_report(bluetooth, report, logger,
                                                  bluetooth_mutex, "mic");
      logger.log(
          vds::LogScope::InputAudio, vds::LogLevel::Info,
          "mic " + std::string(active ? "open" : "close") +
              " sent=" + std::string(sent ? "yes" : "no") +
              " interface_altsetting=" + std::to_string(event.altsetting));
      if (active_audio_in_release) {
        logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                   "audio in endpoint released");
      }
    }
    return;
  }

  if (frame.header.type == VDS_FRAME_USB_HID_OUT) {
    bool applied = false;
    {
      std::lock_guard guard(state.mutex);
      applied = state.output_state.apply_usb_output_report(frame.payload);
      if (applied &&
          (state.audio_out_stream_active || state.speaker_waveout_active)) {
        state.output_state.set_audio_out_stream_active(true,
                                                       state.headset_plugged);
      }
    }
    std::string message =
        "hid out length=" + std::to_string(frame.payload.size()) +
        " applied=" + (applied ? "yes" : "no");
    if (!frame.payload.empty()) {
      message += " report_id=" + std::to_string(frame.payload[0]);
    }
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info, message);
    if (applied) {
      send_bt_state_if_changed(bluetooth, state, logger, bluetooth_mutex);
    }
    return;
  }

  if (frame.header.type == VDS_FRAME_USB_FEATURE_GET) {
    if (frame.payload.empty()) {
      return;
    }
    const std::uint8_t report_id = frame.payload[0];
    if (const auto cached = cached_feature_report(state, report_id)) {
      vds::win::write_handle_frame(virtual_device, VDS_FRAME_USB_FEATURE_REPLY,
                                   *cached, "virtual",
                                   handle_io_cancellation());
      logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                 "feature get report_id=" + std::to_string(report_id) +
                     " cache=hit bluetooth_forward=no");
      return;
    }

    try {
      std::optional<std::vector<std::uint8_t>> report;
      {
        std::lock_guard guard(bluetooth_mutex);
        report = bluetooth.read_feature_report(report_id);
      }
      if (report) {
        cache_feature_report(state, *report);
        vds::win::write_handle_frame(virtual_device,
                                     VDS_FRAME_USB_FEATURE_REPLY, *report,
                                     "virtual", handle_io_cancellation());
      }
      logger.log(
          vds::LogScope::Usb, vds::LogLevel::Info,
          "feature get report_id=" + std::to_string(report_id) +
              " cache=miss bluetooth_forward=" + (report ? "yes" : "no"));
    } catch (const std::exception &error) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 "feature get report_id=" + std::to_string(report_id) +
                     " failed: " + error.what());
    }
    return;
  }

  if (frame.header.type == VDS_FRAME_USB_FEATURE_SET) {
    std::string message =
        "feature set length=" + std::to_string(frame.payload.size());
    if (!frame.payload.empty()) {
      message += " report_id=" + std::to_string(frame.payload[0]);
    }
    logger.log(vds::LogScope::Usb, vds::LogLevel::Info, message);
    if (frame.payload.empty() || frame.payload[0] == 0) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 "ignoring malformed feature set report");
      return;
    }
    const std::uint8_t report_id = frame.payload[0];
    bool synthetic_test_command = false;

    if (frame.payload.size() >= 3 && report_id == kTestCommandReportId) {
      std::size_t command_offset = 1;
      if (frame.payload.size() > command_offset &&
          frame.payload[command_offset] == kTestCommandReportId) {
        ++command_offset;
      }
      if (frame.payload.size() > command_offset + 1) {
        const std::uint8_t command_device = frame.payload[command_offset];
        const std::uint8_t command_action = frame.payload[command_offset + 1];
        const std::size_t command_data_offset = command_offset + 2;
        synthetic_test_command = true;

        std::array<std::uint8_t, VDS_USB_INPUT_REPORT_SIZE> test_result{};
        test_result[0] = kTestCommandResultReportId;
        test_result[1] = command_device;
        test_result[2] = command_action;
        test_result[3] = kTestCommandCompleteStatus;
        cache_feature_report(state, test_result);
        logger.log(
            vds::LogScope::Usb, vds::LogLevel::Info,
            "WebHID test command device=" + std::to_string(command_device) +
                " action=" + std::to_string(command_action));

        bool update_bt_state = false;
        std::string waveout_message;
        {
          std::lock_guard guard(state.mutex);
          if (command_device == kTestCommandAudioDevice &&
              command_action == kTestCommandWaveoutPrepare) {
            state.speaker_waveout_selected =
                frame.payload.size() > command_data_offset + 2 &&
                frame.payload[command_data_offset + 2] ==
                    kTestCommandSpeakerParam;
            waveout_message =
                std::string("WebHID waveout target=") +
                (state.speaker_waveout_selected ? "speaker" : "headphone");
          } else if (command_device == kTestCommandAudioDevice &&
                     command_action == kTestCommandWaveoutControl &&
                     frame.payload.size() > command_data_offset) {
            const bool enable = frame.payload[command_data_offset] != 0;
            const bool speaker_waveout =
                enable && state.speaker_waveout_selected;
            state.speaker_waveout_active = speaker_waveout;
            state.speaker_waveout_phase = 0;
            state.waveout_extractor =
                vds::PcmAudioExtractor{kWindowsPcmWindowFrames};
            state.output_state.set_audio_out_stream_active(
                speaker_waveout, state.headset_plugged);
            state.audio_out_stream_active = speaker_waveout;
            state.audio_pcm_stream_active = speaker_waveout;
            state.audio_jitter_buffering = speaker_waveout;
            state.audio_queue_empty_since = {};
            state.audio_queue_drained_time = {};
            state.have_last_audio_send_time = false;
            state.last_audio_chunk.reset();
            if (!speaker_waveout) {
              state.pending_audio_chunks.clear();
            }
            update_bt_state = true;
            waveout_message =
                std::string("WebHID waveout ") + (enable ? "on" : "off") +
                " target=" +
                (state.speaker_waveout_selected ? "speaker" : "headphone") +
                " synthesized=" + (speaker_waveout ? "yes" : "no");
          }
        }
        if (!waveout_message.empty()) {
          logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                     waveout_message);
        }
        if (update_bt_state) {
          send_bt_state_if_changed(bluetooth, state, logger, bluetooth_mutex);
        }
      }
    }

    if (synthetic_test_command) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                 "feature set report_id=" + std::to_string(report_id) +
                     " bluetooth_forward=no reason=synthetic_webhid_test");
      return;
    }

    try {
      std::lock_guard guard(bluetooth_mutex);
      bluetooth.write_feature_report(frame.payload);
      logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                 "feature set report_id=" + std::to_string(report_id) +
                     " bluetooth_forward=yes");
    } catch (const std::exception &error) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Warn,
                 "feature set report_id=" + std::to_string(report_id) +
                     " failed: " + error.what());
    }
    return;
  }

  if (frame.header.type != VDS_FRAME_USB_AUDIO_OUT) {
    return;
  }

  const auto audio_start = Clock::now();
  const auto peaks = pcm_channel_peaks(frame.payload);
  std::size_t queued_chunks = 0;
  std::size_t dropped_chunks = 0;
  std::size_t pending_chunks = 0;
  std::uint64_t audio_frame_count = 0;
  std::uint64_t dropped_total = 0;
  std::uint64_t underflow_refill_count = 0;
  Clock::duration underflow_refill_gap{};
  Clock::duration underflow_refill_max_gap{};
  bool haptics_nonzero_first_seen = false;
  bool audio_route_activated = false;
  bool underflow_refilled = false;
  bool log_dropped_summary = false;
  bool audio_pcm_stream_started = false;
  const auto extract_start = Clock::now();
  std::vector<vds::AudioChunk> chunks;
  {
    std::lock_guard guard(state.mutex);
    const bool reset_audio_route =
        !state.audio_out_stream_active || state.speaker_waveout_active;
    if (reset_audio_route) {
      state.speaker_waveout_active = false;
      state.output_state.set_audio_out_stream_active(true,
                                                     state.headset_plugged);
      state.audio_out_stream_active = true;
      state.audio_pcm_stream_active = false;
      state.audio_stream_had_pcm = false;
      state.audio_jitter_buffering = true;
      state.audio_jitter_buffer_start = {};
      state.audio_queue_empty_since = {};
      state.audio_queue_drained_time = {};
      state.have_last_audio_send_time = false;
      state.last_audio_usb_frame_time = {};
      state.audio_last_flush_time = {};
      state.audio_last_summary_time = {};
      state.last_audio_chunk.reset();
      state.audio_usb_max_gap = {};
      state.audio_send_max_gap = {};
      state.audio_underflow_max_gap = {};
      state.audio_flush_max_gap = {};
      state.audio_usb_gap_warn_count = 0;
      state.audio_send_gap_warn_count = 0;
      state.audio_flush_gap_warn_count = 0;
      state.audio_low_watermark_pace_count = 0;
      state.audio_last_summary_usb_frame_count = 0;
      state.audio_last_summary_sent_count = 0;
      state.audio_last_summary_concealed_count = 0;
      state.audio_last_summary_keepalive_sent_count = 0;
      state.audio_last_summary_dropped_count = 0;
      state.audio_last_summary_blocked_count = 0;
      state.audio_last_summary_underflow_count = 0;
      state.audio_last_summary_usb_gap_warn_count = 0;
      state.audio_last_summary_send_gap_warn_count = 0;
      state.audio_last_summary_flush_gap_warn_count = 0;
      state.audio_last_summary_low_watermark_pace_count = 0;
      state.audio_haptics_sent_seen = false;
      state.audio_underflow_reported = false;
      state.pending_audio_chunks.clear();
      state.next_haptics_send_time = {};
      state.extractor = vds::PcmAudioExtractor{kWindowsPcmWindowFrames};
      audio_route_activated = true;
    }
    chunks = state.extractor.push_usb_audio(frame.payload);
    const auto extract_duration = Clock::now() - extract_start;
    if (!chunks.empty() && !state.audio_pcm_stream_active) {
      state.audio_pcm_stream_active = true;
      state.audio_stream_had_pcm = true;
      state.audio_jitter_buffering = true;
      state.audio_jitter_buffer_start = {};
      state.audio_queue_empty_since = {};
      state.audio_queue_drained_time = {};
      state.have_last_audio_send_time = false;
      state.last_audio_usb_frame_time = {};
      state.audio_last_flush_time = {};
      state.audio_last_summary_time = {};
      state.last_audio_chunk.reset();
      state.audio_usb_max_gap = {};
      state.audio_send_max_gap = {};
      state.audio_underflow_max_gap = {};
      state.audio_flush_max_gap = {};
      state.audio_usb_gap_warn_count = 0;
      state.audio_send_gap_warn_count = 0;
      state.audio_flush_gap_warn_count = 0;
      state.audio_low_watermark_pace_count = 0;
      state.audio_last_summary_usb_frame_count = state.audio_usb_frame_count;
      state.audio_last_summary_sent_count = state.audio_sent_count;
      state.audio_last_summary_concealed_count = state.audio_concealed_count;
      state.audio_last_summary_keepalive_sent_count =
          state.audio_keepalive_sent_count;
      state.audio_last_summary_dropped_count = state.audio_dropped_count;
      state.audio_last_summary_blocked_count = state.audio_blocked_count;
      state.audio_last_summary_underflow_count = state.audio_underflow_count;
      state.audio_last_summary_usb_gap_warn_count = 0;
      state.audio_last_summary_send_gap_warn_count = 0;
      state.audio_last_summary_flush_gap_warn_count = 0;
      state.audio_last_summary_low_watermark_pace_count = 0;
      state.audio_haptics_sent_seen = false;
      state.audio_underflow_reported = false;
      state.pending_audio_chunks.clear();
      state.next_haptics_send_time = {};
      audio_pcm_stream_started = true;
    }
    if (!chunks.empty()) {
      state.audio_stream_had_pcm = true;
    }
    ++state.audio_usb_frame_count;
    audio_frame_count = state.audio_usb_frame_count;
    if (state.last_audio_usb_frame_time != Clock::time_point{}) {
      const auto frame_gap = audio_start - state.last_audio_usb_frame_time;
      state.audio_usb_max_gap = std::max(state.audio_usb_max_gap, frame_gap);
      if (frame_gap >= kAudioSendGapWarn) {
        ++state.audio_usb_gap_warn_count;
      }
    }
    state.last_audio_usb_frame_time = audio_start;
    const bool haptics_nonzero = peaks[2] != 0 || peaks[3] != 0;
    haptics_nonzero_first_seen =
        haptics_nonzero && !state.audio_haptics_nonzero_seen;
    state.audio_haptics_nonzero_seen |= haptics_nonzero;
    if (!chunks.empty() && state.audio_underflow_reported) {
      underflow_refilled = true;
      ++state.audio_underflow_refill_count;
      underflow_refill_count = state.audio_underflow_refill_count;
      if (state.audio_queue_empty_since != Clock::time_point{}) {
        underflow_refill_gap = audio_start - state.audio_queue_empty_since;
        state.audio_underflow_max_gap =
            std::max(state.audio_underflow_max_gap, underflow_refill_gap);
        underflow_refill_max_gap = state.audio_underflow_max_gap;
      }
      state.audio_underflow_reported = false;
    }

    for (const auto &chunk : chunks) {
      if (state.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
        state.pending_audio_chunks.pop_front();
        ++dropped_chunks;
        ++state.audio_dropped_count;
      }
      state.pending_audio_chunks.push_back(chunk);
      state.audio_queue_empty_reported = false;
      state.audio_queue_empty_since = {};
      state.audio_queue_drained_time = {};
      ++queued_chunks;
      ++state.audio_queued_count;
    }
    pending_chunks = state.pending_audio_chunks.size();
    dropped_total = state.audio_dropped_count;
    log_dropped_summary =
        dropped_chunks > 0 &&
        (dropped_total == dropped_chunks || dropped_total % 1000 == 0);

    if (queued_chunks > 0 &&
        (audio_frame_count == 1 || audio_pcm_stream_started ||
         haptics_nonzero_first_seen || log_dropped_summary)) {
      logger.log(vds::LogScope::Output, vds::LogLevel::Info,
                 "audio out frame=" + std::to_string(audio_frame_count) +
                     " bytes=" + std::to_string(frame.payload.size()) +
                     " chunks=" + std::to_string(queued_chunks) +
                     " pending=" + std::to_string(pending_chunks) +
                     " dropped_total=" + std::to_string(dropped_total) +
                     " peak_ch0=" + std::to_string(peaks[0]) +
                     " peak_ch1=" + std::to_string(peaks[1]) +
                     " peak_ch2=" + std::to_string(peaks[2]) +
                     " peak_ch3=" + std::to_string(peaks[3]) + " extract_us=" +
                     std::to_string(duration_us(extract_duration)) +
                     " total_us=" +
                     std::to_string(duration_us(Clock::now() - audio_start)) +
                     " pcm_stream_started=" +
                     (audio_pcm_stream_started ? "yes" : "no"));
    }
  }

  if (log_dropped_summary) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               "dropped BT 0x36 audio packets dropped=" +
                   std::to_string(dropped_chunks) +
                   " total=" + std::to_string(dropped_total) +
                   " pending=" + std::to_string(pending_chunks));
  }
  if (underflow_refilled) {
    logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
               "audio packet queue refilled after underflow refill_count=" +
                   std::to_string(underflow_refill_count) + " gap_us=" +
                   std::to_string(duration_us(underflow_refill_gap)) +
                   " max_gap_us=" +
                   std::to_string(duration_us(underflow_refill_max_gap)));
  }
  if (audio_route_activated) {
    (void)bluetooth.take_output_diagnostics(true);
    logger.log(vds::LogScope::Output, vds::LogLevel::Info,
               "audio out stream auto-activated by PCM frame");
    send_bt_state_if_changed(bluetooth, state, logger, bluetooth_mutex);
  }
}

void handle_bluetooth_frame(HANDLE virtual_device,
                            BluetoothTransport &bluetooth, BridgeState &state,
                            const Frame &frame, vds::Logger &logger,
                            std::uint32_t trace_flags,
                            std::mutex &bluetooth_mutex) {
  if (frame.header.type == VDS_FRAME_BT_INTERRUPT_PACKET) {
    if (vds::bt_input_payload_type(frame.payload) ==
        vds::BtInputPayloadType::Audio) {
      const bool input_audio_trace =
          trace_enabled(trace_flags, kTraceInputAudio);
      std::optional<std::vector<std::uint8_t>> pcm;
      bool decode_failed = false;
      {
        std::lock_guard guard(state.mutex);
        if (const auto opus_payload = vds::bt_mic_opus_payload(frame.payload)) {
          try {
            pcm = state.mic_decoder.decode(*opus_payload);
          } catch (const std::exception &) {
            ++state.mic_input_decode_fail_count;
            decode_failed = true;
          }
        } else {
          ++state.mic_input_decode_fail_count;
          decode_failed = true;
        }
      }

      if (decode_failed) {
        if (input_audio_trace) {
          logger.log(vds::LogScope::InputAudio, vds::LogLevel::Warn,
                     "mic packet decode failed");
        }
        return;
      }
      if (pcm) {
        for (std::size_t offset = 0; offset + 3 < pcm->size();
             offset += 2 * sizeof(std::int16_t)) {
          (*pcm)[offset + 0] = (*pcm)[offset + 2];
          (*pcm)[offset + 1] = (*pcm)[offset + 3];
        }

        std::array<int, 2> pcm_peaks{};
        for (std::size_t offset = 0; offset + 3 < pcm->size();
             offset += 2 * sizeof(std::int16_t)) {
          for (std::size_t channel = 0; channel < pcm_peaks.size(); ++channel) {
            const std::size_t sample_offset =
                offset + channel * sizeof(std::int16_t);
            const auto raw =
                static_cast<std::uint16_t>((*pcm)[sample_offset + 0]) |
                (static_cast<std::uint16_t>((*pcm)[sample_offset + 1]) << 8);
            const auto sample = static_cast<std::int16_t>(raw);
            const int value = static_cast<int>(sample);
            const int magnitude = value < 0 ? -value : value;
            pcm_peaks[channel] = std::max(pcm_peaks[channel], magnitude);
          }
        }

        vds::win::write_handle_frame(virtual_device, VDS_FRAME_USB_AUDIO_IN,
                                     *pcm, "virtual", handle_io_cancellation());
        std::uint64_t forwarded = 0;
        {
          std::lock_guard guard(state.mutex);
          forwarded = ++state.mic_input_forward_count;
        }
        if (input_audio_trace && (forwarded == 1 || forwarded % 1000 == 0)) {
          logger.log(vds::LogScope::InputAudio, vds::LogLevel::Debug,
                     "mic forwarded count=" + std::to_string(forwarded) +
                         " len=" + std::to_string(pcm->size()) +
                         " peak_ch0=" + std::to_string(pcm_peaks[0]) +
                         " peak_ch1=" + std::to_string(pcm_peaks[1]));
        }
      }
      return;
    }

    const auto report = bt_input_to_usb_input(frame.payload);
    if (report) {
      vds::win::write_handle_frame(virtual_device, VDS_FRAME_USB_HID_IN,
                                   *report, "virtual",
                                   handle_io_cancellation());
      const std::uint8_t headset_status = report->at(kUsbInputHeadsetOffset);
      const bool headset_plugged =
          (headset_status & kUsbInputHeadphonesPluggedMask) != 0;
      const bool headset_mic_plugged =
          (headset_status & kUsbInputMicPluggedMask) != 0;
      const bool mute_button_down = (report->at(kUsbInputMuteButtonOffset) &
                                     kUsbInputMuteButtonMask) != 0;
      bool headset_changed = false;
      bool headset_mic_changed = false;
      bool mic_mute_changed = false;
      bool mic_muted = false;
      bool send_mic_state = false;
      bool output_state_changed = false;
      vds::BtStateReport mic_state_report{};
      vds::BtInitReport mic_close_report{};
      vds::BtInitReport mic_report{};
      std::uint64_t forward_count = 0;
      bool input_changed = false;
      bool send_mic_close = false;
      bool send_mic_open = false;
      {
        std::lock_guard guard(state.mutex);
        forward_count = ++state.usb_input_forward_count;
        input_changed =
            !state.have_last_logged_input_report ||
            input_controls_changed(state.last_logged_input_report, *report);
        if (headset_plugged != state.headset_plugged) {
          state.headset_plugged = headset_plugged;
          headset_changed = true;
          if (state.audio_out_stream_active || state.speaker_waveout_active) {
            state.output_state.set_audio_out_stream_active(
                true, state.headset_plugged);
            output_state_changed = true;
          }
        }
        if (headset_mic_plugged != state.headset_mic_plugged) {
          state.headset_mic_plugged = headset_mic_plugged;
          state.output_state.set_headset_mic_plugged(headset_mic_plugged);
          headset_mic_changed = true;
        }
        if (mute_button_down && !state.mute_button_down) {
          state.mic_muted = !state.mic_muted;
          mic_mute_changed = true;
        }
        state.mute_button_down = mute_button_down;
        mic_muted = state.mic_muted;
        send_mic_state = mic_mute_changed ||
                         (headset_mic_changed && state.audio_in_stream_active);
        if (send_mic_state) {
          mic_state_report = state.output_state.build_bt_mic_state_report(
              state.audio_in_stream_active, state.mic_muted);
        }
        if (headset_mic_changed && state.audio_in_stream_active) {
          mic_close_report = state.output_state.build_bt_mic_report(false);
          mic_report = state.output_state.build_bt_mic_report(true);
          send_mic_close = true;
          send_mic_open = true;
        }
      }
      if (mic_mute_changed) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                   "mic " + std::string(mic_muted ? "muted" : "unmuted"));
      }
      if (send_mic_state) {
        if (send_mic_close) {
          try_send_bt_output_report(bluetooth, mic_close_report, logger,
                                    bluetooth_mutex, "mic");
        }
        try_send_bt_output_report(bluetooth, mic_state_report, logger,
                                  bluetooth_mutex, "mic state");
      }
      if (send_mic_open) {
        try_send_bt_output_report(bluetooth, mic_report, logger,
                                  bluetooth_mutex, "mic");
      }
      if (headset_changed) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                   "headset " +
                       std::string(headset_plugged ? "plugged" : "unplugged"));
      }
      if (headset_mic_changed) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                   "headset mic " + std::string(headset_mic_plugged
                                                    ? "plugged"
                                                    : "unplugged"));
      }
      if (headset_changed || output_state_changed) {
        send_bt_state_if_changed(bluetooth, state, logger, bluetooth_mutex);
      }
      if (input_changed) {
        std::uint64_t change_count = 0;
        {
          std::lock_guard guard(state.mutex);
          state.last_logged_input_report = *report;
          state.have_last_logged_input_report = true;
          change_count = ++state.usb_input_change_log_count;
        }
        if (change_count == 1 ||
            trace_enabled(trace_flags, kTraceInputControl)) {
          logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                     "hid in changed count=" + std::to_string(change_count) +
                         " forwarded=" + std::to_string(forward_count) + " " +
                         input_report_summary(*report));
        }
      }
      if (forward_count == 1 || forward_count % 1024 == 0) {
        logger.log(vds::LogScope::InputControl, vds::LogLevel::Info,
                   std::string("hid in forwarded count=") +
                       std::to_string(forward_count));
      }
    }
    return;
  }

  if (frame.header.type != VDS_FRAME_BT_CONTROL_PACKET) {
    return;
  }

  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
             "control rx length=" + std::to_string(frame.payload.size()) +
                 " prefix=" + hex_bytes(frame.payload, 8, ':', false));
  const auto report = bt_feature_to_usb_feature_reply(frame.payload);
  if (report) {
    vds::win::write_handle_frame(virtual_device, VDS_FRAME_USB_FEATURE_REPLY,
                                 *report, "virtual", handle_io_cancellation());
    if (!report->empty()) {
      logger.log(vds::LogScope::Usb, vds::LogLevel::Info,
                 "feature reply report_id=" + std::to_string((*report)[0]) +
                     " length=" + std::to_string(report->size()));
    }
  }
}

void send_initial_bluetooth_reports(BluetoothTransport &bluetooth,
                                    BridgeState &state, vds::Logger &logger,
                                    std::mutex &bluetooth_mutex) {
  logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Info,
             "sending Windows startup Bluetooth output init report");
  try_send_bt_output_report(bluetooth,
                            state.output_state.build_bt_init_report(), logger,
                            bluetooth_mutex, "init");
  {
    std::lock_guard guard(state.mutex);
    state.audio_in_stream_active = false;
    state.mic_decoder = vds::MicAudioDecoder{};
    state.output_state.set_headset_mic_plugged(state.headset_mic_plugged);
    state.last_sent_state = state.output_state.state();
    state.have_last_sent_state = true;
  }
  logger.log(vds::LogScope::InputAudio, vds::LogLevel::Info,
             "startup mic stream inactive until USB capture opens");
}

void enqueue_speaker_waveout_chunk(BridgeState &state) {
  std::lock_guard guard(state.mutex);
  if (!state.speaker_waveout_active || !state.pending_audio_chunks.empty()) {
    return;
  }

  std::array<std::uint8_t, kWindowsPcmWindowFrames * VDS_AUDIO_CHANNELS *
                               sizeof(std::int16_t)>
      pcm{};
  for (std::size_t frame = 0; frame < kWindowsPcmWindowFrames; ++frame) {
    const double angle = kSpeakerWaveoutTwoPi *
                         static_cast<double>(state.speaker_waveout_phase) /
                         static_cast<double>(kSpeakerWaveoutPeriodFrames);
    const auto sample = static_cast<std::int16_t>(
        std::sin(angle) * static_cast<double>(kSpeakerWaveoutAmplitude));
    state.speaker_waveout_phase =
        (state.speaker_waveout_phase + 1) % kSpeakerWaveoutPeriodFrames;

    for (std::size_t channel = 0; channel < vds::kSpeakerChannels; ++channel) {
      const std::size_t offset =
          (frame * VDS_AUDIO_CHANNELS + channel) * sizeof(std::int16_t);
      const auto value = static_cast<std::uint16_t>(sample);
      pcm[offset + 0] = static_cast<std::uint8_t>(value & 0xff);
      pcm[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }
  }

  const auto chunks = state.waveout_extractor.push_usb_audio(pcm);
  for (const auto &chunk : chunks) {
    if (state.pending_audio_chunks.size() >= kMaxPendingAudioChunks) {
      break;
    }
    state.pending_audio_chunks.push_back(chunk);
  }
}

Clock::duration flush_pending_audio_chunk(BluetoothTransport &bluetooth,
                                          BridgeState &state,
                                          vds::Logger &logger,
                                          std::mutex &bluetooth_mutex) {
  vds::BtReport packet{};
  std::size_t pending_after_pop = 0;
  std::uint64_t sent_count = 0;
  std::uint64_t concealed_count = 0;
  std::uint64_t keepalive_sent_count = 0;
  std::uint64_t dropped_count = 0;
  std::uint64_t stale_dropped_count = 0;
  std::uint64_t blocked_count = 0;
  std::size_t stale_dropped_now = 0;
  bool has_signal = false;
  bool has_haptics_signal = false;
  bool have_gap = false;
  bool sending_keepalive = false;
  bool sending_concealment = false;
  bool haptics_sent_first_seen = false;
  bool haptics_limited = false;
  bool low_watermark_paced = false;
  bool high_watermark_catchup = false;
  bool have_schedule_late = false;
  bool schedule_reset = false;
  Clock::duration send_interval = kAudioOutputBaseInterval;
  Clock::duration send_gap{};
  Clock::duration schedule_late{};
  Clock::duration next_sleep{};
  Clock::time_point scheduled_send_time{};
  Clock::time_point send_time{};
  vds::AudioChunk sent_audio_chunk{};
  std::optional<vds::BtStateReport> stream_stop_report;
  bool have_sent_audio_chunk = false;
  bool audio_route_deactivated = false;

  {
    std::lock_guard guard(state.mutex);
    const auto now = Clock::now();
    if (state.pending_audio_chunks.empty()) {
      if (!state.audio_out_stream_active || !state.audio_pcm_stream_active) {
        return kAudioFlushIdleSleep;
      }

      state.audio_jitter_buffering = false;
      state.audio_jitter_buffer_start = {};
      if (state.audio_queue_empty_since == Clock::time_point{}) {
        state.audio_queue_empty_since = now;
      }
      const auto empty_duration = now - state.audio_queue_empty_since;
      if (state.next_haptics_send_time != Clock::time_point{} &&
          now < state.next_haptics_send_time) {
        const auto remaining = state.next_haptics_send_time - now;
        return std::max<Clock::duration>(
            kAudioFlushMinimumSleep,
            std::min<Clock::duration>(kAudioFlushIdleSleep, remaining));
      }
      if (empty_duration < kAudioUnderflowKeepaliveDelay &&
          state.last_audio_chunk) {
        vds::AudioChunk chunk = *state.last_audio_chunk;
        chunk.haptics.fill(0);
        chunk.has_haptics_signal = false;
        has_signal = chunk.has_signal;
        packet = state.haptics_builder.build_packet(
            chunk.haptics, chunk.speaker, state.output_state.state(), true,
            state.headset_plugged);
        sending_concealment = true;
        scheduled_send_time =
            state.next_haptics_send_time == Clock::time_point{}
                ? now
                : state.next_haptics_send_time;
        send_time = now;
      } else {
        if (empty_duration < kAudioUnderflowKeepaliveDelay) {
          const auto remaining = kAudioUnderflowKeepaliveDelay - empty_duration;
          return std::max<Clock::duration>(
              kAudioFlushMinimumSleep,
              std::min<Clock::duration>(kAudioFlushIdleSleep, remaining));
        }
        if (!state.audio_underflow_reported && state.audio_sent_count != 0) {
          ++state.audio_underflow_count;
          state.audio_underflow_reported = true;
          state.audio_queue_empty_reported = true;
          state.audio_queue_drained_time = state.audio_queue_empty_since;
          logger.log(
              vds::LogScope::Output, vds::LogLevel::Warn,
              "audio packet queue underflow during active stream count=" +
                  std::to_string(state.audio_underflow_count) +
                  " queued=" + std::to_string(state.audio_queued_count) +
                  " sent=" + std::to_string(state.audio_sent_count) +
                  " keepalive_sent=" +
                  std::to_string(state.audio_keepalive_sent_count) +
                  " dropped=" + std::to_string(state.audio_dropped_count) +
                  " stale_dropped=" +
                  std::to_string(state.audio_stale_dropped_count) +
                  " blocked=" + std::to_string(state.audio_blocked_count) +
                  " keepalive_blocked=" +
                  std::to_string(state.audio_keepalive_blocked_count) +
                  " usb_max_gap_us=" +
                  std::to_string(duration_us(state.audio_usb_max_gap)) +
                  " usb_gap_warn=" +
                  std::to_string(state.audio_usb_gap_warn_count) +
                  " send_max_gap_us=" +
                  std::to_string(duration_us(state.audio_send_max_gap)) +
                  " send_gap_warn=" +
                  std::to_string(state.audio_send_gap_warn_count));
          if (auto diagnostics = bluetooth.take_output_diagnostics(true)) {
            logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
                       *diagnostics);
          }
        }
        state.audio_pcm_stream_active = false;
        if (!state.speaker_waveout_active) {
          state.output_state.set_audio_out_stream_active(false,
                                                         state.headset_plugged);
          state.audio_out_stream_active = false;
          state.audio_jitter_buffering = false;
          state.audio_queue_empty_since = {};
          state.audio_queue_drained_time = {};
          state.next_haptics_send_time = {};
          state.last_audio_chunk.reset();

          const vds::DsState current = state.output_state.state();
          if (!state.have_last_sent_state || state.last_sent_state != current) {
            stream_stop_report = state.output_state.build_bt_state_report();
            state.last_sent_state = current;
            state.have_last_sent_state = true;
          }
          audio_route_deactivated = true;
        }
        state.next_haptics_send_time = {};
        next_sleep = kAudioFlushIdleSleep;
      }
    } else {
      if (state.audio_jitter_buffering) {
        if (state.audio_jitter_buffer_start == Clock::time_point{}) {
          state.audio_jitter_buffer_start = now;
        }
        const auto buffer_age = now - state.audio_jitter_buffer_start;
        if (state.pending_audio_chunks.size() < kAudioPrimeChunks &&
            buffer_age < kAudioJitterBufferMaxDelay) {
          return std::chrono::milliseconds(1);
        }
        state.audio_jitter_buffering = false;
        state.next_haptics_send_time = now;
      }

      if (state.next_haptics_send_time != Clock::time_point{} &&
          now < state.next_haptics_send_time) {
        const auto remaining = state.next_haptics_send_time - now;
        return std::max<Clock::duration>(
            kAudioFlushMinimumSleep,
            std::min<Clock::duration>(kAudioFlushIdleSleep, remaining));
      }

      while (state.pending_audio_chunks.size() > kAudioFreshQueueChunks) {
        state.pending_audio_chunks.pop_front();
        ++state.audio_dropped_count;
        ++state.audio_stale_dropped_count;
        ++stale_dropped_now;
      }
      const vds::AudioChunk chunk = state.pending_audio_chunks.front();
      has_signal = chunk.has_signal;
      has_haptics_signal = chunk.has_haptics_signal;
      vds::HapticsChunk haptics = chunk.haptics;
      for (std::int8_t &sample : haptics) {
        const auto limited = static_cast<std::int8_t>(std::clamp<int>(
            sample, kWindowsHapticsSampleMin, kWindowsHapticsSampleMax));
        haptics_limited = haptics_limited || limited != sample;
        sample = limited;
      }
      const bool route_headset = state.headset_plugged;
      packet = state.haptics_builder.build_packet(haptics, chunk.speaker,
                                                  state.output_state.state(),
                                                  true, route_headset);
      sent_audio_chunk = chunk;
      have_sent_audio_chunk = true;
      scheduled_send_time = state.next_haptics_send_time == Clock::time_point{}
                                ? now
                                : state.next_haptics_send_time;
      send_time = now;
      stale_dropped_count = state.audio_stale_dropped_count;
    }
  }

  if (audio_route_deactivated) {
    if (stream_stop_report) {
      (void)try_send_bt_output_report(bluetooth, *stream_stop_report, logger,
                                      bluetooth_mutex, "state");
    }
    logger.log(vds::LogScope::Output, vds::LogLevel::Info,
               "audio out stream auto-deactivated after PCM drain");
    return next_sleep;
  }

  const auto write_start = Clock::now();
  const bool sent = try_send_bt_output_report(bluetooth, packet, logger,
                                              bluetooth_mutex, "audio");
  const auto write_duration = Clock::now() - write_start;
  if (!sent) {
    std::size_t pending_count = 0;
    {
      std::lock_guard guard(state.mutex);
      if (!sending_keepalive && !sending_concealment &&
          !state.pending_audio_chunks.empty()) {
        state.pending_audio_chunks.pop_front();
      }
      if (sending_keepalive) {
        ++state.audio_keepalive_blocked_count;
      } else {
        ++state.audio_blocked_count;
        if (!sending_concealment) {
          ++state.audio_dropped_count;
        }
      }
      blocked_count = state.audio_blocked_count;
      dropped_count = state.audio_dropped_count;
      pending_count = state.pending_audio_chunks.size();
      if (!sending_keepalive) {
        send_interval = audio_output_interval_for_pending(
            pending_count, low_watermark_paced, high_watermark_catchup);
        if (low_watermark_paced) {
          ++state.audio_low_watermark_pace_count;
        }
      }
      state.next_haptics_send_time = scheduled_send_time + send_interval;
      if (state.next_haptics_send_time <= send_time) {
        state.next_haptics_send_time = send_time + send_interval;
      }
      stale_dropped_count = state.audio_stale_dropped_count;
    }
    if (blocked_count == 1 || (blocked_count % 1000) == 0) {
      logger.log(vds::LogScope::Output, vds::LogLevel::Warn,
                 "dropped BT 0x36 audio packet after HID queue blocked count=" +
                     std::to_string(blocked_count) +
                     " pending=" + std::to_string(pending_count) +
                     " dropped=" + std::to_string(dropped_count) +
                     " stale_dropped=" + std::to_string(stale_dropped_count));
    }
    return Clock::duration::zero();
  }

  {
    std::lock_guard guard(state.mutex);
    if (!sending_keepalive && !sending_concealment &&
        !state.pending_audio_chunks.empty()) {
      state.pending_audio_chunks.pop_front();
    }
    pending_after_pop = state.pending_audio_chunks.size();
    if (pending_after_pop == 0) {
      state.audio_jitter_buffer_start = {};
    }
    state.last_sent_state = state.output_state.state();
    state.have_last_sent_state = true;
    if (sending_keepalive) {
      ++state.audio_keepalive_sent_count;
      keepalive_sent_count = state.audio_keepalive_sent_count;
    } else if (sending_concealment) {
      ++state.audio_concealed_count;
      concealed_count = state.audio_concealed_count;
    } else {
      ++state.audio_sent_count;
      if (have_sent_audio_chunk) {
        state.last_audio_chunk = sent_audio_chunk;
      }
      if (has_haptics_signal && !state.audio_haptics_sent_seen) {
        state.audio_haptics_sent_seen = true;
        haptics_sent_first_seen = true;
      }
    }
    sent_count = state.audio_sent_count;
    dropped_count = state.audio_dropped_count;
    stale_dropped_count = state.audio_stale_dropped_count;
    blocked_count = state.audio_blocked_count;
    if (state.have_last_audio_send_time) {
      have_gap = true;
      send_gap = send_time - state.last_audio_send_time;
      state.audio_send_max_gap = std::max(state.audio_send_max_gap, send_gap);
      if (send_gap >= kAudioSendGapWarn) {
        ++state.audio_send_gap_warn_count;
      }
    }
    state.last_audio_send_time = send_time;
    state.have_last_audio_send_time = true;
    if (scheduled_send_time != Clock::time_point{} &&
        send_time > scheduled_send_time) {
      have_schedule_late = true;
      schedule_late = send_time - scheduled_send_time;
    }
    if (!sending_keepalive && !sending_concealment) {
      send_interval = audio_output_interval_for_pending(
          pending_after_pop, low_watermark_paced, high_watermark_catchup);
      if (low_watermark_paced) {
        ++state.audio_low_watermark_pace_count;
      }
    }
    state.next_haptics_send_time = scheduled_send_time + send_interval;
    if (state.next_haptics_send_time <= send_time) {
      state.next_haptics_send_time = send_time + send_interval;
      schedule_reset = true;
    }
    if (state.next_haptics_send_time > send_time) {
      next_sleep = state.next_haptics_send_time - send_time;
    }
  }

  if (stale_dropped_now > 0 || (have_gap && send_gap >= kAudioSendGapWarn) ||
      write_duration >= kAudioWriteSlowWarn ||
      schedule_late >= kAudioWriteSlowWarn || sent_count == 1 ||
      haptics_sent_first_seen) {
    const bool delayed_send = stale_dropped_now > 0 ||
                              (have_gap && send_gap >= kAudioSendGapWarn) ||
                              write_duration >= kAudioWriteSlowWarn ||
                              schedule_late >= kAudioWriteSlowWarn;
    std::string message =
        std::string(sending_keepalive
                        ? "audio keepalive sent count="
                        : (sending_concealment ? "audio concealment sent count="
                                               : "audio sent count=")) +
        std::to_string(
            sending_keepalive
                ? keepalive_sent_count
                : (sending_concealment ? concealed_count : sent_count)) +
        " pending=" + std::to_string(pending_after_pop) +
        " dropped=" + std::to_string(dropped_count) +
        " stale_dropped_now=" + std::to_string(stale_dropped_now) +
        " stale_dropped=" + std::to_string(stale_dropped_count) +
        " blocked=" + std::to_string(blocked_count) +
        " write_us=" + std::to_string(duration_us(write_duration)) +
        " interval_us=" + std::to_string(duration_us(send_interval)) +
        " next_us=" + std::to_string(duration_us(next_sleep)) +
        " has_signal=" + (has_signal ? "yes" : "no") +
        " has_haptics=" + (has_haptics_signal ? "yes" : "no");
    if (have_schedule_late) {
      message += " due_late_us=" + std::to_string(duration_us(schedule_late));
    }
    if (schedule_reset) {
      message += " schedule_reset=yes";
    }
    if (haptics_limited) {
      message += " haptics_limited=yes";
    }
    if (low_watermark_paced) {
      message += " low_watermark_pace=yes";
    }
    if (high_watermark_catchup) {
      message += " high_watermark_catchup=yes";
    }
    if (have_gap) {
      message += " gap_us=" + std::to_string(duration_us(send_gap));
    }
    logger.log(vds::LogScope::Output,
               delayed_send ? vds::LogLevel::Warn : vds::LogLevel::Info,
               message);
  }
  return Clock::duration::zero();
}

void maybe_log_audio_summary(BluetoothTransport &bluetooth, BridgeState &state,
                             vds::Logger &logger) {
  std::string message;
  bool warn = false;
  {
    std::lock_guard guard(state.mutex);
    const bool have_audio_activity = state.audio_out_stream_active ||
                                     state.audio_pcm_stream_active ||
                                     !state.pending_audio_chunks.empty();
    if (!have_audio_activity) {
      return;
    }

    const auto now = Clock::now();
    if (state.audio_last_summary_time != Clock::time_point{} &&
        now - state.audio_last_summary_time < kAudioSummaryInterval) {
      return;
    }

    const auto interval = state.audio_last_summary_time == Clock::time_point{}
                              ? Clock::duration{}
                              : now - state.audio_last_summary_time;
    const std::uint64_t usb_delta =
        state.audio_usb_frame_count - state.audio_last_summary_usb_frame_count;
    const std::uint64_t sent_delta =
        state.audio_sent_count - state.audio_last_summary_sent_count;
    const std::uint64_t concealed_delta =
        state.audio_concealed_count - state.audio_last_summary_concealed_count;
    const std::uint64_t keepalive_delta =
        state.audio_keepalive_sent_count -
        state.audio_last_summary_keepalive_sent_count;
    const std::uint64_t dropped_delta =
        state.audio_dropped_count - state.audio_last_summary_dropped_count;
    const std::uint64_t blocked_delta =
        state.audio_blocked_count - state.audio_last_summary_blocked_count;
    const std::uint64_t underflow_delta =
        state.audio_underflow_count - state.audio_last_summary_underflow_count;
    const std::uint64_t usb_gap_warn_delta =
        state.audio_usb_gap_warn_count -
        state.audio_last_summary_usb_gap_warn_count;
    const std::uint64_t send_gap_warn_delta =
        state.audio_send_gap_warn_count -
        state.audio_last_summary_send_gap_warn_count;
    const std::uint64_t flush_gap_warn_delta =
        state.audio_flush_gap_warn_count -
        state.audio_last_summary_flush_gap_warn_count;
    const std::uint64_t low_watermark_pace_delta =
        state.audio_low_watermark_pace_count -
        state.audio_last_summary_low_watermark_pace_count;

    warn = dropped_delta != 0 || blocked_delta != 0 || underflow_delta != 0 ||
           usb_gap_warn_delta != 0 || send_gap_warn_delta != 0 ||
           flush_gap_warn_delta != 0;
    message =
        "audio summary interval_us=" + std::to_string(duration_us(interval)) +
        " active=" + (state.audio_out_stream_active ? "yes" : "no") +
        " pcm_active=" + (state.audio_pcm_stream_active ? "yes" : "no") +
        " pending=" + std::to_string(state.pending_audio_chunks.size()) +
        " usb_frames=" + std::to_string(state.audio_usb_frame_count) +
        " usb_delta=" + std::to_string(usb_delta) +
        " sent=" + std::to_string(state.audio_sent_count) +
        " sent_delta=" + std::to_string(sent_delta) +
        " concealed=" + std::to_string(state.audio_concealed_count) +
        " concealed_delta=" + std::to_string(concealed_delta) +
        " keepalive=" + std::to_string(state.audio_keepalive_sent_count) +
        " keepalive_delta=" + std::to_string(keepalive_delta) +
        " dropped=" + std::to_string(state.audio_dropped_count) +
        " dropped_delta=" + std::to_string(dropped_delta) +
        " stale_dropped=" + std::to_string(state.audio_stale_dropped_count) +
        " blocked=" + std::to_string(state.audio_blocked_count) +
        " blocked_delta=" + std::to_string(blocked_delta) +
        " underflow=" + std::to_string(state.audio_underflow_count) +
        " underflow_delta=" + std::to_string(underflow_delta) +
        " usb_max_gap_us=" +
        std::to_string(duration_us(state.audio_usb_max_gap)) +
        " send_max_gap_us=" +
        std::to_string(duration_us(state.audio_send_max_gap)) +
        " flush_max_gap_us=" +
        std::to_string(duration_us(state.audio_flush_max_gap)) +
        " gap_warn_delta_usb=" + std::to_string(usb_gap_warn_delta) +
        " gap_warn_delta_send=" + std::to_string(send_gap_warn_delta) +
        " gap_warn_delta_flush=" + std::to_string(flush_gap_warn_delta) +
        " low_watermark_pace=" +
        std::to_string(state.audio_low_watermark_pace_count) +
        " low_watermark_pace_delta=" + std::to_string(low_watermark_pace_delta);

    state.audio_last_summary_time = now;
    state.audio_last_summary_usb_frame_count = state.audio_usb_frame_count;
    state.audio_last_summary_sent_count = state.audio_sent_count;
    state.audio_last_summary_concealed_count = state.audio_concealed_count;
    state.audio_last_summary_keepalive_sent_count =
        state.audio_keepalive_sent_count;
    state.audio_last_summary_dropped_count = state.audio_dropped_count;
    state.audio_last_summary_blocked_count = state.audio_blocked_count;
    state.audio_last_summary_underflow_count = state.audio_underflow_count;
    state.audio_last_summary_usb_gap_warn_count =
        state.audio_usb_gap_warn_count;
    state.audio_last_summary_send_gap_warn_count =
        state.audio_send_gap_warn_count;
    state.audio_last_summary_flush_gap_warn_count =
        state.audio_flush_gap_warn_count;
    state.audio_last_summary_low_watermark_pace_count =
        state.audio_low_watermark_pace_count;
  }

  logger.log(vds::LogScope::Output,
             warn ? vds::LogLevel::Warn : vds::LogLevel::Info, message);
  if (auto diagnostics = bluetooth.take_output_diagnostics(true)) {
    logger.log(vds::LogScope::Output,
               warn ? vds::LogLevel::Warn : vds::LogLevel::Info, *diagnostics);
  }
}

void audio_flush_loop(BluetoothTransport &bluetooth, BridgeState &state,
                      vds::Logger &logger, std::mutex &bluetooth_mutex,
                      std::atomic_bool &bridge_restart_requested) {
  AudioThreadPriorityGuard priority_guard;
  logger.log(vds::LogScope::Output,
             priority_guard.boosted() ? vds::LogLevel::Info
                                      : vds::LogLevel::Warn,
             priority_guard.summary());
  HighResolutionSleeper sleeper;
  logger.log(vds::LogScope::Output,
             sleeper.active() ? vds::LogLevel::Info : vds::LogLevel::Warn,
             std::string("high resolution audio flush timer ") +
                 (sleeper.active() ? "enabled" : "unavailable"));
  while (!g_stop_requested.load() && !bridge_restart_requested.load()) {
    {
      std::lock_guard guard(state.mutex);
      const auto now = Clock::now();
      if (state.audio_last_flush_time != Clock::time_point{}) {
        const auto flush_gap = now - state.audio_last_flush_time;
        state.audio_flush_max_gap =
            std::max(state.audio_flush_max_gap, flush_gap);
        if (flush_gap >= kAudioSendGapWarn) {
          ++state.audio_flush_gap_warn_count;
        }
      }
      state.audio_last_flush_time = now;
    }
    enqueue_speaker_waveout_chunk(state);
    const auto sleep_duration =
        flush_pending_audio_chunk(bluetooth, state, logger, bluetooth_mutex);
    maybe_log_audio_summary(bluetooth, state, logger);
    sleeper.sleep_for(sleep_duration);
  }
}

void virtual_to_bluetooth_loop(HANDLE virtual_device,
                               BluetoothTransport &bluetooth,
                               BridgeState &state, vds::Logger &logger,
                               std::mutex &bluetooth_mutex,
                               std::atomic_bool &bridge_restart_requested) {
  while (!g_stop_requested.load() && !bridge_restart_requested.load()) {
    handle_virtual_frame(virtual_device, bluetooth, state,
                         vds::win::read_handle_frame(virtual_device, "virtual",
                                                     handle_io_cancellation()),
                         logger, bluetooth_mutex, bridge_restart_requested);
  }
}

void bluetooth_to_virtual_loop(BluetoothTransport &bluetooth,
                               HANDLE virtual_device, BridgeState &state,
                               vds::Logger &logger,
                               std::atomic_bool &bridge_restart_requested,
                               const std::atomic_uint32_t *trace_flags,
                               std::mutex &bluetooth_mutex) {
  while (!g_stop_requested.load() && !bridge_restart_requested.load()) {
    handle_bluetooth_frame(
        virtual_device, bluetooth, state, bluetooth.read_frame(), logger,
        trace_flags == nullptr ? 0 : trace_flags->load(), bluetooth_mutex);
  }
}

void run_bridge_session(const std::string &device_address,
                        const std::string &virtual_device,
                        const vds::ControllerConfig *selected_config,
                        vds::Logger &logger,
                        std::atomic_bool *session_stop_requested = nullptr,
                        const std::atomic_uint32_t *trace_flags = nullptr) {
  if (selected_config == nullptr) {
    throw std::runtime_error("Windows bridge session requires config");
  }
  const std::uint32_t profile =
      resolve_virtual_port_profile(*selected_config, logger);

  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "Windows bridge opening virtual=" + virtual_device +
                 " device_address=" + device_address);
  std::string discovery_error;
  const auto device =
      find_connected_bluetooth_device(device_address, &discovery_error);
  if (!device) {
    throw std::runtime_error("failed to open Bluetooth controller: " +
                             discovery_error);
  }
  std::unique_ptr<vds::win::hidhide::DeviceGuard> hidden_device =
      vds::win::hidhide::hide_device(device->instance_path, logger);
  std::unique_ptr<BluetoothTransport> bluetooth =
      make_hid_bluetooth_transport(device_address);
  std::unique_ptr<vds::win::usbip::VirtualPort> virtual_port =
      vds::win::usbip::open_virtual_port(profile, selected_config->ports[0],
                                         logger);
  BridgeState state;
  std::mutex bluetooth_mutex;
  UniqueHandle virtual_device_handle = open_device(virtual_port->pipe_path());
  std::atomic_bool bridge_restart_requested = false;
  std::thread stop_monitor;

  if (session_stop_requested != nullptr) {
    stop_monitor = std::thread([&] {
      while (!g_stop_requested.load() && !bridge_restart_requested.load() &&
             !session_stop_requested->load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (g_stop_requested.load() || session_stop_requested->load()) {
        request_bridge_restart(virtual_device_handle.get(), bluetooth.get(),
                               bridge_restart_requested);
      }
    });
  }
  auto stop_startup_monitors = [&] {
    bridge_restart_requested = true;
    if (stop_monitor.joinable()) {
      stop_monitor.join();
    }
  };

  try {
    send_initial_bluetooth_reports(*bluetooth, state, logger, bluetooth_mutex);
  } catch (...) {
    stop_startup_monitors();
    throw;
  }
  if (bridge_restart_requested.load()) {
    stop_startup_monitors();
    throw std::runtime_error("Bluetooth transport disconnected during startup");
  }

  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "Windows bridge started virtual=" + virtual_device +
                 " bluetooth=" + bluetooth->description());

  std::thread to_bluetooth([&] {
    try {
      virtual_to_bluetooth_loop(virtual_device_handle.get(), *bluetooth, state,
                                logger, bluetooth_mutex,
                                bridge_restart_requested);
    } catch (const std::exception &error) {
      if (!g_stop_requested) {
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                   std::string("virtual-to-bluetooth stopped: ") +
                       error.what());
        request_bridge_restart(virtual_device_handle.get(), bluetooth.get(),
                               bridge_restart_requested);
      }
    }
  });

  std::thread to_virtual([&] {
    try {
      bluetooth_to_virtual_loop(*bluetooth, virtual_device_handle.get(), state,
                                logger, bridge_restart_requested, trace_flags,
                                bluetooth_mutex);
    } catch (const std::exception &error) {
      if (!g_stop_requested) {
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                   std::string("bluetooth-to-virtual stopped: ") +
                       error.what());
        request_bridge_restart(virtual_device_handle.get(), bluetooth.get(),
                               bridge_restart_requested);
      }
    }
  });

  std::thread audio_flush([&] {
    try {
      audio_flush_loop(*bluetooth, state, logger, bluetooth_mutex,
                       bridge_restart_requested);
    } catch (const std::exception &error) {
      if (!g_stop_requested) {
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                   std::string("audio flush stopped: ") + error.what());
        request_bridge_restart(virtual_device_handle.get(), bluetooth.get(),
                               bridge_restart_requested);
      }
    }
  });

  to_bluetooth.join();
  to_virtual.join();
  audio_flush.join();
  bridge_restart_requested = true;
  if (stop_monitor.joinable()) {
    stop_monitor.join();
  }
}

struct BridgeWorker {
  std::string address;
  std::string device_address;
  unsigned port = 0;
  vds::ControllerProfile profile = vds::ControllerProfile::Unspecified;
  std::string last_error;
  UniqueHandle done_event;
  std::thread thread;
  std::atomic_bool stop_requested = false;
  std::atomic_bool done = false;
};

class ControlPipeServer {
public:
  ControlPipeServer(std::string pipe_name, vds::Logger &logger)
      : logger_(logger), pipe_name_(std::move(pipe_name)),
        stop_event_(CreateEventA(nullptr, TRUE, FALSE, nullptr)),
        command_event_(CreateEventA(nullptr, TRUE, FALSE, nullptr)) {
    if (!stop_event_ || !command_event_) {
      throw std::runtime_error("failed to create control pipe event: " +
                               win32_error_message(GetLastError()));
    }
    thread_ = std::thread([this] { thread_main(); });
  }

  ~ControlPipeServer() { stop(); }

  ControlPipeServer(const ControlPipeServer &) = delete;
  ControlPipeServer &operator=(const ControlPipeServer &) = delete;

  HANDLE command_event() const { return command_event_.get(); }

  template <typename Handler> void service_ready(Handler &&handler) {
    while (true) {
      std::shared_ptr<ControlRequest> request;
      {
        std::lock_guard lock(mutex_);
        if (requests_.empty()) {
          ResetEvent(command_event_.get());
          return;
        }
        request = requests_.front();
        requests_.pop_front();
      }

      std::string reply;
      if (request->command.empty()) {
        reply = "{\"OK\":false,\"error\":\"empty command\"}\n";
      } else {
        reply = handler(trim_command(request->command));
      }

      {
        std::lock_guard lock(request->mutex);
        request->reply = std::move(reply);
      }
      SetEvent(request->done_event.get());
    }
  }

  void stop() {
    if (stop_requested_.exchange(true)) {
      return;
    }
    SetEvent(stop_event_.get());
    complete_queued_requests("{\"OK\":false,\"error\":\"daemon stopping\"}\n");

    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  struct ControlRequest {
    std::string command;
    std::string reply;
    UniqueHandle done_event;
    std::mutex mutex;
  };

  void complete_queued_requests(std::string reply) {
    std::deque<std::shared_ptr<ControlRequest>> requests;
    {
      std::lock_guard lock(mutex_);
      requests.swap(requests_);
      ResetEvent(command_event_.get());
    }
    for (const auto &request : requests) {
      {
        std::lock_guard lock(request->mutex);
        request->reply = reply;
      }
      SetEvent(request->done_event.get());
    }
  }

  std::string queue_command(std::string command) {
    auto request = std::make_shared<ControlRequest>();
    request->command = std::move(command);
    request->done_event.reset(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!request->done_event) {
      return "{\"OK\":false,\"error\":\"failed to queue command\"}\n";
    }

    {
      std::lock_guard lock(mutex_);
      if (stop_requested_) {
        return "{\"OK\":false,\"error\":\"daemon stopping\"}\n";
      }
      requests_.push_back(request);
      SetEvent(command_event_.get());
    }

    HANDLE wait_handles[2] = {request->done_event.get(), stop_event_.get()};
    const DWORD wait_result =
        WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
      return "{\"OK\":false,\"error\":\"daemon stopping\"}\n";
    }

    std::lock_guard lock(request->mutex);
    return request->reply;
  }

  void thread_main() {
    while (!stop_requested_) {
      UniqueHandle pipe(CreateNamedPipeA(
          pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096,
          0, nullptr));
      if (!pipe) {
        logger_.log(vds::LogScope::Control, vds::LogLevel::Error,
                    "failed to create control pipe " + pipe_name_ + ": " +
                        win32_error_message(GetLastError()));
        WaitForSingleObject(stop_event_.get(), 1000);
        continue;
      }

      if (!connect_client(pipe.get()) || stop_requested_) {
        DisconnectNamedPipe(pipe.get());
        continue;
      }

      handle_client(pipe.get());
      DisconnectNamedPipe(pipe.get());
    }
  }

  bool wait_for_pipe_io(HANDLE pipe, OVERLAPPED &overlapped,
                        DWORD &bytes_transferred, std::string_view label) {
    HANDLE wait_handles[2] = {overlapped.hEvent, stop_event_.get()};
    const DWORD wait_result =
        WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe, &overlapped);
      (void)GetOverlappedResult(pipe, &overlapped, &bytes_transferred, TRUE);
      return false;
    }
    if (wait_result != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &overlapped);
      (void)GetOverlappedResult(pipe, &overlapped, &bytes_transferred, TRUE);
      if (!stop_requested_) {
        logger_.log(vds::LogScope::Control, vds::LogLevel::Warn,
                    std::string(label) +
                        " wait failed: " + win32_error_message(GetLastError()));
      }
      return false;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &bytes_transferred, FALSE)) {
      const DWORD error = GetLastError();
      if (!stop_requested_ && error != ERROR_NO_DATA &&
          error != ERROR_BROKEN_PIPE) {
        logger_.log(vds::LogScope::Control, vds::LogLevel::Warn,
                    std::string(label) +
                        " completion failed: " + win32_error_message(error));
      }
      return false;
    }
    return true;
  }

  bool connect_client(HANDLE pipe) {
    UniqueHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      logger_.log(vds::LogScope::Control, vds::LogLevel::Error,
                  "failed to create control pipe connect event: " +
                      win32_error_message(GetLastError()));
      return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) {
      return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
      return true;
    }
    if (error != ERROR_IO_PENDING) {
      if (!stop_requested_) {
        logger_.log(vds::LogScope::Control, vds::LogLevel::Warn,
                    "control pipe connect failed: " +
                        win32_error_message(error));
      }
      return false;
    }

    DWORD ignored = 0;
    return wait_for_pipe_io(pipe, overlapped, ignored, "control pipe connect");
  }

  std::optional<std::string> read_command(HANDLE pipe) {
    UniqueHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      logger_.log(vds::LogScope::Control, vds::LogLevel::Error,
                  "failed to create control pipe read event: " +
                      win32_error_message(GetLastError()));
      return std::nullopt;
    }

    std::array<char, 512> buffer{};
    DWORD got = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL started =
        ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1),
                 &got, &overlapped);
    if (!started) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        if (!stop_requested_ && error != ERROR_NO_DATA &&
            error != ERROR_BROKEN_PIPE) {
          logger_.log(vds::LogScope::Control, vds::LogLevel::Warn,
                      "control pipe read failed: " +
                          win32_error_message(error));
        }
        return std::nullopt;
      }
      if (!wait_for_pipe_io(pipe, overlapped, got, "control pipe read")) {
        return std::nullopt;
      }
    }
    if (got == 0) {
      return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(got));
  }

  bool write_reply(HANDLE pipe, const std::string &reply) {
    UniqueHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      logger_.log(vds::LogScope::Control, vds::LogLevel::Error,
                  "failed to create control pipe write event: " +
                      win32_error_message(GetLastError()));
      return false;
    }

    DWORD written = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL started =
        WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                  &written, &overlapped);
    if (!started) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) {
        if (!stop_requested_) {
          logger_.log(vds::LogScope::Control, vds::LogLevel::Warn,
                      "control pipe reply failed: " +
                          win32_error_message(error));
        }
        return false;
      }
      if (!wait_for_pipe_io(pipe, overlapped, written, "control pipe reply")) {
        return false;
      }
    }
    return true;
  }

  void handle_client(HANDLE pipe) {
    const auto command = read_command(pipe);
    if (!command) {
      return;
    }

    const std::string reply = queue_command(*command);
    (void)write_reply(pipe, reply);
  }

  vds::Logger &logger_;
  std::string pipe_name_;
  UniqueHandle stop_event_;
  UniqueHandle command_event_;
  std::thread thread_;
  std::atomic_bool stop_requested_ = false;
  std::mutex mutex_;
  std::deque<std::shared_ptr<ControlRequest>> requests_;
};

bool worker_is_connected(const BridgeWorker &worker) {
  return !worker.done.load() && !worker.stop_requested.load();
}

std::string handle_supervisor_control_command(
    const std::string &command,
    const std::vector<std::unique_ptr<BridgeWorker>> &workers,
    const std::string &db_path, std::uint32_t &trace_flags,
    bool &reload_requested, vds::Logger &logger) {
  std::vector<vds::VdsdControlControllerStatus> controller_statuses;
  controller_statuses.reserve(workers.size());
  for (const auto &worker : workers) {
    controller_statuses.push_back(vds::VdsdControlControllerStatus{
        .address = worker->address,
        .connected = worker_is_connected(*worker),
        .path = worker_is_connected(*worker)
                    ? vds::win::usbip::endpoint_for_port(worker->port)
                    : "",
    });
  }

  std::vector<vds::VdsdControlPortCandidate> port_candidates;
  port_candidates.reserve(vds::kMaxPortCount);
  for (unsigned port = 0; port < vds::kMaxPortCount; ++port) {
    port_candidates.push_back(vds::VdsdControlPortCandidate{
        .port = port,
        .path = vds::win::usbip::endpoint_for_port(port),
    });
  }

  std::vector<vds::VdsdControlPortBinding> port_bindings;
  port_bindings.reserve(workers.size());
  for (const auto &worker : workers) {
    if (!worker_is_connected(*worker)) {
      continue;
    }
    port_bindings.push_back(vds::VdsdControlPortBinding{
        .port = worker->port,
        .address = worker->address,
        .device_address = worker->device_address,
    });
  }

  const std::vector<vds::VdsdControlPortStatus> port_statuses =
      vds::build_vdsd_control_port_statuses(port_candidates, port_bindings);

  return vds::handle_vdsd_control_command(
      command, db_path, controller_statuses, port_statuses,
      [] { return list_windows_controller_targets(); }, trace_flags,
      reload_requested, logger);
}

void run_configured_bridge_worker(vds::ControllerConfig config,
                                  std::string device_address,
                                  vds::Logger &logger,
                                  std::atomic_bool &stop_requested,
                                  std::atomic_bool &done,
                                  std::string &last_error,
                                  const std::atomic_uint32_t &trace_flags) {
  try {
    if (config.ports.size() != 1) {
      throw std::runtime_error("bridge worker requires one reserved port");
    }
    const std::string virtual_device =
        vds::win::usbip::endpoint_for_port(config.ports[0]);
    run_bridge_session(device_address, virtual_device, &config, logger,
                       &stop_requested, &trace_flags);
    if (!g_stop_requested && !stop_requested.load()) {
      last_error = "bridge session ended";
      logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
                 "Windows bridge worker ended address=" + config.address +
                     ": " + last_error);
    }
  } catch (const std::exception &error) {
    if (!g_stop_requested && !stop_requested.load()) {
      last_error = error.what();
      logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                 "Windows bridge worker failed address=" + config.address +
                     ": " + error.what());
    }
  }
  done = true;
}

void run_bridge_supervisor(const Options &options, vds::Logger &logger) {
  std::vector<std::unique_ptr<BridgeWorker>> workers;
  std::vector<vds::VdsdWorkerFailureBackoff> worker_failure_backoffs;
  std::vector<std::string> discovery_warned_states;
  std::vector<std::string> no_port_warned_addresses;
  std::vector<std::string> port_binding_warned_states;
  ControlPipeServer control_pipe(options.pipe, logger);
  UniqueHandle retry_timer(CreateWaitableTimerExA(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_MODIFY_STATE | SYNCHRONIZE));
  if (!retry_timer) {
    retry_timer.reset(CreateWaitableTimerExA(nullptr, nullptr, 0,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE));
  }
  if (!retry_timer) {
    throw std::runtime_error("failed to create supervisor retry timer: " +
                             win32_error_message(GetLastError()));
  }
  std::atomic_uint32_t trace_flags = 0;
  bool reload_requested = false;
  bool scan_requested = true;
  bool config_load_logged = false;
  bool virtual_port_provider_warned = false;
  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "Windows bridge supervisor started pipe=" + options.pipe);

  enum class WaitKind {
    Stop,
    Control,
    Retry,
    WorkerDone,
  };
  struct WaitTarget {
    HANDLE handle = nullptr;
    WaitKind kind = WaitKind::Stop;
    std::size_t index = 0;
  };
  auto arm_retry_timer = [&](Clock::duration duration) {
    using HundredNanoseconds =
        std::chrono::duration<long long, std::ratio<1, 10000000>>;
    const auto ticks = std::max<long long>(
        1, std::chrono::duration_cast<HundredNanoseconds>(duration).count());
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -ticks;
    if (!SetWaitableTimerEx(retry_timer.get(), &due_time, 0, nullptr, nullptr,
                            nullptr, 0)) {
      throw std::runtime_error("failed to arm supervisor retry timer: " +
                               win32_error_message(GetLastError()));
    }
  };

  while (!g_stop_requested) {
    bool worker_removed = false;
    for (auto worker = workers.begin(); worker != workers.end();) {
      if ((*worker)->done.load()) {
        if ((*worker)->thread.joinable()) {
          (*worker)->thread.join();
        }
        if (!(*worker)->last_error.empty()) {
          vds::record_vdsd_worker_failure(
              worker_failure_backoffs, (*worker)->address,
              Clock::now() + kBridgeWorkerFailureRetryDelay);
        }
        worker = workers.erase(worker);
        worker_removed = true;
      } else {
        ++worker;
      }
    }
    if (worker_removed) {
      scan_requested = true;
    }

    if (reload_requested) {
      scan_requested = true;
    }

    if (std::any_of(worker_failure_backoffs.begin(),
                    worker_failure_backoffs.end(), [](const auto &backoff) {
                      return Clock::now() >= backoff.retry_after;
                    })) {
      scan_requested = true;
    }

    if (scan_requested) {
      scan_requested = false;
      try {
        const vds::ConfigDb db = load_config_db_for_attempt(options, logger);
        if (!config_load_logged || reload_requested) {
          logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
                     "loaded controller config count=" +
                         std::to_string(db.controllers.size()));
          config_load_logged = true;
        }
        const std::vector<unsigned> enabled_ports = enabled_virtual_ports();
        const bool virtual_port_provider_is_available =
            !enabled_ports.empty() && vds::win::usbip::client_available() &&
            vds::win::hidhide::provider_available();
        if (!virtual_port_provider_is_available) {
          if (!virtual_port_provider_warned) {
            std::string detail;
            if (!vds::win::usbip::client_available()) {
              detail = kWindowsVirtualPortProviderUnavailable;
            } else {
              detail = kWindowsHidHideProviderUnavailable;
            }
            logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                       std::string(kVirtualPortProviderUnavailableReason) +
                           " detail=" + detail);
            virtual_port_provider_warned = true;
          }
        } else {
          if (virtual_port_provider_warned) {
            logger.log(vds::LogScope::Port, vds::LogLevel::Info,
                       "virtual port provider recovered");
            virtual_port_provider_warned = false;
          }
        }

        const std::vector<HidBluetoothDevice> hid_devices =
            list_hid_bluetooth_devices();

        for (const auto &worker : workers) {
          const bool stale = vds::vdsd_worker_config_is_stale(
              db, worker->address, worker->profile, worker->port,
              virtual_port_is_enabled(worker->port));
          if (stale && !worker->stop_requested.exchange(true)) {
            logger.log(
                vds::LogScope::Daemon, vds::LogLevel::Info,
                "Windows bridge worker retiring address=" + worker->address +
                    " port=" + std::to_string(worker->port));
          }
        }

        std::vector<unsigned> reserved_ports;
        reserved_ports.reserve(workers.size());
        std::vector<std::string> reserved_device_addresses;
        reserved_device_addresses.reserve(workers.size());
        for (const auto &worker : workers) {
          reserved_ports.push_back(worker->port);
          reserved_device_addresses.push_back(worker->device_address);
        }

        for (const auto &config : db.controllers) {
          if (!vds::consume_vdsd_worker_retry(worker_failure_backoffs,
                                              config.address, Clock::now())) {
            continue;
          }

          const auto existing = std::find_if(
              workers.begin(), workers.end(), [&](const auto &worker) {
                return worker->address == config.address;
              });
          if (existing != workers.end()) {
            continue;
          }
          if (!virtual_port_provider_is_available) {
            if (vds::remember_vdsd_warning_state(
                    port_binding_warned_states, config.address,
                    kVirtualPortProviderUnavailableReason)) {
              logger.log(
                  vds::LogScope::Daemon, vds::LogLevel::Warn,
                  "controller binding unavailable address=" + config.address +
                      " reason=" + kVirtualPortProviderUnavailableReason);
            }
            continue;
          }
          const std::string port_binding_error =
              "no allowed virtual port enabled ports=[" +
              vds::format_ports(config.ports) + "]";
          if (!controller_has_enabled_virtual_port(config, enabled_ports)) {
            if (vds::remember_vdsd_warning_state(port_binding_warned_states,
                                                 config.address,
                                                 port_binding_error)) {
              logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
                         "controller binding unavailable address=" +
                             config.address + " reason=" + port_binding_error);
            }
            continue;
          }
          vds::forget_vdsd_warning_state(port_binding_warned_states,
                                         config.address);
          std::string discovery_error;
          const auto connected_device = find_connected_bluetooth_device(
              config.address, hid_devices, &discovery_error);
          if (!connected_device) {
            if (vds::remember_vdsd_warning_state(
                    discovery_warned_states, config.address, discovery_error)) {
              logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
                         "controller HID unavailable address=" +
                             config.address + " reason=" + discovery_error);
            }
            continue;
          }
          vds::forget_vdsd_warning_state(discovery_warned_states,
                                         config.address);
          const vds::VdsdWorkerLaunchDecision launch =
              vds::select_vdsd_worker_launch_decision(
                  config, available_virtual_ports(), reserved_ports,
                  config.address, virtual_port_provider_is_available,
                  reserved_device_addresses);
          if (launch.status ==
              vds::VdsdWorkerLaunchStatus::VirtualPortProviderUnavailable) {
            if (vds::remember_vdsd_warning_state(
                    port_binding_warned_states, config.address,
                    kVirtualPortProviderUnavailableReason)) {
              logger.log(
                  vds::LogScope::Daemon, vds::LogLevel::Warn,
                  "rejected HID device address=" + config.address +
                      " reason=" + kVirtualPortProviderUnavailableReason);
            }
            continue;
          }
          if (launch.status ==
              vds::VdsdWorkerLaunchStatus::DeviceAddressReserved) {
            continue;
          }
          if (launch.status == vds::VdsdWorkerLaunchStatus::NoAvailablePort) {
            if (vds::remember_vdsd_warning(no_port_warned_addresses,
                                           config.address)) {
              logger.log(vds::LogScope::Daemon, vds::LogLevel::Warn,
                         "rejected HID device address=" + config.address +
                             " reason=no available virtual port");
            }
            continue;
          }
          vds::forget_vdsd_warning(no_port_warned_addresses, config.address);

          auto worker = std::make_unique<BridgeWorker>();
          worker->address = config.address;
          worker->device_address = launch.device_address;
          worker->port = launch.port;
          worker->profile = config.profile;
          worker->done_event.reset(CreateEventA(nullptr, TRUE, FALSE, nullptr));
          if (!worker->done_event) {
            throw std::runtime_error("failed to create worker event: " +
                                     win32_error_message(GetLastError()));
          }
          BridgeWorker *worker_ptr = worker.get();
          worker->thread = std::thread([worker_config = launch.config,
                                        device_address = launch.device_address,
                                        &logger, worker_ptr, &trace_flags] {
            run_configured_bridge_worker(worker_config, device_address, logger,
                                         worker_ptr->stop_requested,
                                         worker_ptr->done,
                                         worker_ptr->last_error, trace_flags);
            SetEvent(worker_ptr->done_event.get());
          });
          logger.log(
              vds::LogScope::Daemon, vds::LogLevel::Info,
              "Windows bridge worker launched address=" + config.address +
                  " port=" + std::to_string(launch.port) +
                  " device_address=" + launch.device_address);
          workers.push_back(std::move(worker));
          reserved_ports.push_back(launch.port);
          reserved_device_addresses.push_back(launch.device_address);
        }
      } catch (const std::exception &error) {
        logger.log(vds::LogScope::Daemon, vds::LogLevel::Error,
                   std::string("Windows bridge supervisor scan failed: ") +
                       error.what());
        scan_requested = true;
      }
    }

    if (reload_requested) {
      reload_requested = false;
    }

    const auto now = Clock::now();
    std::optional<Clock::time_point> retry_after = now + kBridgeOpenRetryDelay;
    if (!worker_failure_backoffs.empty()) {
      const auto next_retry = std::min_element(
          worker_failure_backoffs.begin(), worker_failure_backoffs.end(),
          [](const auto &left, const auto &right) {
            return left.retry_after < right.retry_after;
          });
      if (next_retry != worker_failure_backoffs.end()) {
        if (next_retry->retry_after <= now) {
          scan_requested = true;
          continue;
        }
        retry_after =
            retry_after
                ? std::min(*retry_after, next_retry->retry_after)
                : std::optional<Clock::time_point>(next_retry->retry_after);
      }
    }

    bool retry_timer_armed = false;
    if (retry_after) {
      arm_retry_timer(*retry_after - now);
      retry_timer_armed = true;
    } else {
      CancelWaitableTimer(retry_timer.get());
    }

    std::vector<WaitTarget> wait_targets;
    wait_targets.reserve(workers.size() + 3);
    if (g_stop_event != nullptr) {
      wait_targets.push_back(WaitTarget{
          .handle = g_stop_event,
          .kind = WaitKind::Stop,
          .index = 0,
      });
    }
    wait_targets.push_back(WaitTarget{
        .handle = control_pipe.command_event(),
        .kind = WaitKind::Control,
        .index = 0,
    });
    if (retry_timer_armed) {
      wait_targets.push_back(WaitTarget{
          .handle = retry_timer.get(),
          .kind = WaitKind::Retry,
          .index = 0,
      });
    }
    for (std::size_t index = 0; index < workers.size(); ++index) {
      if (!workers[index]->done_event) {
        continue;
      }
      wait_targets.push_back(WaitTarget{
          .handle = workers[index]->done_event.get(),
          .kind = WaitKind::WorkerDone,
          .index = index,
      });
    }

    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(wait_targets.size());
    for (const auto &target : wait_targets) {
      wait_handles.push_back(target.handle);
    }

    const DWORD wait_result =
        WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()),
                               wait_handles.data(), FALSE, INFINITE);
    if (wait_result == WAIT_FAILED) {
      throw std::runtime_error("Windows bridge supervisor wait failed: " +
                               win32_error_message(GetLastError()));
    }

    const DWORD wait_index = wait_result - WAIT_OBJECT_0;
    if (wait_index >= wait_targets.size()) {
      throw std::runtime_error("Windows bridge supervisor wait returned an "
                               "unexpected object index");
    }

    switch (wait_targets[wait_index].kind) {
    case WaitKind::Stop:
      g_stop_requested = true;
      break;
    case WaitKind::Control:
      control_pipe.service_ready([&](const std::string &command) {
        std::uint32_t current_trace_flags = trace_flags.load();
        const std::string reply = handle_supervisor_control_command(
            command, workers, options.db_path, current_trace_flags,
            reload_requested, logger);
        trace_flags.store(current_trace_flags);
        return reply;
      });
      if (reload_requested) {
        scan_requested = true;
      }
      break;
    case WaitKind::Retry:
      CancelWaitableTimer(retry_timer.get());
      scan_requested = true;
      break;
    case WaitKind::WorkerDone:
      scan_requested = true;
      break;
    }
  }

  for (auto &worker : workers) {
    if (worker->thread.joinable()) {
      worker->thread.join();
    }
  }
  logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
             "Windows bridge supervisor stopped");
}

} // namespace

int run_windows_daemon(int argc, char **argv, bool service_mode) {
  try {
    g_stop_requested = false;
    const Options options = parse_platform_args(argc, argv);
    UniqueHandle stop_event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event) {
      throw std::runtime_error("failed to create stop event: " +
                               win32_error_message(GetLastError()));
    }
    g_stop_event = stop_event.get();
    if (!service_mode) {
      SetConsoleCtrlHandler(console_handler, TRUE);
    }

    vds::Logger logger(options.log_path);
    TimerResolutionGuard timer_resolution(kWindowsTimerResolutionMs);
    logger.log(vds::LogScope::Output,
               timer_resolution.active() ? vds::LogLevel::Info
                                         : vds::LogLevel::Warn,
               std::string("Windows timer resolution 1 ms ") +
                   (timer_resolution.active() ? "enabled" : "request failed"));
    if (!virtual_port_provider_available()) {
      logger.log(vds::LogScope::Port, vds::LogLevel::Error,
                 std::string(kVirtualPortProviderUnavailableReason) +
                     " detail=" + kWindowsVirtualPortProviderUnavailable);
      throw std::runtime_error(kWindowsVirtualPortProviderUnavailable);
    }
    if (!vds::win::hidhide::provider_available()) {
      logger.log(vds::LogScope::Bluetooth, vds::LogLevel::Error,
                 std::string(kVirtualPortProviderUnavailableReason) +
                     " detail=" + kWindowsHidHideProviderUnavailable);
      throw std::runtime_error(kWindowsHidHideProviderUnavailable);
    }
    vds::win::hidhide::register_daemon();
    if (service_mode) {
      update_service_status(SERVICE_RUNNING);
      logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
                 "Windows service started");
    }

    run_bridge_supervisor(options, logger);
    g_stop_event = nullptr;
    logger.log(vds::LogScope::Daemon, vds::LogLevel::Info,
               "Windows bridge stopped");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}

void WINAPI service_main(DWORD argc, LPSTR *argv) {
  g_service_status_handle = RegisterServiceCtrlHandlerExA(
      kServiceName, service_control_handler, nullptr);
  if (g_service_status_handle == nullptr) {
    g_service_exit_code = GetLastError();
    return;
  }

  update_service_status(SERVICE_START_PENDING, ERROR_SUCCESS, 5000);
  g_service_exit_code = static_cast<DWORD>(
      run_windows_daemon(static_cast<int>(argc), argv, true));
  update_service_status(SERVICE_STOPPED, g_service_exit_code == 0
                                             ? ERROR_SUCCESS
                                             : ERROR_SERVICE_SPECIFIC_ERROR);
}

bool dispatch_service_if_needed() {
  SERVICE_TABLE_ENTRYA service_table[] = {
      {const_cast<LPSTR>(kServiceName), service_main},
      {nullptr, nullptr},
  };
  if (StartServiceCtrlDispatcherA(service_table)) {
    return true;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
    return false;
  }
  throw std::runtime_error("failed to connect service dispatcher: " +
                           win32_error_message(error));
}

int main(int argc, char **argv) {
  try {
    if (dispatch_service_if_needed()) {
      return static_cast<int>(g_service_exit_code);
    }
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  return run_windows_daemon(argc, argv, false);
}
