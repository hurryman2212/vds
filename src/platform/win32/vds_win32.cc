// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_win32.hh"

namespace vds::win {

std::string win32_error_message(DWORD error) {
  LPSTR message = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageA(
      flags, nullptr, error, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
  if (length == 0 || message == nullptr) {
    return "Win32 error " + std::to_string(error);
  }

  std::string text(message, length);
  LocalFree(message);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

} // namespace vds::win
