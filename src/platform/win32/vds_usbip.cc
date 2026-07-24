// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
// Copyright (C) 2026 Antonio <bleader93@gmail.com>

#include "vds_usbip.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <timeapi.h>

#include "unique_handle.hh"
#include "vds/ds5_usb.h"
#include "vds_io.hh"
#include "vds_log.hh"
#include "vds_win32.hh"

namespace vds::win::usbip {

namespace {

constexpr std::uint16_t kOpReqDevlist = 0x8005;
constexpr std::uint16_t kOpRepDevlist = 0x0005;
constexpr std::uint16_t kOpReqImport = 0x8003;
constexpr std::uint16_t kOpRepImport = 0x0003;

constexpr std::uint32_t kCmdSubmit = 0x00000001;
constexpr std::uint32_t kRetSubmit = 0x00000003;
constexpr std::uint32_t kCmdUnlink = 0x00000002;
constexpr std::uint32_t kRetUnlink = 0x00000004;

constexpr std::uint32_t kDirOut = 0;
constexpr std::uint32_t kDirIn = 1;

constexpr DWORD kClientProcessTimeoutMs = 10000;
constexpr DWORD kClientAttachTimeoutMs = 10000;
constexpr DWORD kClientAttachPollMs = 25;
constexpr std::string_view kUsbipClientRelativePath = R"(USBip\usbip.exe)";
constexpr std::string_view kPathPrefix = "/vds/virtual/dualsense";
constexpr std::size_t kMaxTransferBufferBytes = 1024 * 1024;
constexpr std::int32_t kMaxIsoPackets = 1024;

constexpr std::size_t kMicBytesPerMs = 192;
constexpr std::size_t kMicPrimeBytes = 40 * kMicBytesPerMs;
constexpr std::size_t kMicMaxBufferBytes = 120 * kMicBytesPerMs;

std::string busid_for_port(unsigned port_index) {
  if (port_index >= VDS_MAX_PORT_COUNT) {
    throw std::runtime_error("USB/IP port must be in range 0.." +
                             std::to_string(VDS_MAX_PORT_COUNT - 1));
  }
  return "1-" + std::to_string(port_index + 1);
}

std::string endpoint_for_busid(std::string_view busid) {
  return "usbip://127.0.0.1:" + std::to_string(kDefaultPort) + "/" +
         std::string(busid);
}

std::uint16_t hton16(std::uint16_t v) { return htons(v); }
std::uint32_t hton32(std::uint32_t v) { return htonl(v); }
std::uint16_t ntoh16(std::uint16_t v) { return ntohs(v); }
std::uint32_t ntoh32(std::uint32_t v) { return ntohl(v); }

#pragma pack(push, 1)
struct OpCommonHeader {
  std::uint16_t version;
  std::uint16_t code;
  std::uint32_t status;
};

// usbip_usb_device, as sent in OP_REP_IMPORT / OP_REP_DEVLIST.
struct WireUsbDevice {
  char path[256];
  char busid[32];
  std::uint32_t busnum;
  std::uint32_t devnum;
  std::uint32_t speed; // 3 = USB_SPEED_HIGH
  std::uint16_t idVendor;
  std::uint16_t idProduct;
  std::uint16_t bcdDevice;
  std::uint8_t bDeviceClass;
  std::uint8_t bDeviceSubClass;
  std::uint8_t bDeviceProtocol;
  std::uint8_t bConfigurationValue;
  std::uint8_t bNumConfigurations;
  std::uint8_t bNumInterfaces;
};

struct WireUsbInterface {
  std::uint8_t bInterfaceClass;
  std::uint8_t bInterfaceSubClass;
  std::uint8_t bInterfaceProtocol;
  std::uint8_t padding;
};

// Common header for USBIP_CMD_SUBMIT / RET_SUBMIT / CMD_UNLINK / RET_UNLINK.
struct UsbipHeaderBasic {
  std::uint32_t command;
  std::uint32_t seqnum;
  std::uint32_t devid;
  std::uint32_t direction;
  std::uint32_t ep;
};

struct UsbipCmdSubmit {
  UsbipHeaderBasic base;
  std::uint32_t transfer_flags;
  std::int32_t transfer_buffer_length;
  std::int32_t start_frame;
  std::int32_t number_of_packets;
  std::int32_t interval;
  std::uint8_t setup[8];
};

struct UsbipRetSubmit {
  UsbipHeaderBasic base;
  std::int32_t status;
  std::int32_t actual_length;
  std::int32_t start_frame;
  std::int32_t number_of_packets;
  std::int32_t error_count;
  std::uint8_t padding[8];
};

struct UsbipIsoPacketDescriptor {
  std::uint32_t offset;
  std::uint32_t length;
  std::int32_t actual_length;
  std::int32_t status;
};

struct UsbipCmdUnlink {
  UsbipHeaderBasic base;
  std::uint32_t unlink_seqnum;
  std::uint8_t padding[24];
};

struct UsbipRetUnlink {
  UsbipHeaderBasic base;
  std::int32_t status;
  std::uint8_t padding[24];
};
#pragma pack(pop)

// Endpoint numbers are sourced directly from the shared descriptor table.
constexpr std::uint8_t kEpAudioOut = VDS_USB_AUDIO_OUT_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpAudioIn = VDS_USB_AUDIO_IN_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpHidIn = VDS_USB_HID_IN_ENDPOINT & 0x0f;
constexpr std::uint8_t kEpHidOut = VDS_USB_HID_OUT_ENDPOINT & 0x0f;

struct WinsockGuard {
  WinsockGuard() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }
  ~WinsockGuard() { WSACleanup(); }
};

class Socket {
public:
  explicit Socket(SOCKET s = INVALID_SOCKET) : s_(s) {}
  ~Socket() { reset(); }
  Socket(Socket &&other) noexcept : s_(other.s_) { other.s_ = INVALID_SOCKET; }
  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      reset();
      s_ = other.s_;
      other.s_ = INVALID_SOCKET;
    }
    return *this;
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  SOCKET get() const { return s_; }
  bool valid() const { return s_ != INVALID_SOCKET; }
  void reset(SOCKET s = INVALID_SOCKET) {
    if (s_ != INVALID_SOCKET) {
      closesocket(s_);
    }
    s_ = s;
  }

private:
  SOCKET s_;
};

bool recv_exact(SOCKET s, void *buffer, std::size_t size) {
  auto *data = static_cast<char *>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const int got = recv(s, data, static_cast<int>(remaining), 0);
    if (got <= 0) {
      return false;
    }
    data += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

bool send_exact(SOCKET s, const void *buffer, std::size_t size) {
  const auto *data = static_cast<const char *>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const int sent = send(s, data, static_cast<int>(remaining), 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return true;
}

std::wstring utf16_from_utf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }

  const int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    throw std::runtime_error("failed to convert USB/IP argument to UTF-16");
  }

  std::wstring converted(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), converted.data(),
                          required) != required) {
    throw std::runtime_error("failed to convert USB/IP argument to UTF-16");
  }
  return converted;
}

std::string utf8_from_utf16(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    throw std::runtime_error("failed to convert USB/IP path to UTF-8");
  }

  std::string converted(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), converted.data(),
                          required, nullptr, nullptr) != required) {
    throw std::runtime_error("failed to convert USB/IP path to UTF-8");
  }
  return converted;
}

std::wstring environment_value(const wchar_t *name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) {
    return {};
  }

  std::wstring value(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
  if (written == 0 || written >= required) {
    return {};
  }
  value.resize(written);
  return value;
}

std::wstring resolve_client_path() {
  std::wstring path = environment_value(L"VDS_USBIP_EXE");
  if (!path.empty()) {
    return path;
  }

  path = environment_value(L"ProgramW6432");
  if (path.empty()) {
    path = environment_value(L"ProgramFiles");
  }
  if (path.empty()) {
    return {};
  }
  if (path.back() != L'\\') {
    path.push_back(L'\\');
  }
  path += utf16_from_utf8(kUsbipClientRelativePath);
  return path;
}

