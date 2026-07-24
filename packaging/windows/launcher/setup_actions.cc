#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <msiquery.h>

namespace {

std::wstring quote_command_argument(std::wstring_view value) {
  std::wstring result = L"\"";
  unsigned backslash_count = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslash_count;
      continue;
    }
    if (ch == L'"') {
      result.append(backslash_count * 2 + 1, L'\\');
      result.push_back(ch);
      backslash_count = 0;
      continue;
    }
    result.append(backslash_count, L'\\');
    backslash_count = 0;
    result.push_back(ch);
  }
  result.append(backslash_count * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::wstring sanitize_log_text(std::wstring value) {
  for (wchar_t &ch : value) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
      ch = L' ';
    } else if (ch < 0x20) {
      ch = L'?';
    }
  }
  return value;
}

std::wstring wide_from_code_page(UINT code_page, DWORD flags,
                                 std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const int length =
      MultiByteToWideChar(code_page, flags, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }

  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(code_page, flags, value.data(),
                      static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::wstring fallback_wide_from_log_bytes(std::string_view value) {
  std::wstring result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      result.push_back(L' ');
    } else if (ch >= 0x20 && ch < 0x7f) {
      result.push_back(static_cast<wchar_t>(ch));
    } else {
      result.push_back(L'?');
    }
  }
  return result;
}

std::wstring wide_from_log_bytes(std::string_view value) {
  std::wstring result =
      wide_from_code_page(CP_UTF8, MB_ERR_INVALID_CHARS, value);
  if (result.empty() && !value.empty()) {
    result = wide_from_code_page(CP_OEMCP, 0, value);
  }
  if (result.empty() && !value.empty()) {
    result = wide_from_code_page(CP_ACP, 0, value);
  }
  if (result.empty() && !value.empty()) {
    result = fallback_wide_from_log_bytes(value);
  }
  return sanitize_log_text(std::move(result));
}

std::filesystem::path installer_log_path() {
  std::vector<wchar_t> program_data(32768);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= static_cast<DWORD>(program_data.size())) {
    throw std::runtime_error("ProgramData is not available");
  }

  std::filesystem::path path(program_data.data());
  path /= L"vDS";
  std::filesystem::create_directories(path);
  path /= L"installer.log";
  return path;
}

void append_installer_log(std::wstring_view message) noexcept {
  try {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t timestamp[64]{};
    swprintf_s(timestamp, L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu ",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds);

    std::wstring line(timestamp);
    line += message;
    line += L"\r\n";

    const std::filesystem::path log_path = installer_log_path();
    bool write_bom = true;
    std::error_code error;
    if (std::filesystem::exists(log_path, error)) {
      write_bom = std::filesystem::file_size(log_path, error) == 0;
    }

    std::ofstream stream(log_path, std::ios::binary | std::ios::app);
    if (write_bom) {
      constexpr wchar_t bom = 0xfeff;
      stream.write(reinterpret_cast<const char *>(&bom), sizeof(bom));
    }
    stream.write(reinterpret_cast<const char *>(line.data()),
                 static_cast<std::streamsize>(line.size() * sizeof(wchar_t)));
  } catch (...) {
  }
}

std::wstring msi_property(MSIHANDLE install, const wchar_t *name) {
  DWORD size = 0;
  wchar_t empty_value[1]{};
  UINT status = MsiGetPropertyW(install, name, empty_value, &size);
  if (status != ERROR_MORE_DATA && status != ERROR_SUCCESS) {
    return {};
  }

  std::wstring value(size, L'\0');
  ++size;
  value.resize(size);
  status = MsiGetPropertyW(install, name, value.data(), &size);
  if (status != ERROR_SUCCESS) {
    return {};
  }
  value.resize(size);
  return value;
}

std::filesystem::path program_data_path() {
  std::vector<wchar_t> program_data(MAX_PATH + 1);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= program_data.size()) {
    throw std::runtime_error("ProgramData is not available");
  }

  return std::filesystem::path(program_data.data());
}

