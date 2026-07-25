// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace vds {

enum class LogLevel {
  Debug,
  Info,
  Warn,
  Error,
};

enum class LogScope {
  Bluetooth,
  Config,
  Control,
  Daemon,
  Hid,
  InputAudio,
  InputControl,
  Output,
  Port,
  Usb,
};

class Logger {
public:
  explicit Logger(const std::string &path);

  void reopen();
  void log(LogScope scope, LogLevel level, std::string_view message);

private:
  std::ofstream open_file() const;

  std::mutex mutex_;
  std::string path_;
  std::ofstream file_;
};

const char *log_level_name(LogLevel level);
const char *log_scope_name(LogScope scope);

} // namespace vds