bool regular_file_exists(const std::wstring &path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring quote_argument(std::wstring_view argument) {
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::wstring
client_command_line(std::initializer_list<std::wstring> arguments) {
  const std::wstring executable = resolve_client_path();
  std::wstring command_line = quote_argument(executable);
  for (const std::wstring &argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_argument(argument);
  }
  return command_line;
}

struct ClientProcess {
  UniqueHandle process;
  UniqueHandle thread;
};

ClientProcess
start_client_process(std::initializer_list<std::wstring> arguments) {
  const std::wstring executable = resolve_client_path();
  if (!regular_file_exists(executable)) {
    throw std::runtime_error("usbip.exe is unavailable");
  }

  std::wstring command_line = client_command_line(arguments);
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    throw std::runtime_error("failed to start usbip.exe: " +
                             win32_error_message(GetLastError()));
  }
  return {
      .process = UniqueHandle(process_info.hProcess),
      .thread = UniqueHandle(process_info.hThread),
  };
}

struct ClientProcessResult {
  DWORD exit_code = ERROR_GEN_FAILURE;
  std::string output;
};

ClientProcessResult
run_client_process(std::initializer_list<std::wstring> arguments) {
  SECURITY_ATTRIBUTES security_attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
  };
  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &security_attributes, 0)) {
    throw std::runtime_error("failed to create usbip.exe output pipe: " +
                             win32_error_message(GetLastError()));
  }
  UniqueHandle output_read(read_handle);
  UniqueHandle output_write(write_handle);
  if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    throw std::runtime_error("failed to configure usbip.exe output pipe: " +
                             win32_error_message(GetLastError()));
  }

  const std::wstring executable = resolve_client_path();
  if (!regular_file_exists(executable)) {
    throw std::runtime_error("usbip.exe is unavailable");
  }
  std::wstring command_line = client_command_line(arguments);
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = output_write.get();
  startup_info.hStdError = output_write.get();
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    throw std::runtime_error("failed to start usbip.exe: " +
                             win32_error_message(GetLastError()));
  }

  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);
  output_write.reset();

  ClientProcessResult result;
  std::thread output_reader;
  try {
    output_reader = std::thread([&] {
      std::array<char, 4096> buffer{};
      for (;;) {
        DWORD read = 0;
        if (!ReadFile(output_read.get(), buffer.data(),
                      static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
          return;
        }
        result.output.append(buffer.data(), read);
      }
    });
  } catch (...) {
    TerminateProcess(process.get(), ERROR_NOT_ENOUGH_MEMORY);
    WaitForSingleObject(process.get(), INFINITE);
    throw;
  }

  const DWORD wait_result =
      WaitForSingleObject(process.get(), kClientProcessTimeoutMs);
  if (wait_result != WAIT_OBJECT_0) {
    const DWORD wait_error =
        wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    TerminateProcess(process.get(), wait_result == WAIT_TIMEOUT
                                        ? ERROR_TIMEOUT
                                        : ERROR_OPERATION_ABORTED);
    WaitForSingleObject(process.get(), INFINITE);
    output_reader.join();
    if (wait_result == WAIT_TIMEOUT) {
      throw std::runtime_error("usbip.exe timed out");
    }
    throw std::runtime_error("usbip.exe wait failed: " +
                             win32_error_message(wait_error));
  }
  output_reader.join();

  if (!GetExitCodeProcess(process.get(), &result.exit_code)) {
    throw std::runtime_error("failed to read usbip.exe exit code: " +
                             win32_error_message(GetLastError()));
  }
  return result;
}

std::optional<unsigned> attached_client_port(std::string_view busid) {
  const ClientProcessResult result = run_client_process({L"port"});
  if (result.exit_code != ERROR_SUCCESS) {
    return std::nullopt;
  }

  std::optional<unsigned> current_port;
  std::size_t line_begin = 0;
  while (line_begin < result.output.size()) {
    const std::size_t line_end = result.output.find('\n', line_begin);
    std::string_view line(
        result.output.data() + line_begin,
        (line_end == std::string::npos ? result.output.size() : line_end) -
            line_begin);
    if (line.rfind("Port ", 0) == 0) {
      unsigned port = 0;
      std::size_t index = 5;
      bool parsed = false;
      while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
        parsed = true;
        port = port * 10 + static_cast<unsigned>(line[index] - '0');
        ++index;
      }
      current_port = parsed ? std::optional<unsigned>(port) : std::nullopt;
    }

    const std::string endpoint = endpoint_for_busid(busid);
    if (current_port && line.find(endpoint) != std::string_view::npos) {
      return current_port;
    }
    if (line_end == std::string::npos) {
      break;
    }
    line_begin = line_end + 1;
  }
  return std::nullopt;
}

void detach_client(std::string_view busid, Logger &logger) {
  const std::optional<unsigned> port = attached_client_port(busid);
  if (!port) {
    return;
  }

  const ClientProcessResult result =
      run_client_process({L"detach", L"-p", std::to_wstring(*port)});
  if (result.exit_code != ERROR_SUCCESS) {
    throw std::runtime_error("usbip.exe detach failed with exit code " +
                             std::to_string(result.exit_code));
  }
  try {
    logger.log(LogScope::Usb, LogLevel::Info,
               "USB/IP client detached busid=" + std::string(busid) +
                   " port=" + std::to_string(*port));
  } catch (...) {
  }
}

// Picks the correct descriptor tables/endpoint-interval info for the
// configured profile (DS5 vs DSE). All byte content comes straight from
// include/vds/ds5_usb.h -- this module does not fabricate any descriptor
// bytes of its own.
struct ProfileDescriptors {
  const std::uint8_t *device;
  std::size_t device_len;
  const std::uint8_t *config;
  std::size_t config_len;
  const std::uint8_t *hid;
  std::size_t hid_len;
  const std::uint8_t *hid_report;
  std::size_t hid_report_len;
  std::uint16_t idVendor;
  std::uint16_t idProduct;
};

// Builds a USB STRING descriptor (bLength, bDescriptorType=0x03, then
// UTF-16LE code units -- ASCII input only, which covers all vDS string
// constants) for the given ASCII text. Returns the raw descriptor bytes.
std::vector<std::uint8_t> build_string_descriptor(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(2 + text.size() * 2);
  bytes.push_back(0);    // bLength, patched below
  bytes.push_back(0x03); // bDescriptorType = STRING
  for (const char c : text) {
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(0x00);
  }
  bytes[0] = static_cast<std::uint8_t>(bytes.size());
  return bytes;
}

// String descriptor index 0 is always the supported-languages list, not
// text: bLength=4, bDescriptorType=0x03, wLANGID[0] = 0x0409 (English/US).
std::vector<std::uint8_t> build_langid_descriptor() {
  return {0x04, 0x03, 0x09, 0x04};
}

ProfileDescriptors descriptors_for_profile(std::uint32_t profile) {
  if (profile == VDS_PROFILE_DSE) {
    return ProfileDescriptors{
        vds_dse_usb_device_descriptor,
        sizeof(vds_dse_usb_device_descriptor),
        vds_dse_usb_configuration_descriptor,
        sizeof(vds_dse_usb_configuration_descriptor),
        vds_dse_usb_hid_descriptor,
        sizeof(vds_dse_usb_hid_descriptor),
        vds_dse_usb_hid_report_descriptor,
        sizeof(vds_dse_usb_hid_report_descriptor),
        0x054c,
        0x0df2,
    };
  }
  return ProfileDescriptors{
      vds_ds5_usb_device_descriptor,
      sizeof(vds_ds5_usb_device_descriptor),
      vds_ds5_usb_configuration_descriptor,
      sizeof(vds_ds5_usb_configuration_descriptor),
      vds_ds5_usb_hid_descriptor,
      sizeof(vds_ds5_usb_hid_descriptor),
      vds_ds5_usb_hid_report_descriptor,
      sizeof(vds_ds5_usb_hid_report_descriptor),
      0x054c,
      0x0ce6,
  };
}

} // namespace

struct VirtualPort::Impl : public std::enable_shared_from_this<Impl> {
  struct Server;