std::filesystem::path program_files_path() {
  std::vector<wchar_t> program_files(MAX_PATH + 1);
  DWORD length =
      GetEnvironmentVariableW(L"ProgramW6432", program_files.data(),
                              static_cast<DWORD>(program_files.size()));
  if (length == 0) {
    length = GetEnvironmentVariableW(L"ProgramFiles", program_files.data(),
                                     static_cast<DWORD>(program_files.size()));
  }
  if (length == 0 || length >= program_files.size()) {
    throw std::runtime_error("Program Files is not available");
  }

  return std::filesystem::path(program_files.data());
}

std::filesystem::path temp_path() {
  std::vector<wchar_t> temp_path(MAX_PATH + 1);
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
  if (length == 0 || length >= temp_path.size()) {
    throw std::runtime_error("GetTempPathW failed");
  }

  return std::filesystem::path(temp_path.data());
}

std::filesystem::path installer_cache_dir() {
  std::filesystem::path path = temp_path();
  path /= L"vDSSetup";
  return path;
}

std::filesystem::path installer_cache_path() {
  std::filesystem::path path = installer_cache_dir();
  path /= L"vDSSetup.exe";
  return path;
}

std::filesystem::path prepare_installer_cache_path() {
  std::filesystem::create_directories(installer_cache_dir());
  return installer_cache_path();
}

void delete_hklm_vds_setup_value(const wchar_t *name) {
  HKEY setup_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\vDS\\Setup", 0,
                    KEY_SET_VALUE, &setup_key) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteValueW(setup_key, name);
  RegCloseKey(setup_key);
}

void set_registry_string(HKEY key, const wchar_t *name,
                         const std::wstring &value) {
  const DWORD byte_count =
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE *>(value.data()), byte_count);
}

void set_registry_dword(HKEY key, const wchar_t *name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD,
                 reinterpret_cast<const BYTE *>(&value), sizeof(value));
}

std::wstring cache_setup_launcher(MSIHANDLE install) {
  const std::wstring source = msi_property(install, L"VDS_SETUP_SOURCE");
  if (source.empty() || !std::filesystem::exists(source)) {
    return {};
  }

  const std::filesystem::path cached_installer = prepare_installer_cache_path();
  std::filesystem::copy_file(source, cached_installer,
                             std::filesystem::copy_options::overwrite_existing);
  return cached_installer.wstring();
}

void update_control_panel_entry(MSIHANDLE install,
                                const std::wstring &cached_installer) {
  if (cached_installer.empty()) {
    return;
  }

  HKEY uninstall_key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                      L"Uninstall\\vDS",
                      0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                      &uninstall_key, nullptr) != ERROR_SUCCESS) {
    return;
  }

  const std::wstring install_folder = msi_property(install, L"INSTALLFOLDER");
  const std::wstring display_version =
      msi_property(install, L"VDS_DISPLAY_VERSION");
  const std::wstring uninstall_string =
      quote_command_argument(cached_installer) + L" --uninstall";

  set_registry_string(uninstall_key, L"DisplayName", L"vDS");
  set_registry_string(uninstall_key, L"DisplayVersion", display_version);
  set_registry_string(uninstall_key, L"DisplayIcon", cached_installer);
  set_registry_string(uninstall_key, L"InstallLocation", install_folder);
  set_registry_string(uninstall_key, L"Publisher", L"Jihong Min");
  set_registry_string(uninstall_key, L"UninstallString", uninstall_string);
  set_registry_dword(uninstall_key, L"NoModify", 1);
  set_registry_dword(uninstall_key, L"NoRepair", 1);

  RegCloseKey(uninstall_key);
}

void delete_control_panel_entry(REGSAM registry_view) {
  HKEY uninstall_root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_WRITE | registry_view,
                    &uninstall_root) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteTreeW(uninstall_root, L"vDS");
  RegCloseKey(uninstall_root);
}

