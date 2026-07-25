// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

namespace vds::setup {

inline std::filesystem::path program_data_directory() {
  std::vector<wchar_t> program_data(32768);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= program_data.size()) {
    throw std::runtime_error("ProgramData is not available");
  }
  return std::filesystem::path(program_data.data());
}

inline std::filesystem::path persistent_launcher_path() {
  return program_data_directory() / L"vDS" / L"Setup" / L"vDSSetup.exe";
}

inline std::filesystem::path secure_system_temp_root() {
  std::vector<wchar_t> windows_directory(MAX_PATH + 1);
  for (;;) {
    const UINT length = GetSystemWindowsDirectoryW(
        windows_directory.data(), static_cast<UINT>(windows_directory.size()));
    if (length == 0) {
      throw std::runtime_error("GetSystemWindowsDirectoryW failed");
    }
    if (length < windows_directory.size()) {
      windows_directory.resize(length);
      break;
    }
    windows_directory.resize(static_cast<std::size_t>(length) + 1);
  }

  const std::filesystem::path root =
      std::filesystem::path(windows_directory.data()) / L"Temp" / L"vDSSetup";
  PSECURITY_DESCRIPTOR security_descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
          &security_descriptor, nullptr)) {
    throw std::runtime_error(
        "failed to create the vDS setup directory security descriptor");
  }

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.lpSecurityDescriptor = security_descriptor;
  security_attributes.bInheritHandle = FALSE;
  if (!CreateDirectoryW(root.c_str(), &security_attributes)) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      LocalFree(security_descriptor);
      throw std::runtime_error("failed to create the vDS system temp root: " +
                               std::to_string(error));
    }
  }

  const DWORD attributes = GetFileAttributesW(root.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    LocalFree(security_descriptor);
    throw std::runtime_error(
        "the vDS system temp root is not a safe directory");
  }

  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  PSID owner = nullptr;
  BOOL owner_defaulted = FALSE;
  PSID group = nullptr;
  BOOL group_defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(security_descriptor, &dacl_present, &dacl,
                                 &dacl_defaulted) ||
      !dacl_present ||
      !GetSecurityDescriptorOwner(security_descriptor, &owner,
                                  &owner_defaulted) ||
      !GetSecurityDescriptorGroup(security_descriptor, &group,
                                  &group_defaulted)) {
    LocalFree(security_descriptor);
    throw std::runtime_error(
        "failed to read the vDS setup directory security descriptor");
  }

  std::wstring mutable_root = root.wstring();
  const DWORD security_status = SetNamedSecurityInfoW(
      mutable_root.data(), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      owner, group, dacl, nullptr);
  LocalFree(security_descriptor);
  if (security_status != ERROR_SUCCESS) {
    throw std::runtime_error("failed to secure the vDS system temp root: " +
                             std::to_string(security_status));
  }
  return root;
}

} // namespace vds::setup