  Impl(std::uint32_t profile_in, unsigned port_index_in, Logger &logger_in)
      : profile(profile_in), port_index(port_index_in), logger(logger_in),
        busid(busid_for_port(port_index)),
        device_path(std::string(kPathPrefix) + std::to_string(port_index)) {}

  std::uint32_t profile;
  unsigned port_index;
  Logger &logger;
  std::string busid;
  std::string device_path;
  std::string pipe_path;

  UniqueHandle pipe_server;
  std::atomic_bool stop_requested{false};
  std::atomic_bool attached{false};
  bool registered = false;

  std::atomic<SOCKET> client_socket{INVALID_SOCKET};
  std::atomic_bool reader_exited{false};
  std::mutex session_mutex;
  std::condition_variable session_cv;
  bool session_active = false;

  void start();
  void stop();
  bool begin_import_session(SOCKET client);
  void finish_import_session();
  void handle_import_session(SOCKET client);
  bool handle_cmd_submit(
      SOCKET client, const UsbipCmdSubmit &cmd,
      std::span<const std::uint8_t> out_data,
      const std::vector<UsbipIsoPacketDescriptor> &iso_descriptors);

  Frame read_daemon_frame(HANDLE pipe);
  void write_daemon_frame(HANDLE pipe, std::uint16_t type,
                          std::span<const std::uint8_t> payload);

  std::thread pipe_reader_thread;
  std::mutex demux_mutex;
  std::condition_variable demux_cv;
  std::deque<Frame> hid_in_queue;
  std::deque<Frame> feature_reply_queue;
  std::vector<std::uint8_t> mic_ring_buffer;
  bool mic_primed = false;

  void pipe_reader_loop();
  bool wait_for_hid_in_frame(Frame &out, DWORD timeout_ms);
  bool wait_for_feature_reply(Frame &out, DWORD timeout_ms);
  std::vector<std::uint8_t> take_mic_bytes(std::size_t count);

  struct PacedReply {
    SOCKET client;
    std::vector<std::uint8_t> bytes;
  };
  std::thread iso_pacer_thread;
  std::mutex pacer_mutex;
  std::condition_variable pacer_cv;
  std::multimap<std::chrono::steady_clock::time_point, PacedReply>
      paced_replies;
  std::chrono::steady_clock::time_point iso_out_next_due{};
  std::chrono::steady_clock::time_point iso_in_next_due{};
  std::mutex socket_send_mutex;
  void iso_pacer_loop();

  std::atomic<int> speaker_gain_q15{1 << 15};
  std::atomic<int> fu_volume_db256{0};
  std::atomic<bool> fu_muted{false};
  void update_speaker_gain();
};

struct VirtualPort::Impl::Server {
  static Server &instance() {
    static Server server;
    return server;
  }

  void register_port(const std::shared_ptr<Impl> &port) {
    std::lock_guard lock(ports_mutex);
    const auto existing = ports.find(port->port_index);
    if (existing != ports.end() && !existing->second.expired()) {
      throw std::runtime_error("USB/IP virtual port is already active");
    }
    ports[port->port_index] = port;
  }

  void unregister_port(unsigned port_index) {
    std::lock_guard lock(ports_mutex);
    ports.erase(port_index);
  }

private:
  Server() : listener_thread([this] { listener_loop(); }) {
    std::unique_lock lock(ready_mutex);
    ready_cv.wait(lock, [this] { return ready; });
    if (!startup_error.empty()) {
      lock.unlock();
      if (listener_thread.joinable()) {
        listener_thread.join();
      }
      throw std::runtime_error(startup_error);
    }
  }

  ~Server() {
    stop_requested = true;
    const SOCKET poke = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (poke != INVALID_SOCKET) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = inet_addr("127.0.0.1");
      address.sin_port = hton16(kDefaultPort);
      connect(poke, reinterpret_cast<const sockaddr *>(&address),
              sizeof(address));
      closesocket(poke);
    }
    if (listener_thread.joinable()) {
      listener_thread.join();
    }
  }

  std::vector<std::shared_ptr<Impl>> active_ports() {
    std::vector<std::shared_ptr<Impl>> result;
    std::lock_guard lock(ports_mutex);
    for (auto port = ports.begin(); port != ports.end();) {
      if (std::shared_ptr<Impl> active = port->second.lock()) {
        result.push_back(std::move(active));
        ++port;
      } else {
        port = ports.erase(port);
      }
    }
    return result;
  }

  std::shared_ptr<Impl> find_port(std::string_view busid) {
    const std::vector<std::shared_ptr<Impl>> snapshot = active_ports();
    const auto match =
        std::find_if(snapshot.begin(), snapshot.end(), [&](const auto &port) {
          return port->busid == busid && !port->stop_requested.load();
        });
    return match == snapshot.end() ? nullptr : *match;
  }

  WireUsbDevice wire_device(const Impl &port) {
    const ProfileDescriptors descriptors =
        descriptors_for_profile(port.profile);
    WireUsbDevice device{};
    std::strncpy(device.path, port.device_path.c_str(),
                 sizeof(device.path) - 1);
    std::strncpy(device.busid, port.busid.c_str(), sizeof(device.busid) - 1);
    device.busnum = hton32(1);
    device.devnum = hton32(port.port_index + 1);
    device.speed = hton32(3);
    device.idVendor = hton16(descriptors.idVendor);
    device.idProduct = hton16(descriptors.idProduct);
    device.bcdDevice = hton16(VDS_USB_DEVICE_BCD);
    device.bDeviceClass = descriptors.device[4];
    device.bDeviceSubClass = descriptors.device[5];
    device.bDeviceProtocol = descriptors.device[6];
    device.bConfigurationValue = VDS_USB_CONFIGURATION_VALUE;
    device.bNumConfigurations = VDS_USB_NUM_CONFIGURATIONS;
    device.bNumInterfaces = 4;
    return device;
  }

  bool send_device_list(SOCKET client) {
    const std::vector<std::shared_ptr<Impl>> snapshot = active_ports();
    OpCommonHeader reply{
        hton16(kProtocolVersion),
        hton16(kOpRepDevlist),
        0,
    };
    const std::uint32_t count =
        hton32(static_cast<std::uint32_t>(snapshot.size()));
    if (!send_exact(client, &reply, sizeof(reply)) ||
        !send_exact(client, &count, sizeof(count))) {
      return false;
    }

    constexpr WireUsbInterface interfaces[4] = {
        {0x01, 0x01, 0x00, 0},
        {0x01, 0x02, 0x00, 0},
        {0x01, 0x02, 0x00, 0},
        {0x03, 0x00, 0x00, 0},
    };
    for (const auto &port : snapshot) {
      const WireUsbDevice device = wire_device(*port);
      if (!send_exact(client, &device, sizeof(device)) ||
          !send_exact(client, interfaces, sizeof(interfaces))) {
        return false;
      }
    }
    return true;
  }

  void handle_client(Socket client) {
    OpCommonHeader header{};
    if (!recv_exact(client.get(), &header, sizeof(header))) {
      return;
    }

    if (ntoh16(header.version) != kProtocolVersion ||
        ntoh32(header.status) != 0) {
      return;
    }
    const std::uint16_t code = ntoh16(header.code);
    if (code == kOpReqDevlist) {
      (void)send_device_list(client.get());
      return;
    }
    if (code != kOpReqImport) {
      return;
    }

    std::array<char, 32> requested_busid{};
    if (!recv_exact(client.get(), requested_busid.data(),
                    requested_busid.size())) {
      return;
    }
    const std::size_t length =
        std::find(requested_busid.begin(), requested_busid.end(), '\0') -
        requested_busid.begin();
    std::shared_ptr<Impl> port =
        find_port(std::string_view(requested_busid.data(), length));
    if (!port || !port->begin_import_session(client.get())) {
      OpCommonHeader reply{
          hton16(kProtocolVersion),
          hton16(kOpRepImport),
          hton32(1),
      };
      (void)send_exact(client.get(), &reply, sizeof(reply));
      return;
    }

    try {
      std::thread([port, client = std::move(client)]() mutable {
        try {
          port->handle_import_session(client.get());
        } catch (const std::exception &error) {
          try {
            port->logger.log(LogScope::Usb, LogLevel::Error,
                             "USB/IP import session failed busid=" +
                                 port->busid + ": " + error.what());
          } catch (...) {
          }
        } catch (...) {
          try {
            port->logger.log(LogScope::Usb, LogLevel::Error,
                             "USB/IP import session failed busid=" +
                                 port->busid);
          } catch (...) {
          }
        }
        port->finish_import_session();
      }).detach();
    } catch (...) {
      port->finish_import_session();
    }
  }

  void listener_loop() {
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    std::string error;
    if (!listener.valid()) {
      error = "failed to create USB/IP listener";
    } else {
      const BOOL reuse = TRUE;
      setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&reuse), sizeof(reuse));

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = inet_addr("127.0.0.1");
      address.sin_port = hton16(kDefaultPort);
      if (bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0) {
        error = "failed to bind USB/IP listener to 127.0.0.1:" +
                std::to_string(kDefaultPort) + ": " +
                std::to_string(WSAGetLastError());
      } else if (listen(listener.get(), 8) != 0) {
        error = "failed to listen for USB/IP clients: " +
                std::to_string(WSAGetLastError());
      }
    }

    {
      std::lock_guard lock(ready_mutex);
      startup_error = std::move(error);
      ready = true;
    }
    ready_cv.notify_all();
    if (!startup_error.empty()) {
      return;
    }

    while (!stop_requested.load()) {
      sockaddr_in client_address{};
      int client_address_size = sizeof(client_address);
      Socket client(accept(listener.get(),
                           reinterpret_cast<sockaddr *>(&client_address),
                           &client_address_size));
      if (!client.valid()) {
        continue;
      }
      if (stop_requested.load()) {
        return;
      }
      handle_client(std::move(client));
    }
  }

  WinsockGuard winsock;
  std::mutex ports_mutex;
  std::map<unsigned, std::weak_ptr<Impl>> ports;
  std::atomic_bool stop_requested{false};
  std::thread listener_thread;
  std::mutex ready_mutex;
  std::condition_variable ready_cv;
  bool ready = false;
  std::string startup_error;
};

