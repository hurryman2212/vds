#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include "setup_paths.hh"
#include "uninstall_payload.hh"

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

bool service_exists(const wchar_t *name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    return true;
  }

  SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD error = GetLastError();
    CloseServiceHandle(scm);
    return error != ERROR_SERVICE_DOES_NOT_EXIST;
  }

  CloseServiceHandle(service);
  CloseServiceHandle(scm);
  return true;
}

std::filesystem::path uninstall_root() {
  std::filesystem::path root = vds::setup::secure_system_temp_root();
  root /= L"uninstall-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_payloads(const std::filesystem::path &root) {
  for (const auto &payload : kVdsUninstallPayloads) {
    const std::filesystem::path path = root / payload.relative_path;
    std::filesystem::create_directories(path.parent_path());

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
      throw std::runtime_error("failed to open payload output");
    }
    stream.write(reinterpret_cast<const char *>(payload.data),
                 static_cast<std::streamsize>(payload.size));
    if (!stream) {
      throw std::runtime_error("failed to write payload output");
    }
  }
}

std::wstring read_previous_install_dir() {
  std::vector<wchar_t> value(32768);
  DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LSTATUS status =
      RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\vDS\\Setup", L"InstallDir",
                   RRF_RT_REG_SZ, nullptr, value.data(), &size);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return {};
  }
  if (status != ERROR_SUCCESS) {
    throw std::runtime_error("failed to read previous InstallDir");
  }
  return value.data();
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

void delete_windows_installer_entries() {
  delete_windows_installer_entries(KEY_WOW64_64KEY);
  delete_windows_installer_entries(KEY_WOW64_32KEY);
  delete_windows_installer_entries(0);
}

void append_process_output(std::wstring_view prefix, std::string_view output) {
  if (output.empty()) {
    std::wstring message(prefix);
    message += L"<no output>";
    append_installer_log(message);
    return;
  }

  std::istringstream stream{std::string(output)};
  std::string line;
  while (std::getline(stream, line)) {
    std::wstring message(prefix);
    message += wide_from_log_bytes(line);
    append_installer_log(message);
  }
}

int run_process(std::wstring command_line) {
  {
    std::wstring message = L"run uninstall script: ";
    message += command_line;
    append_installer_log(message);
  }

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0)) {
    std::wstring message = L"uninstall script CreatePipe failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    return 1;
  }
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    std::wstring message = L"uninstall script SetHandleInformation failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 1;
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup_info.wShowWindow = SW_HIDE;
  startup_info.hStdOutput = write_pipe;
  startup_info.hStdError = write_pipe;
  PROCESS_INFORMATION process_info{};

  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup_info,
                      &process_info)) {
    std::wstring message = L"uninstall script CreateProcessW failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 1;
  }

  CloseHandle(write_pipe);

  std::string output;
  char buffer[4096];
  DWORD bytes_read = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) &&
         bytes_read != 0) {
    output.append(buffer, buffer + bytes_read);
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    exit_code = 1;
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  CloseHandle(read_pipe);

  append_process_output(L"uninstall script output: ", output);

  std::wstring message = L"uninstall script exit: ";
  message += std::to_wstring(exit_code);
  message += L" output_bytes=";
  message += std::to_wstring(output.size());
  append_installer_log(message);
  return static_cast<int>(exit_code);
}

std::filesystem::path powershell_path() {
  std::vector<wchar_t> system_dir(MAX_PATH + 1);
  const UINT length = GetSystemDirectoryW(system_dir.data(),
                                          static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    throw std::runtime_error("GetSystemDirectoryW failed");
  }

  std::filesystem::path path(system_dir.data());
  path /= L"WindowsPowerShell\\v1.0\\powershell.exe";
  return path;
}

int run_powershell_script(const std::filesystem::path &script,
                          const std::vector<std::wstring> &arguments) {
  std::wstring command_line =
      quote_command_argument(powershell_path().wstring());
  command_line += L" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                  L"-WindowStyle Hidden -File ";
  command_line += quote_command_argument(script.wstring());
  for (const auto &argument : arguments) {
    command_line.push_back(L' ');
    command_line += quote_command_argument(argument);
  }

  return run_process(std::move(command_line));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::filesystem::path root;
  try {
    append_installer_log(L"existing vDS uninstall runner started");
    root = uninstall_root();
    write_payloads(root);

    int status = 0;
    if (service_exists(L"vdsd")) {
      status =
          run_powershell_script(root / L"uninstall_service.ps1", {L"-Stop"});
      (void)status;
    }

    const std::wstring previous_install_dir = read_previous_install_dir();
    if (!previous_install_dir.empty()) {
      status = run_powershell_script(root / L"uninstall_env.ps1",
                                     {previous_install_dir});
      (void)status;
    }

    delete_windows_installer_entries();

    std::filesystem::remove_all(root);
    append_installer_log(L"existing vDS uninstall runner finished");
    return 0;
  } catch (...) {
    if (!root.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
    append_installer_log(L"existing vDS uninstall runner failed");
    return 1;
  }
}
