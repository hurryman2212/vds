// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_hidhide.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "unique_handle.hh"
#include "vds_log.hh"
#include "vds_win32.hh"

namespace vds::win::hidhide {

namespace {

constexpr DWORD kProcessTimeoutMs = 10000;
constexpr std::wstring_view kDefaultCliRelativePath =
    LR"(Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe)";

std::wstring utf16_from_utf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }

  const int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    throw std::runtime_error("failed to convert HidHide argument to UTF-16");
  }

  std::wstring converted(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), converted.data(),
                          required) != required) {
    throw std::runtime_error("failed to convert HidHide argument to UTF-16");
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
    throw std::runtime_error("failed to convert HidHide path to UTF-8");
  }

  std::string converted(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), converted.data(),
                          required, nullptr, nullptr) != required) {
    throw std::runtime_error("failed to convert HidHide path to UTF-8");
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

bool regular_file_exists(const std::wstring &path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring resolve_cli_path() {
  std::wstring path = environment_value(L"VDS_HIDHIDE_CLI");
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
  path += kDefaultCliRelativePath;
  return path;
}

std::wstring executable_path() {
  std::vector<wchar_t> path(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0) {
      throw std::runtime_error("failed to resolve vdsd executable path: " +
                               win32_error_message(GetLastError()));
    }
    if (length < path.size() - 1) {
      return std::wstring(path.data(), length);
    }
    path.resize(path.size() * 2);
  }
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

std::string run_cli(std::initializer_list<std::wstring> arguments) {
  const std::wstring cli_path = resolve_cli_path();
  if (!regular_file_exists(cli_path)) {
    throw std::runtime_error("HidHideCLI is unavailable");
  }

  SECURITY_ATTRIBUTES security_attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
  };
  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &security_attributes, 0)) {
    throw std::runtime_error("failed to create HidHideCLI output pipe: " +
                             win32_error_message(GetLastError()));
  }
  UniqueHandle output_read(read_handle);
  UniqueHandle output_write(write_handle);
  if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    throw std::runtime_error("failed to configure HidHideCLI output pipe: " +
                             win32_error_message(GetLastError()));
  }

  std::wstring command_line = quote_argument(cli_path);
  for (const std::wstring &argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_argument(argument);
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = output_write.get();
  startup_info.hStdError = output_write.get();
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(cli_path.c_str(), command_line.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    throw std::runtime_error("failed to start HidHideCLI: " +
                             win32_error_message(GetLastError()));
  }

  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);
  output_write.reset();

  std::string output;
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
        output.append(buffer.data(), read);
      }
    });
  } catch (...) {
    TerminateProcess(process.get(), ERROR_NOT_ENOUGH_MEMORY);
    WaitForSingleObject(process.get(), INFINITE);
    throw;
  }

  const DWORD wait_result =
      WaitForSingleObject(process.get(), kProcessTimeoutMs);
  if (wait_result != WAIT_OBJECT_0) {
    const DWORD wait_error =
        wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    TerminateProcess(process.get(), wait_result == WAIT_TIMEOUT
                                        ? ERROR_TIMEOUT
                                        : ERROR_OPERATION_ABORTED);
    WaitForSingleObject(process.get(), INFINITE);
    output_reader.join();
    if (wait_result == WAIT_TIMEOUT) {
      throw std::runtime_error("HidHideCLI timed out");
    }
    throw std::runtime_error("HidHideCLI wait failed: " +
                             win32_error_message(wait_error));
  }
  output_reader.join();

  DWORD exit_code = ERROR_GEN_FAILURE;
  if (!GetExitCodeProcess(process.get(), &exit_code)) {
    throw std::runtime_error("failed to read HidHideCLI exit code: " +
                             win32_error_message(GetLastError()));
  }
  if (exit_code != ERROR_SUCCESS) {
    throw std::runtime_error("HidHideCLI failed with exit code " +
                             std::to_string(exit_code));
  }
  return output;
}

bool device_is_hidden(std::string_view instance_path) {
  std::string output = run_cli({L"--dev-list"});
  std::string needle = "--dev-hide \"" + std::string(instance_path) + "\"";
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return output.find(needle) != std::string::npos;
}

} // namespace

bool provider_available() {
  const std::wstring path = resolve_cli_path();
  return !path.empty() && regular_file_exists(path);
}

std::string provider_path() {
  const std::wstring path = resolve_cli_path();
  return path.empty() ? std::string{} : utf8_from_utf16(path);
}

void register_daemon() { run_cli({L"--app-reg", executable_path()}); }

DeviceGuard::DeviceGuard(std::string instance_path, Logger &logger)
    : instance_path_(std::move(instance_path)), logger_(logger) {
  if (instance_path_.empty()) {
    throw std::runtime_error(
        "Windows HID device has no instance path for HidHide");
  }

  register_daemon();
  if (!device_is_hidden(instance_path_)) {
    run_cli({L"--dev-hide", utf16_from_utf8(instance_path_)});
    hide_added_ = true;
  }
  try {
    run_cli({L"--cloak-on"});
  } catch (...) {
    if (hide_added_) {
      try {
        run_cli({L"--dev-unhide", utf16_from_utf8(instance_path_)});
      } catch (...) {
      }
      hide_added_ = false;
    }
    throw;
  }

  try {
    logger_.log(LogScope::Bluetooth, LogLevel::Info,
                std::string("HidHide ") +
                    (hide_added_ ? "concealed" : "kept concealed") +
                    " Bluetooth HID device instance=" + instance_path_);
  } catch (...) {
  }
}

DeviceGuard::~DeviceGuard() {
  if (!hide_added_) {
    return;
  }

  try {
    run_cli({L"--dev-unhide", utf16_from_utf8(instance_path_)});
    try {
      logger_.log(LogScope::Bluetooth, LogLevel::Info,
                  "HidHide revealed Bluetooth HID device instance=" +
                      instance_path_);
    } catch (...) {
    }
  } catch (const std::exception &error) {
    try {
      logger_.log(LogScope::Bluetooth, LogLevel::Warn,
                  "failed to reveal HidHide device instance=" + instance_path_ +
                      ": " + error.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      logger_.log(LogScope::Bluetooth, LogLevel::Warn,
                  "failed to reveal HidHide device instance=" + instance_path_);
    } catch (...) {
    }
  }
}

std::unique_ptr<DeviceGuard> hide_device(std::string instance_path,
                                         Logger &logger) {
  return std::make_unique<DeviceGuard>(std::move(instance_path), logger);
}

} // namespace vds::win::hidhide