void VirtualPort::Impl::start() {
  pipe_path = R"(\\.\pipe\vds_usbip_port)" + std::to_string(port_index);

  pipe_server.reset(CreateNamedPipeA(
      pipe_path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024,
      0, nullptr));
  if (!pipe_server) {
    throw std::runtime_error("failed to create " + pipe_path);
  }

  iso_pacer_thread = std::thread([this] { iso_pacer_loop(); });
  Server::instance().register_port(shared_from_this());
  registered = true;
}

void VirtualPort::Impl::stop() {
  if (stop_requested.exchange(true)) {
    return;
  }
  if (registered) {
    Server::instance().unregister_port(port_index);
    registered = false;
  }
  pacer_cv.notify_all();
  demux_cv.notify_all();

  const SOCKET session_socket = client_socket.exchange(INVALID_SOCKET);
  if (session_socket != INVALID_SOCKET) {
    shutdown(session_socket, SD_BOTH);
  }
  if (pipe_server) {
    CancelIoEx(pipe_server.get(), nullptr);
  }

  {
    std::unique_lock lock(session_mutex);
    session_cv.wait(lock, [this] { return !session_active; });
  }

  if (pipe_reader_thread.joinable()) {
    pipe_server.reset();
    pipe_reader_thread.join();
  } else {
    pipe_server.reset();
  }

  if (iso_pacer_thread.joinable()) {
    pacer_cv.notify_all();
    iso_pacer_thread.join();
  }
}

bool VirtualPort::Impl::begin_import_session(SOCKET client) {
  std::lock_guard lock(session_mutex);
  if (session_active || stop_requested.load()) {
    return false;
  }
  session_active = true;
  client_socket = client;
  return true;
}

void VirtualPort::Impl::finish_import_session() {
  {
    std::lock_guard lock(session_mutex);
    client_socket = INVALID_SOCKET;
    session_active = false;
  }
  session_cv.notify_all();
}

void VirtualPort::Impl::iso_pacer_loop() {
  // Without 1ms timer resolution, wait_until quantizes to the system tick
  // (up to ~15.6ms), turning the 10ms completion cadence into lumpy
  // clumps -- the same reason vdsd's audio flush loop uses its
  // HighResolutionSleeper. Scoped to this thread's lifetime.
  const bool high_res = timeBeginPeriod(1) == TIMERR_NOERROR;
  std::unique_lock<std::mutex> lk(pacer_mutex);
  while (!stop_requested.load()) {
    if (paced_replies.empty()) {
      pacer_cv.wait(lk);
      continue;
    }
    // Earliest-due entry (multimap is ordered by key = due time).
    auto it = paced_replies.begin();
    const auto due = it->first;
    const auto now = std::chrono::steady_clock::now();
    if (now < due) {
      // Wake early if a sooner-due reply is inserted (notify on enqueue).
      pacer_cv.wait_until(lk, due);
      continue; // re-check earliest/stop after any wakeup
    }
    PacedReply reply = std::move(it->second);
    paced_replies.erase(it);
    lk.unlock();
    {
      std::lock_guard<std::mutex> send_guard(socket_send_mutex);
      // Failure just means the client went away; the session loop notices
      // on its own recv path, nothing to do here.
      send_exact(reply.client, reply.bytes.data(), reply.bytes.size());
    }
    lk.lock();
  }
  if (high_res) {
    timeEndPeriod(1);
  }
}

void VirtualPort::Impl::update_speaker_gain() {
  if (fu_muted.load()) {
    speaker_gain_q15 = 0;
    return;
  }
  const double db = static_cast<double>(fu_volume_db256.load()) / 256.0;
  double linear = std::pow(10.0, db / 20.0);
  if (linear > 1.0)
    linear = 1.0;
  if (linear < 0.0)
    linear = 0.0;
  speaker_gain_q15 = static_cast<int>(linear * (1 << 15) + 0.5);
}