std::wstring registry_string(HKEY key, const wchar_t *name) {
  DWORD type = 0;
  DWORD byte_count = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &byte_count) !=
          ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }

  std::wstring value(byte_count / sizeof(wchar_t), L'\0');
  if (value.empty()) {
    return {};
  }
  if (RegQueryValueExW(key, name, nullptr, nullptr,
                       reinterpret_cast<BYTE *>(value.data()),
                       &byte_count) != ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

bool is_vds_windows_installer_entry(HKEY uninstall_key) {
  const std::wstring display_name =
      registry_string(uninstall_key, L"DisplayName");
  return display_name == L"vDS" &&
         registry_string(uninstall_key, L"Publisher") == L"Jihong Min";
}

void delete_windows_installer_entries(REGSAM registry_view) {
  HKEY uninstall_root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0,
                    KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE |
                        DELETE | registry_view,
                    &uninstall_root) != ERROR_SUCCESS) {
    return;
  }

  DWORD index = 0;
  while (true) {
    wchar_t subkey_name[256]{};
    DWORD subkey_name_length =
        static_cast<DWORD>(sizeof(subkey_name) / sizeof(subkey_name[0]));
    const LSTATUS enum_status =
        RegEnumKeyExW(uninstall_root, index, subkey_name, &subkey_name_length,
                      nullptr, nullptr, nullptr, nullptr);
    if (enum_status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (enum_status != ERROR_SUCCESS) {
      ++index;
      continue;
    }

    bool remove_entry = false;
    HKEY uninstall_key = nullptr;
    if (RegOpenKeyExW(uninstall_root, subkey_name, 0, KEY_QUERY_VALUE,
                      &uninstall_key) == ERROR_SUCCESS) {
      remove_entry = is_vds_windows_installer_entry(uninstall_key);
      RegCloseKey(uninstall_key);
    }

    if (remove_entry) {
      std::wstring message = L"remove stale vDS Windows Installer entry: ";
      message += subkey_name;
      append_installer_log(message);
      RegDeleteTreeW(uninstall_root, subkey_name);
      continue;
    }

    ++index;
  }

  RegCloseKey(uninstall_root);
}

void remove_userspace_files(const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }

  std::error_code ignored;
  std::filesystem::remove(path / L"vdsd.exe", ignored);
  std::filesystem::remove(path / L"vdsctl.exe", ignored);
  std::filesystem::remove(path / L"opus.dll", ignored);
  std::filesystem::remove(path, ignored);
}

} // namespace

extern "C" __declspec(dllexport) UINT __stdcall
UpdateDisplayVersion(MSIHANDLE install) {
  const std::wstring cached_installer = cache_setup_launcher(install);
  update_control_panel_entry(install, cached_installer);
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall ClearPathMarker(MSIHANDLE) {
  delete_hklm_vds_setup_value(L"PathEntry");
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
RequestProgramDataRemoval(MSIHANDLE install) {
  if (msi_property(install, L"CustomActionData") != L"1") {
    return ERROR_SUCCESS;
  }

  try {
    const std::filesystem::path marker =
        program_data_path() / L"vDS.remove-program-data";
    std::ofstream stream(marker, std::ios::binary | std::ios::trunc);
    if (!stream) {
      append_installer_log(
          L"failed to create ProgramData removal request marker");
      return ERROR_INSTALL_FAILURE;
    }
    stream << "remove\n";
    stream.close();
    if (!stream) {
      append_installer_log(
          L"failed to write ProgramData removal request marker");
      return ERROR_INSTALL_FAILURE;
    }
    append_installer_log(
        L"ProgramData removal requested after Windows Installer exits");
  } catch (const std::exception &error) {
    std::wstring message = L"failed to request ProgramData removal: ";
    message += wide_from_log_bytes(error.what());
    append_installer_log(message);
    return ERROR_INSTALL_FAILURE;
  } catch (...) {
    append_installer_log(L"failed to request ProgramData removal");
    return ERROR_INSTALL_FAILURE;
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall
RemoveInstallDirectories(MSIHANDLE install) {
  try {
    remove_userspace_files(
        std::filesystem::path(msi_property(install, L"CustomActionData")));
    remove_userspace_files(program_files_path() / L"vDS");
  } catch (...) {
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall RemoveVdsRegistry(MSIHANDLE) {
  delete_control_panel_entry(KEY_WOW64_64KEY);
  delete_control_panel_entry(KEY_WOW64_32KEY);
  delete_control_panel_entry(0);
  delete_windows_installer_entries(KEY_WOW64_64KEY);
  delete_windows_installer_entries(KEY_WOW64_32KEY);
  delete_windows_installer_entries(0);

  HKEY software_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_WRITE,
                    &software_key) == ERROR_SUCCESS) {
    RegDeleteTreeW(software_key, L"vDS");
    RegCloseKey(software_key);
  }
  return ERROR_SUCCESS;
}