void VirtualPort::Impl::handle_import_session(SOCKET client) {
  const ProfileDescriptors desc = descriptors_for_profile(profile);

  OpCommonHeader reply{
      hton16(kProtocolVersion),
      hton16(kOpRepImport),
      0,
  };
  if (!send_exact(client, &reply, sizeof(reply))) {
    return;
  }

  WireUsbDevice wire_dev{};
  std::strncpy(wire_dev.path, device_path.c_str(), sizeof(wire_dev.path) - 1);
  std::strncpy(wire_dev.busid, busid.c_str(), sizeof(wire_dev.busid) - 1);
  wire_dev.busnum = hton32(1);
  wire_dev.devnum = hton32(port_index + 1);
  wire_dev.speed = hton32(3);
  wire_dev.idVendor = hton16(desc.idVendor);
  wire_dev.idProduct = hton16(desc.idProduct);
  wire_dev.bcdDevice = hton16(VDS_USB_DEVICE_BCD);
  wire_dev.bDeviceClass = desc.device[4];
  wire_dev.bDeviceSubClass = desc.device[5];
  wire_dev.bDeviceProtocol = desc.device[6];
  wire_dev.bConfigurationValue = VDS_USB_CONFIGURATION_VALUE;
  wire_dev.bNumConfigurations = VDS_USB_NUM_CONFIGURATIONS;
  wire_dev.bNumInterfaces = 4;
  if (!send_exact(client, &wire_dev, sizeof(wire_dev))) {
    return;
  }

  attached = true;
  auto cleanup_connection = [this]() noexcept {
    attached = false;
    try {
      std::lock_guard<std::mutex> guard(pacer_mutex);
      paced_replies.clear();
      iso_out_next_due = {};
      iso_in_next_due = {};
    } catch (...) {
    }
    DisconnectNamedPipe(pipe_server.get());
  };

  try {
    try {
      logger.log(LogScope::Usb, LogLevel::Info,
                 "USB/IP client imported busid=" + busid);
    } catch (...) {
    }

    UniqueHandle connect_event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    if (!connect_event) {
      throw std::runtime_error("failed to create USB/IP pipe connect event: " +
                               win32_error_message(GetLastError()));
    }
    OVERLAPPED connect_overlapped{};
    connect_overlapped.hEvent = connect_event.get();
    if (!ConnectNamedPipe(pipe_server.get(), &connect_overlapped)) {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        const DWORD wait_result =
            WaitForSingleObject(connect_event.get(), INFINITE);
        DWORD transferred = 0;
        if (wait_result != WAIT_OBJECT_0 ||
            !GetOverlappedResult(pipe_server.get(), &connect_overlapped,
                                 &transferred, FALSE)) {
          throw std::runtime_error("failed to connect USB/IP bridge pipe: " +
                                   win32_error_message(GetLastError()));
        }
      } else if (error != ERROR_PIPE_CONNECTED) {
        throw std::runtime_error("failed to connect USB/IP bridge pipe: " +
                                 win32_error_message(error));
      }
    }

    if (!pipe_reader_thread.joinable()) {
      reader_exited = false;
      pipe_reader_thread = std::thread([this] { pipe_reader_loop(); });
    }

    while (!stop_requested.load()) {
      UsbipCmdSubmit cmd{};
      if (!recv_exact(client, &cmd, sizeof(cmd))) {
        break;
      }
      cmd.base.command = ntoh32(cmd.base.command);
      cmd.base.seqnum = ntoh32(cmd.base.seqnum);
      cmd.base.devid = ntoh32(cmd.base.devid);
      cmd.base.direction = ntoh32(cmd.base.direction);
      cmd.base.ep = ntoh32(cmd.base.ep);
      cmd.transfer_flags = ntoh32(cmd.transfer_flags);
      cmd.transfer_buffer_length = static_cast<std::int32_t>(
          ntoh32(static_cast<std::uint32_t>(cmd.transfer_buffer_length)));
      cmd.number_of_packets = static_cast<std::int32_t>(
          ntoh32(static_cast<std::uint32_t>(cmd.number_of_packets)));

      if (cmd.base.command == kCmdUnlink) {
        // Best-effort: acknowledge unlink without tracking in-flight URBs
        // individually. vDS's interrupt/iso endpoints are effectively
        // always-pending polling loops, so an unlink just means "stop
        // expecting a reply for that seqnum," which our per-request
        // synchronous handling already satisfies.
        UsbipRetUnlink unlink_reply{};
        unlink_reply.base.command = hton32(kRetUnlink);
        unlink_reply.base.seqnum = hton32(cmd.base.seqnum);
        {
          std::lock_guard<std::mutex> send_guard(socket_send_mutex);
          send_exact(client, &unlink_reply, sizeof(unlink_reply));
        }
        continue;
      }
      if (cmd.base.command != kCmdSubmit) {
        break;
      }
      if ((cmd.base.direction != kDirOut && cmd.base.direction != kDirIn) ||
          cmd.transfer_buffer_length < 0 ||
          static_cast<std::size_t>(cmd.transfer_buffer_length) >
              kMaxTransferBufferBytes ||
          cmd.number_of_packets < -1 ||
          cmd.number_of_packets > kMaxIsoPackets) {
        break;
      }

      // CMD_SUBMIT wire layout (verified against BOTH usbip-win2's
      // libdrv/pdu.cpp get_isoc_descr() and usbip-virtual-device's
      // command/submit.go Decode()): header, then the transfer buffer (OUT
      // direction only), then the iso packet descriptors. The descriptors
      // come AFTER the data, not before.
      //
      // The previous order here (descriptors first, data second) never
      // desynced the stream -- 160 + 3840 bytes reads the same total as
      // 3840 + 160 -- which made it invisible: it silently parsed the first
      // 160 bytes of PCM as descriptors and forwarded byte-shifted PCM to
      // vdsd. Priming URBs carry silence (all zeros), so the garbage
      // zero descriptors produced replies that happened to pass usbip-win2's
      // validation; the first URB carrying real audio put nonzero PCM into
      // the fake descriptors, our echoed reply's actual_length became a
      // garbage sum, the client's check() failed, and its recv thread exited
      // via async_detach_and_delete -- the recurring "connection dies right
      // after isoc OUT submission #2" signature.
      std::vector<std::uint8_t> out_data;
      std::vector<UsbipIsoPacketDescriptor> iso_descriptors;
      if (cmd.base.direction == kDirOut && cmd.transfer_buffer_length > 0) {
        out_data.resize(static_cast<std::size_t>(cmd.transfer_buffer_length));
        if (!recv_exact(client, out_data.data(), out_data.size())) {
          break;
        }
      }
      if (cmd.number_of_packets > 0) {
        iso_descriptors.resize(static_cast<std::size_t>(cmd.number_of_packets));
        if (!recv_exact(client, iso_descriptors.data(),
                        iso_descriptors.size() *
                            sizeof(UsbipIsoPacketDescriptor))) {
          break;
        }
        // recv_exact() above copies the raw wire bytes verbatim -- these
        // fields are still network byte order at this point, same as
        // cmd.number_of_packets before its explicit ntoh32() a few lines up.
        // Convert once here so every later use (pacing math, IN clamping, and
        // the RET_SUBMIT echo) works with real host-order values instead of
        // silently double-converting them when the reply is built.
        std::size_t total_iso_length = 0;
        for (auto &packet : iso_descriptors) {
          packet.offset = ntoh32(packet.offset);
          packet.length = ntoh32(packet.length);
          const std::size_t offset = packet.offset;
          const std::size_t length = packet.length;
          if (offset > static_cast<std::size_t>(cmd.transfer_buffer_length) ||
              length > static_cast<std::size_t>(cmd.transfer_buffer_length) -
                           offset) {
            throw std::runtime_error(
                "invalid USB/IP isochronous packet bounds");
          }
          total_iso_length += length;
        }
        if (total_iso_length !=
            static_cast<std::size_t>(cmd.transfer_buffer_length)) {
          throw std::runtime_error(
              "invalid USB/IP isochronous transfer length");
        }
      }

      try {
        if (!handle_cmd_submit(client, cmd, out_data, iso_descriptors)) {
          break;
        }
      } catch (const std::exception &) {
        break;
      }
    }

    try {
      logger.log(LogScope::Usb, LogLevel::Info,
                 "USB/IP client disconnected busid=" + busid);
    } catch (...) {
    }
  } catch (...) {
    cleanup_connection();
    throw;
  }
  cleanup_connection();
}

Frame VirtualPort::Impl::read_daemon_frame(HANDLE pipe) {
  return vds::win::read_handle_frame(pipe, "usbip-bridge", {});
}

void VirtualPort::Impl::write_daemon_frame(
    HANDLE pipe, std::uint16_t type, std::span<const std::uint8_t> payload) {
  vds::win::write_handle_frame(pipe, type, payload, "usbip-bridge", {});
}

void VirtualPort::Impl::pipe_reader_loop() {
  for (;;) {
    Frame frame;
    try {
      frame = read_daemon_frame(pipe_server.get());
    } catch (...) {
      // Pipe closed/cancelled (normal on stop(), or on a real daemon-side
      // teardown). Wake any waiters so they fail fast instead of hanging.
      reader_exited = true;
      demux_cv.notify_all();
      return;
    }
    if (stop_requested.load()) {
      reader_exited = true;
      demux_cv.notify_all();
      return;
    }
    std::lock_guard<std::mutex> lk(demux_mutex);
    switch (frame.header.type) {
    case VDS_FRAME_USB_HID_IN:
      hid_in_queue.push_back(std::move(frame));
      // Bound the queue: if the HID_IN handler stalls (e.g. blocked on
      // something else) we want to drop stale reports, not build
      // unbounded latency.
      if (hid_in_queue.size() > 8)
        hid_in_queue.pop_front();
      break;
    case VDS_FRAME_USB_FEATURE_REPLY:
      feature_reply_queue.push_back(std::move(frame));
      break;
    case VDS_FRAME_USB_AUDIO_IN: {
      mic_ring_buffer.insert(mic_ring_buffer.end(), frame.payload.begin(),
                             frame.payload.end());
      if (mic_ring_buffer.size() > kMicMaxBufferBytes) {
        mic_ring_buffer.erase(
            mic_ring_buffer.begin(),
            mic_ring_buffer.begin() +
                static_cast<std::ptrdiff_t>(mic_ring_buffer.size() -
                                            kMicMaxBufferBytes));
      }
      break;
    }
    default:
      // VDS_FRAME_STATUS and others are not expected in this direction;
      // drop rather than let them desync a queue.
      break;
    }
    demux_cv.notify_all();
  }
}

bool VirtualPort::Impl::wait_for_hid_in_frame(Frame &out, DWORD timeout_ms) {
  std::unique_lock<std::mutex> lk(demux_mutex);
  if (!demux_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
        return !hid_in_queue.empty() || stop_requested.load();
      })) {
    return false;
  }
  if (hid_in_queue.empty())
    return false;
  out = std::move(hid_in_queue.front());
  hid_in_queue.pop_front();
  return true;
}

bool VirtualPort::Impl::wait_for_feature_reply(Frame &out, DWORD timeout_ms) {
  std::unique_lock<std::mutex> lk(demux_mutex);
  if (!demux_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
        return !feature_reply_queue.empty() || stop_requested.load();
      })) {
    return false;
  }
  if (feature_reply_queue.empty())
    return false;
  out = std::move(feature_reply_queue.front());
  feature_reply_queue.pop_front();
  return true;
}

std::vector<std::uint8_t> VirtualPort::Impl::take_mic_bytes(std::size_t count) {
  std::lock_guard<std::mutex> lk(demux_mutex);
  std::vector<std::uint8_t> out;

  if (!mic_primed) {
    if (mic_ring_buffer.size() >= kMicPrimeBytes) {
      mic_primed = true;
    } else {
      out.assign(count, 0);
      return out;
    }
  }

  if (mic_ring_buffer.size() >= count) {
    out.assign(mic_ring_buffer.begin(),
               mic_ring_buffer.begin() + static_cast<std::ptrdiff_t>(count));
    mic_ring_buffer.erase(mic_ring_buffer.begin(),
                          mic_ring_buffer.begin() +
                              static_cast<std::ptrdiff_t>(count));
    return out;
  }

  out.assign(mic_ring_buffer.begin(), mic_ring_buffer.end());
  out.resize(count, 0);
  mic_ring_buffer.clear();
  mic_primed = false;
  return out;
}

bool VirtualPort::Impl::handle_cmd_submit(
    SOCKET client, const UsbipCmdSubmit &cmd,
    std::span<const std::uint8_t> out_data,
    const std::vector<UsbipIsoPacketDescriptor> &iso_descriptors) {
  const ProfileDescriptors desc = descriptors_for_profile(profile);
  UsbipRetSubmit ret{};
  ret.base.command = hton32(kRetSubmit);
  ret.base.seqnum = hton32(cmd.base.seqnum);

  std::vector<std::uint8_t> reply_data;

  if (cmd.base.ep == 0) {
    // Control transfer: setup packet lives in cmd.setup (already in wire
    // byte order per spec -- USB setup packets are little-endian on the
    // wire and USB/IP does not re-encode them).
    const std::uint8_t bmRequestType = cmd.setup[0];
    const std::uint8_t bRequest = cmd.setup[1];
    const std::uint16_t wValue =
        static_cast<std::uint16_t>(cmd.setup[2] | (cmd.setup[3] << 8));
    const std::uint16_t wIndex =
        static_cast<std::uint16_t>(cmd.setup[4] | (cmd.setup[5] << 8));
    const std::uint16_t wLength =
        static_cast<std::uint16_t>(cmd.setup[6] | (cmd.setup[7] << 8));
    const bool device_to_host = (bmRequestType & 0x80) != 0;
    const std::uint8_t request_type = bmRequestType & 0x60;

    if (request_type == 0x00 && bRequest == 0x06 && device_to_host) {
      // GET_DESCRIPTOR
      const std::uint8_t desc_type = static_cast<std::uint8_t>(wValue >> 8);
      const std::uint8_t desc_index = static_cast<std::uint8_t>(wValue & 0xff);
      const std::uint8_t *src = nullptr;
      std::size_t len = 0;
      if (desc_type == 0x01) { // DEVICE
        src = desc.device;
        len = desc.device_len;
      } else if (desc_type == 0x02) { // CONFIGURATION
        src = desc.config;
        len = desc.config_len;
      } else if (desc_type == 0x21) { // HID
        src = desc.hid;
        len = desc.hid_len;
      } else if (desc_type == 0x22) { // HID REPORT
        src = desc.hid_report;
        len = desc.hid_report_len;
      } else if (desc_type == 0x03) { // STRING
        // Index 0 is the language-ID list; index 1 is iManufacturer,
        // index 2 is iProduct (per the device descriptor byte layout in
        // ds5_usb.h -- iManufacturer=1, iProduct=2, iSerialNumber=0/none).
        std::vector<std::uint8_t> built;
        if (desc_index == 0) {
          built = build_langid_descriptor();
        } else if (desc_index == 1) {
          built = build_string_descriptor(VDS_USB_MANUFACTURER_STRING);
        } else if (desc_index == 2) {
          built = build_string_descriptor(profile == VDS_PROFILE_DSE
                                              ? VDS_DSE_USB_PRODUCT_STRING
                                              : VDS_DS5_USB_PRODUCT_STRING);
        }
        if (!built.empty()) {
          const std::size_t copy_len =
              std::min<std::size_t>(built.size(), wLength);
          reply_data.assign(built.begin(), built.begin() + copy_len);
        }
        ret.status = 0;
        src = nullptr;
        len = 0;
      }
      if (src != nullptr && len > 0) {
        const std::size_t copy_len = std::min<std::size_t>(len, wLength);
        reply_data.assign(src, src + copy_len);
      }
      ret.status = 0;
    } else if (request_type == 0x00 && bRequest == 0x09) {
      // SET_CONFIGURATION -- accept, no frame to forward (interfaces start
      // at altsetting 0 / inactive until SET_INTERFACE).
      ret.status = 0;
    } else if (bmRequestType == 0x01 && bRequest == 0x0b) {
      // SET_INTERFACE(interface=wIndex, altsetting=wValue). Forward to
      // Forward the interface transition through the existing frame boundary.
      vds_usb_interface_event event{};
      event.interface_number = static_cast<std::uint8_t>(wIndex);
      event.altsetting = static_cast<std::uint8_t>(wValue);
      if (wIndex == VDS_USB_AUDIO_OUT_INTERFACE) {
        event.interface_type = VDS_USB_INTERFACE_AUDIO_OUT;
      } else if (wIndex == VDS_USB_AUDIO_IN_INTERFACE) {
        event.interface_type = VDS_USB_INTERFACE_AUDIO_IN;
        // Flush the mic jitter buffer on every mic start/stop so a fresh
        // recording session doesn't inherit stale audio or a half-full
        // cushion from a previous one; re-prime from empty.
        {
          std::lock_guard<std::mutex> lk(demux_mutex);
          mic_ring_buffer.clear();
          mic_primed = false;
        }
      } else {
        event.interface_type = VDS_USB_INTERFACE_HID;
      }
      std::array<std::uint8_t, sizeof(event)> bytes{};
      std::memcpy(bytes.data(), &event, sizeof(event));
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_INTERFACE, bytes);
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 && bRequest == 0x01 &&
               device_to_host) {
      // HID GET_REPORT (feature). Ask vdsd, which caches/forwards over
      // Bluetooth as needed, then wait for the FEATURE_REPLY frame.
      const std::uint8_t report_id = static_cast<std::uint8_t>(wValue & 0xff);
      const std::array<std::uint8_t, 1> request{report_id};
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_FEATURE_GET, request);
      Frame reply;
      wait_for_feature_reply(reply, 500);
      if (reply.header.type == VDS_FRAME_USB_FEATURE_REPLY) {
        reply_data = reply.payload;
        // Never return more than the host asked for. A control-IN reply whose
        // actual_length exceeds the requested wLength is a wire violation that
        // makes usbip-win2 reset the whole TCP connection (dropping every
        // interface incl. audio, then persistent-reattach restores it). The
        // Bluetooth feature reply can be a different size than the USB
        // GET_REPORT requested (e.g. a WebHID app reading firmware report
        // 0x20), so clamp.
        if (reply_data.size() > wLength) {
          reply_data.resize(wLength);
        }
      }
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 && bRequest == 0x09) {
      // HID SET_REPORT (feature/output via control pipe).
      write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_FEATURE_SET,
                         out_data);
      ret.status = 0;
    } else if ((bmRequestType & 0x60) == 0x20 &&
               (bmRequestType & 0x1f) == 0x01 &&
               (bRequest == 0x81 || bRequest == 0x82 || bRequest == 0x83 ||
                bRequest == 0x84 || bRequest == 0x01)) {
      // USB Audio Class 1.0 Feature Unit control request (GET_CUR/MIN/MAX/RES
      // or SET_CUR) targeting a control selector (wValue hi byte) on an
      // entity (wIndex hi byte). usbaudio.sys queries these (typically
      // MUTE_CONTROL=0x01, VOLUME_CONTROL=0x02) during IRP_MN_START_DEVICE;
      // a compliant device must answer rather than STALL, or the class
      // driver fails to start. We don't have a real mixer to back these, so
      // just report sane fixed values (unmuted, 0dB, wide min/max range).
      const std::uint8_t control_selector =
          static_cast<std::uint8_t>((wValue >> 8) & 0xff);
      const bool is_mute = control_selector == 0x01;
      const bool is_volume = control_selector == 0x02;
      if (device_to_host && (is_mute || is_volume)) {
        if (is_mute) {
          reply_data.assign(
              {fu_muted.load() ? std::uint8_t{0x01} : std::uint8_t{0x00}});
        } else if (bRequest == 0x82) { // GET_MIN
          reply_data.assign(
              {0x01, 0xda});           // -100.00 dB (0xDA01, 1/256 dB units)
        } else if (bRequest == 0x83) { // GET_MAX
          reply_data.assign({0x00, 0x00}); // 0.00 dB
        } else if (bRequest == 0x84) {     // GET_RES
          reply_data.assign({0x01, 0x00}); // 1/256 dB step
        } else { // GET_CUR -- report the value the host last set
          const auto cur = static_cast<std::uint16_t>(
              static_cast<std::int16_t>(fu_volume_db256.load()));
          reply_data.assign({static_cast<std::uint8_t>(cur & 0xff),
                             static_cast<std::uint8_t>((cur >> 8) & 0xff)});
        }
        const std::size_t copy_len =
            std::min<std::size_t>(reply_data.size(), wLength);
        reply_data.resize(copy_len);
      } else if (!device_to_host && bRequest == 0x01) {
        // SET_CUR: actually apply it (previously accepted and discarded,
        // which left the Windows volume slider nonfunctional and audio
        // pinned at full scale). Payload arrives in the control OUT data
        // stage (out_data).
        if (is_mute && !out_data.empty()) {
          fu_muted = out_data[0] != 0;
          update_speaker_gain();
        } else if (is_volume && out_data.size() >= 2) {
          fu_volume_db256 = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(out_data[0]) |
              (static_cast<std::uint16_t>(out_data[1]) << 8));
          update_speaker_gain();
        }
      }
      ret.status = 0;
    } else {
      // Unhandled/unknown control request: STALL.
      ret.status = -32; // -EPIPE
    }
    // Defensive net: no control-IN reply may exceed the requested wLength.
    // An oversize actual_length is a wire violation that resets the whole
    // USB/IP connection (all interfaces incl. audio drop). The individual
    // handlers above clamp, but this guarantees it for any future path too.
    if (device_to_host && reply_data.size() > wLength) {
      reply_data.resize(wLength);
    }
  } else if (cmd.base.ep == kEpHidIn && cmd.base.direction == kDirIn) {
    Frame frame;
    if (wait_for_hid_in_frame(frame, 50)) {
      reply_data = frame.payload;
    }
    ret.status = 0;
  } else if (cmd.base.ep == kEpHidOut && cmd.base.direction == kDirOut) {
    write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_HID_OUT, out_data);
    ret.status = 0;
  } else if (cmd.base.ep == kEpAudioOut && cmd.base.direction == kDirOut) {
    // Isochronous OUT (speaker/haptics). Treat the whole URB's packets as one
    // contiguous PCM buffer, matching VDS_FRAME_USB_AUDIO_OUT.
    //
    // Pace completion at the packet cadence so Windows does not submit seconds
    // of PCM as a burst that overflows the Bluetooth write queue.
    // Apply the host-set Feature Unit volume to the speaker channels
    // (ch0/1) only; ch2/3 are haptics and must not follow system volume.
    // 4-channel 16-bit interleaved: 8 bytes per frame.
    const int gain_q15 = speaker_gain_q15.load();
    std::vector<std::uint8_t> gained;
    std::span<const std::uint8_t> pcm_to_forward = out_data;
    if (gain_q15 != (1 << 15)) {
      gained.assign(out_data.begin(), out_data.end());
      for (std::size_t off = 0; off + 8 <= gained.size(); off += 8) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
          const std::size_t idx = off + ch * 2;
          const auto sample = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(gained[idx]) |
              (static_cast<std::uint16_t>(gained[idx + 1]) << 8));
          const int scaled = (static_cast<int>(sample) * gain_q15) >> 15;
          const auto clamped = static_cast<std::int16_t>(
              scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : scaled));
          gained[idx] = static_cast<std::uint8_t>(clamped & 0xff);
          gained[idx + 1] = static_cast<std::uint8_t>((clamped >> 8) & 0xff);
        }
      }
      pcm_to_forward = gained;
    }
    write_daemon_frame(pipe_server.get(), VDS_FRAME_USB_AUDIO_OUT,
                       pcm_to_forward);
    ret.status = 0;
    ret.number_of_packets = cmd.number_of_packets;
    // Completion pacing happens at the send stage below (see the PacedReply
    // member comments): the reply for this URB is handed to the pacer
    // thread and sent at its real-time due moment instead of instantly.
    // (An earlier blocking-sleep pacing attempt was removed as it stalled
    // the shared session thread; the dedicated pacer avoids that.)
  } else if (cmd.base.ep == kEpAudioIn && cmd.base.direction == kDirIn) {
    // Isochronous IN (mic). Answer with exactly transfer_buffer_length bytes
    // from the jitter buffer (zero-padded on underrun) so the reply is
    // wire-consistent. Crucially, this reply is PACED at the send stage
    // below, exactly like audio OUT: without pacing, Windows completes and
    // resubmits isoc IN URBs as fast as the loopback allows (~10x real
    // time), draining the buffer far faster than the Bluetooth mic can fill
    // it -- so ~90% of every reply was zero-pad (measured recv/req ratio
    // ~0.09) and the mic was pure stutter. Pacing throttles Windows to the
    // real 48kHz rate so each URB carries the audio that actually arrived in
    // that interval.
    reply_data =
        take_mic_bytes(static_cast<std::size_t>(cmd.transfer_buffer_length));
    ret.status = 0;
    ret.number_of_packets = cmd.number_of_packets;
  } else {
    ret.status = -32; // -EPIPE: unknown endpoint
  }

  std::vector<UsbipIsoPacketDescriptor> reply_iso;
  std::uint32_t iso_actual_total = 0;
  if (cmd.number_of_packets > 0) {
    // Build the iso reply array up front (not just before sending it) so the
    // RET_SUBMIT header's actual_length can be the sum of per-packet
    // actual_lengths -- usbip-win2's client validates that invariant, and a
    // mismatch (previously: header always used reply_data.size(), which is 0
    // for OUT transfers since OUT has no reply payload) silently poisons the
    // connection right after the first real iso OUT submission.
    reply_iso.resize(iso_descriptors.size());
    for (std::size_t i = 0; i < reply_iso.size(); ++i) {
      const std::uint32_t off = iso_descriptors[i].offset;
      const std::uint32_t len = iso_descriptors[i].length;
      std::int32_t actual = static_cast<std::int32_t>(len);
      if (cmd.base.direction == kDirIn) {
        actual = (off >= reply_data.size())
                     ? 0
                     : static_cast<std::int32_t>(
                           std::min<std::size_t>(len, reply_data.size() - off));
      }
      reply_iso[i].offset = hton32(off);
      reply_iso[i].length = hton32(len);
      reply_iso[i].actual_length = hton32(static_cast<std::uint32_t>(actual));
      reply_iso[i].status = hton32(0);
      iso_actual_total += static_cast<std::uint32_t>(actual);
    }
  }

  const std::uint32_t actual_length =
      cmd.number_of_packets > 0
          ? iso_actual_total
          : static_cast<std::uint32_t>(cmd.base.direction == kDirOut
                                           ? out_data.size()
                                           : reply_data.size());
  ret.actual_length = hton32(ret.status == 0 ? actual_length : 0);
  ret.status = hton32(static_cast<std::uint32_t>(ret.status));
  // BUG FIX: number_of_packets (and start_frame/error_count) must be sent
  // in network byte order like status/actual_length -- this field was being
  // copied straight from the host-order cmd.number_of_packets and never
  // swapped. For HID (number_of_packets == -1, i.e. 0xFFFFFFFF) this is
  // byte-order symmetric so it looked fine all session; the moment real
  // isochronous audio carried a real nonzero packet count (10), usbip-win2
  // received a garbage value in a strict, validated field and reset the
  // TCP connection right after the first reply -- exactly matching the
  // observed "dies on the second ISO OUT submission" symptom.
  ret.number_of_packets =
      hton32(static_cast<std::uint32_t>(ret.number_of_packets));
  ret.start_frame = hton32(0);
  ret.error_count = hton32(0);

  // Isochronous replies (audio OUT = speaker/haptics, audio IN = mic) are
  // paced instead of sent inline -- see the PacedReply member comments.
  // Serialize the complete RET_SUBMIT and hand it to the pacer with a
  // real-time due moment: 1ms of audio per iso packet, anchored to the
  // previous URB's due time in that direction so a burst of early
  // submissions from Windows drains at exactly real time. If the stream
  // pauses (>250ms since the last due), the anchor resets so a new stream
  // does not inherit stale lateness or a far-future anchor.
  // Wire layout differs by direction: OUT carries header + iso descriptors
  // (no data); IN carries header + data payload + iso descriptors.
  const bool pace_out = cmd.base.ep == kEpAudioOut &&
                        cmd.base.direction == kDirOut &&
                        cmd.number_of_packets > 0;
  const bool pace_in = cmd.base.ep == kEpAudioIn &&
                       cmd.base.direction == kDirIn &&
                       cmd.number_of_packets > 0;
  if (pace_out || pace_in) {
    PacedReply paced{};
    paced.client = client;
    const std::size_t data_len = pace_in ? reply_data.size() : 0;
    const std::size_t iso_len =
        reply_iso.size() * sizeof(UsbipIsoPacketDescriptor);
    paced.bytes.resize(sizeof(ret) + data_len + iso_len);
    std::memcpy(paced.bytes.data(), &ret, sizeof(ret));
    if (data_len > 0) {
      std::memcpy(paced.bytes.data() + sizeof(ret), reply_data.data(),
                  data_len);
    }
    if (iso_len > 0) {
      std::memcpy(paced.bytes.data() + sizeof(ret) + data_len, reply_iso.data(),
                  iso_len);
    }
    {
      std::lock_guard<std::mutex> guard(pacer_mutex);
      auto &anchor = pace_out ? iso_out_next_due : iso_in_next_due;
      const auto now = std::chrono::steady_clock::now();
      if (anchor == std::chrono::steady_clock::time_point{} ||
          anchor < now - std::chrono::milliseconds(250)) {
        anchor = now; // stream (re)start: first URB completes now
      }
      const auto due = anchor;
      anchor += std::chrono::milliseconds(cmd.number_of_packets);
      paced_replies.emplace(due, std::move(paced));
    }
    pacer_cv.notify_all();
    return true;
  }

  std::lock_guard<std::mutex> send_guard(socket_send_mutex);
  if (!send_exact(client, &ret, sizeof(ret))) {
    return false;
  }
  if (!reply_data.empty() &&
      !send_exact(client, reply_data.data(), reply_data.size())) {
    return false;
  }
  if (cmd.number_of_packets > 0) {
    // reply_iso was already built above (needed early so the RET_SUBMIT
    // header's actual_length could be the sum of these packets' actual
    // lengths). Just send it here.
    if (!reply_iso.empty() &&
        !send_exact(client, reply_iso.data(),
                    reply_iso.size() * sizeof(UsbipIsoPacketDescriptor))) {
      return false;
    }
  }
  return true;
}

bool client_available() {
  const std::wstring path = resolve_client_path();
  return !path.empty() && regular_file_exists(path);
}

std::string client_path() {
  const std::wstring path = resolve_client_path();
  return path.empty() ? std::string{} : utf8_from_utf16(path);
}

std::string endpoint_for_port(unsigned port_index) {
  return endpoint_for_busid(busid_for_port(port_index));
}

VirtualPort::VirtualPort(std::uint32_t profile, unsigned port_index,
                         Logger &logger)
    : impl_(std::make_shared<Impl>(profile, port_index, logger)) {
  try {
    impl_->start();
    pipe_path_ = impl_->pipe_path;

    ClientProcess attach_process =
        start_client_process({L"attach", L"-r", L"127.0.0.1", L"-b",
                              utf16_from_utf8(impl_->busid), L"--once"});
    const DWORD started_at = GetTickCount();
    while (!impl_->attached.load()) {
      DWORD exit_code = STILL_ACTIVE;
      if (!GetExitCodeProcess(attach_process.process.get(), &exit_code)) {
        throw std::runtime_error("failed to read usbip.exe attach state: " +
                                 win32_error_message(GetLastError()));
      }
      if (exit_code != STILL_ACTIVE && exit_code != ERROR_SUCCESS) {
        throw std::runtime_error("usbip.exe attach failed with exit code " +
                                 std::to_string(exit_code));
      }
      if (GetTickCount() - started_at >= kClientAttachTimeoutMs) {
        if (exit_code == STILL_ACTIVE) {
          TerminateProcess(attach_process.process.get(), ERROR_TIMEOUT);
          WaitForSingleObject(attach_process.process.get(), INFINITE);
        }
        throw std::runtime_error("usbip.exe attach timed out for busid=" +
                                 impl_->busid);
      }
      Sleep(kClientAttachPollMs);
    }
    try {
      logger.log(LogScope::Usb, LogLevel::Info,
                 "USB/IP virtual port attached busid=" + impl_->busid +
                     " port=" + std::to_string(port_index));
    } catch (...) {
    }
  } catch (...) {
    impl_->stop();
    throw;
  }
}

VirtualPort::~VirtualPort() {
  if (impl_) {
    try {
      detach_client(impl_->busid, impl_->logger);
    } catch (const std::exception &error) {
      try {
        impl_->logger.log(LogScope::Usb, LogLevel::Warn,
                          "failed to detach USB/IP client busid=" +
                              impl_->busid + ": " + error.what());
      } catch (...) {
      }
    } catch (...) {
      try {
        impl_->logger.log(LogScope::Usb, LogLevel::Warn,
                          "failed to detach USB/IP client busid=" +
                              impl_->busid);
      } catch (...) {
      }
    }
    impl_->stop();
  }
}

bool VirtualPort::usb_attached() const { return impl_->attached.load(); }

std::string_view VirtualPort::busid() const { return impl_->busid; }

std::unique_ptr<VirtualPort>
open_virtual_port(std::uint32_t profile, unsigned port_index, Logger &logger) {
  return std::make_unique<VirtualPort>(profile, port_index, logger);
}

} // namespace vds::win::usbip
