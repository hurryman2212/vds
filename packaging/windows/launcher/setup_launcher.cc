#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include "setup_payload.hh"

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

std::wstring quote_powershell_single_quoted_string(std::wstring_view value) {
  std::wstring result = L"'";
  for (const wchar_t ch : value) {
    if (ch == L'\'') {
      result += L"''";
    } else {
      result.push_back(ch);
    }
  }
  result.push_back(L'\'');
  return result;
}

std::filesystem::path system_executable(const wchar_t *name) {
  std::vector<wchar_t> system_dir(MAX_PATH + 1);
  const UINT length = GetSystemDirectoryW(system_dir.data(),
                                          static_cast<UINT>(system_dir.size()));
  if (length == 0 || length >= system_dir.size()) {
    throw std::runtime_error("GetSystemDirectoryW failed");
  }

  std::filesystem::path path(system_dir.data());
  path /= name;
  return path;
}

std::filesystem::path current_executable_path() {
  std::vector<wchar_t> path(MAX_PATH);
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0) {
      throw std::runtime_error("GetModuleFileNameW failed");
    }
    if (length < path.size()) {
      path.resize(length);
      return std::filesystem::path(path.data());
    }
    path.resize(path.size() * 2);
  }
}

std::filesystem::path program_data_path() {
  std::vector<wchar_t> program_data(32768);
  const DWORD length =
      GetEnvironmentVariableW(L"ProgramData", program_data.data(),
                              static_cast<DWORD>(program_data.size()));
  if (length == 0 || length >= static_cast<DWORD>(program_data.size())) {
    throw std::runtime_error("ProgramData is not available");
  }

  return std::filesystem::path(program_data.data());
}

std::filesystem::path installer_log_path() {
  std::filesystem::path path = program_data_path();
  path /= L"vDS";
  std::filesystem::create_directories(path);
  path /= L"installer.log";
  return path;
}

void reset_installer_log() {
  std::ofstream stream(installer_log_path(),
                       std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("failed to create installer log");
  }
  constexpr wchar_t bom = 0xfeff;
  stream.write(reinterpret_cast<const char *>(&bom), sizeof(bom));
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

bool process_is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }

  TOKEN_ELEVATION elevation{};
  DWORD bytes = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                      sizeof(elevation), &bytes);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

void configure_process_ui() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int relaunch_elevated(bool uninstall) {
  const std::filesystem::path executable = current_executable_path();
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = executable.c_str();
  info.lpParameters = uninstall ? L"--uninstall" : L"";
  info.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&info)) {
    if (GetLastError() == ERROR_CANCELLED) {
      MessageBoxW(nullptr, L"Setup was canceled.", L"vDS Setup",
                  MB_OK | MB_ICONINFORMATION);
      return ERROR_CANCELLED;
    }
    MessageBoxW(nullptr, L"Failed to start elevated vDS setup.", L"vDS Setup",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(info.hProcess, &exit_code);
  CloseHandle(info.hProcess);
  return static_cast<int>(exit_code);
}

std::filesystem::path setup_work_dir() {
  std::vector<wchar_t> temp_path(MAX_PATH + 1);
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
  if (length == 0 || length >= temp_path.size()) {
    throw std::runtime_error("GetTempPathW failed");
  }

  std::filesystem::path path(temp_path.data());
  path /= L"vDSSetup";
  path /= L"payload-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void write_payloads(const std::filesystem::path &dir) {
  for (const auto &payload : kVdsSetupPayloads) {
    const std::filesystem::path path = dir / payload.file_name;
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
      throw std::runtime_error("failed to create setup payload");
    }
    stream.write(reinterpret_cast<const char *>(payload.data),
                 static_cast<std::streamsize>(payload.size));
    if (!stream) {
      throw std::runtime_error("failed to write setup payload");
    }
  }
}

int run_process(std::wstring command_line, int show_window) {
  {
    std::wstring message = L"run: ";
    message += command_line;
    append_installer_log(message);
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW;
  startup_info.wShowWindow = static_cast<WORD>(show_window);
  PROCESS_INFORMATION process_info{};

  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup_info, &process_info)) {
    std::wstring message = L"CreateProcessW failed: ";
    message += std::to_wstring(GetLastError());
    append_installer_log(message);
    return 1;
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    exit_code = 1;
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);

  std::wstring message = L"exit: ";
  message += std::to_wstring(exit_code);
  append_installer_log(message);
  return static_cast<int>(exit_code);
}

void launch_process(std::wstring command_line, int show_window) {
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESHOWWINDOW;
  startup_info.wShowWindow = static_cast<WORD>(show_window);
  PROCESS_INFORMATION process_info{};

  if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup_info, &process_info)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
  }
}

bool is_success_exit_code(int status) {
  return status == 0 || status == 3010 || status == 1641;
}

bool is_restart_exit_code(int status) {
  return status == 3010 || status == 1641;
}

int run_dependency_script(const std::filesystem::path &script,
                          const std::filesystem::path &download_dir,
                          std::wstring_view mode) {
  std::wstring command_line = quote_command_argument(
      system_executable(L"WindowsPowerShell\\v1.0\\powershell.exe").wstring());
  command_line += L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy "
                  L"Bypass -WindowStyle Hidden -File ";
  command_line += quote_command_argument(script.wstring());
  command_line += L" -Mode ";
  command_line += mode;
  command_line += L" -DownloadDir ";
  command_line += quote_command_argument(download_dir.wstring());
  command_line += L" -LogPath ";
  command_line += quote_command_argument(installer_log_path().wstring());
  return run_process(std::move(command_line), SW_HIDE);
}

struct ServiceHandle {
  SC_HANDLE value = nullptr;

  ServiceHandle() = default;
  explicit ServiceHandle(SC_HANDLE value) : value(value) {}
  ServiceHandle(const ServiceHandle &) = delete;
  ServiceHandle &operator=(const ServiceHandle &) = delete;

  ServiceHandle &operator=(ServiceHandle &&other) noexcept {
    if (this != &other) {
      if (value) {
        CloseServiceHandle(value);
      }
      value = other.value;
      other.value = nullptr;
    }
    return *this;
  }

  ~ServiceHandle() {
    if (value) {
      CloseServiceHandle(value);
    }
  }

  SC_HANDLE get() const { return value; }
};

void append_win32_error_log(std::wstring_view prefix, DWORD error) noexcept {
  std::wstring message(prefix);
  message += L": ";
  message += std::to_wstring(error);
  append_installer_log(message);
}

std::wstring wide_from_ascii(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (const unsigned char ch : text) {
    if (ch >= 0x20 && ch < 0x7f) {
      result.push_back(static_cast<wchar_t>(ch));
    } else {
      result.push_back(L'?');
    }
  }
  return result;
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

std::wstring read_hklm_string(const wchar_t *subkey, const wchar_t *name,
                              DWORD *value_type = nullptr) {
  DWORD type = 0;
  DWORD byte_count = 0;
  LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name,
                                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                                nullptr, &byte_count);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return {};
  }
  if (status != ERROR_SUCCESS || byte_count == 0) {
    return {};
  }

  std::wstring value(byte_count / sizeof(wchar_t), L'\0');
  status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name,
                        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                        value.data(), &byte_count);
  if (status != ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  if (value_type) {
    *value_type = type;
  }
  return value;
}

std::filesystem::path installed_vds_dir() {
  const std::wstring install_dir =
      read_hklm_string(L"Software\\vDS\\Setup", L"InstallDir");
  if (!install_dir.empty()) {
    return std::filesystem::path(install_dir);
  }

  std::filesystem::path path = program_files_path();
  path /= L"vDS";
  return path;
}

std::wstring lowercase_ascii(std::wstring value) {
  for (wchar_t &ch : value) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return value;
}

std::wstring path_entry_key(std::wstring value) {
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
    value = value.substr(1, value.size() - 2);
  }
  while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return lowercase_ascii(std::move(value));
}

bool machine_path_contains(std::wstring_view path_value,
                           const std::wstring &install_dir) {
  const std::wstring wanted = path_entry_key(install_dir);
  std::wstring entry;
  for (const wchar_t ch : path_value) {
    if (ch == L';') {
      if (path_entry_key(entry) == wanted) {
        return true;
      }
      entry.clear();
      continue;
    }
    entry.push_back(ch);
  }
  return path_entry_key(entry) == wanted;
}

void broadcast_environment_change() {
  DWORD_PTR result = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                      reinterpret_cast<LPARAM>(L"Environment"),
                      SMTO_ABORTIFHUNG, 5000, &result);
}

bool path_install_was_requested() {
  return read_hklm_string(L"Software\\vDS\\Setup", L"PathEntry") == L"1";
}

void ensure_machine_path(const std::filesystem::path &install_dir) {
  if (!path_install_was_requested()) {
    append_installer_log(L"machine PATH update was not requested");
    return;
  }

  DWORD type = REG_EXPAND_SZ;
  std::wstring path =
      read_hklm_string(L"SYSTEM\\CurrentControlSet\\Control\\Session "
                       L"Manager\\Environment",
                       L"Path", &type);
  const std::wstring install_dir_string = install_dir.wstring();
  if (machine_path_contains(path, install_dir_string)) {
    append_installer_log(L"machine PATH already contains vDS install dir");
    broadcast_environment_change();
    return;
  }

  if (!path.empty() && path.back() != L';') {
    path.push_back(L';');
  }
  path += install_dir_string;

  HKEY key = nullptr;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", 0,
      KEY_SET_VALUE, &key);
  if (status != ERROR_SUCCESS) {
    append_win32_error_log(L"failed to open machine environment registry key",
                           status);
    return;
  }

  if (type != REG_EXPAND_SZ) {
    type = REG_SZ;
  }
  RegSetValueExW(key, L"Path", 0, type,
                 reinterpret_cast<const BYTE *>(path.data()),
                 static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  broadcast_environment_change();
  append_installer_log(L"machine PATH updated with vDS install dir");
}

void ensure_vdsd_service_registered(const std::filesystem::path &install_dir) {
  const std::filesystem::path vdsd_path = install_dir / L"vdsd.exe";
  if (!std::filesystem::exists(vdsd_path)) {
    throw std::runtime_error("vdsd.exe was not installed");
  }

  ServiceHandle scm(OpenSCManagerW(
      nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (!scm.get()) {
    throw std::runtime_error("failed to open service control manager");
  }

  ServiceHandle service(OpenServiceW(scm.get(), L"vdsd",
                                     SERVICE_CHANGE_CONFIG |
                                         SERVICE_QUERY_STATUS | SERVICE_START));
  if (!service.get()) {
    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_DOES_NOT_EXIST) {
      throw std::runtime_error("failed to open vdsd service");
    }

    const std::wstring binary_path =
        quote_command_argument(vdsd_path.wstring());
    service = ServiceHandle(CreateServiceW(
        scm.get(), L"vdsd", L"vDS Daemon",
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binary_path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service.get()) {
      throw std::runtime_error("failed to create vdsd service");
    }
    append_installer_log(L"vdsd service was created");
  }

  const std::wstring binary_path = quote_command_argument(vdsd_path.wstring());
  if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE,
                            SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                            binary_path.c_str(), nullptr, nullptr, nullptr,
                            nullptr, nullptr, L"vDS Daemon")) {
    throw std::runtime_error("failed to configure vdsd service");
  }

  SERVICE_DESCRIPTIONW description{};
  description.lpDescription = const_cast<LPWSTR>(L"vDS userspace daemon");
  ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION,
                        &description);
  append_installer_log(L"vdsd service is registered for automatic startup");
}

void start_vdsd_service() {
  ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm.get()) {
    append_win32_error_log(L"failed to open service control manager",
                           GetLastError());
    return;
  }

  ServiceHandle service(
      OpenServiceW(scm.get(), L"vdsd", SERVICE_QUERY_STATUS | SERVICE_START));
  if (!service.get()) {
    append_win32_error_log(L"failed to open vdsd service", GetLastError());
    return;
  }

  SERVICE_STATUS_PROCESS status{};
  DWORD bytes_needed = 0;
  if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                           reinterpret_cast<LPBYTE>(&status), sizeof(status),
                           &bytes_needed) &&
      status.dwCurrentState == SERVICE_RUNNING) {
    append_installer_log(L"vdsd service is already running");
    return;
  }

  if (!StartServiceW(service.get(), 0, nullptr)) {
    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_ALREADY_RUNNING) {
      append_win32_error_log(L"failed to start vdsd service", error);
      return;
    }
  }

  for (int attempt = 0; attempt < 40; ++attempt) {
    if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status), sizeof(status),
                             &bytes_needed) &&
        status.dwCurrentState == SERVICE_RUNNING) {
      append_installer_log(L"vdsd service started");
      return;
    }
    Sleep(250);
  }
  append_installer_log(L"vdsd service did not report running after start");
}

void append_msi_log_arguments(std::wstring &command_line) {
  command_line += L" /l*vx+! ";
  command_line += quote_command_argument(installer_log_path().wstring());
}

int run_main_msi(const std::filesystem::path &msi,
                 const std::filesystem::path &setup_source) {
  std::wstring command_line =
      quote_command_argument(system_executable(L"msiexec.exe").wstring());
  command_line += L" /i ";
  command_line += quote_command_argument(msi.wstring());
  command_line += L" ADDLOCAL=MainFeature VDS_SETUP_LAUNCHED=1 "
                  L"VDS_SETUP_SOURCE=";
  command_line += quote_command_argument(setup_source.wstring());
  command_line += L" /norestart";
  append_msi_log_arguments(command_line);
  return run_process(std::move(command_line), SW_SHOWNORMAL);
}

int run_main_msi_uninstall(const std::filesystem::path &msi,
                           const std::filesystem::path &setup_source) {
  std::wstring command_line =
      quote_command_argument(system_executable(L"msiexec.exe").wstring());
  command_line += L" /i ";
  command_line += quote_command_argument(msi.wstring());
  command_line += L" REMOVE=ALL VDS_EXPLICIT_UNINSTALL=1 VDS_SETUP_LAUNCHED=1 "
                  L"VDS_SETUP_SOURCE=";
  command_line += quote_command_argument(setup_source.wstring());
  command_line += L" /norestart";
  append_msi_log_arguments(command_line);
  return run_process(std::move(command_line), SW_SHOWNORMAL);
}

void remove_setup_cache_after_exit() {
  std::vector<wchar_t> temp_path(MAX_PATH + 1);
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
  if (length == 0 || length >= temp_path.size()) {
    return;
  }

  std::filesystem::path cache_dir(temp_path.data());
  cache_dir /= L"vDSSetup";

  std::wstring command_line = quote_command_argument(
      system_executable(L"WindowsPowerShell\\v1.0\\powershell.exe").wstring());
  command_line += L" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                  L"-WindowStyle Hidden -Command ";

  const std::wstring quoted_cache_dir =
      quote_powershell_single_quoted_string(cache_dir.wstring());
  std::wstring script = L"Start-Sleep -Seconds 2; "
                        L"for ($i = 0; $i -lt 30; ++$i) { "
                        L"Remove-Item -LiteralPath ";
  script += quoted_cache_dir;
  script += L" -Recurse -Force -ErrorAction SilentlyContinue; "
            L"if (!(Test-Path -LiteralPath ";
  script += quoted_cache_dir;
  script += L")) { break }; "
            L"Start-Sleep -Seconds 1 "
            L"}";
  command_line += quote_command_argument(script);
  launch_process(std::move(command_line), SW_HIDE);
}

void show_failed_status(const wchar_t *stage, int status) {
  std::wstring message = L"vDS setup failed while ";
  message += stage;
  message += L".\n\nExit code: ";
  message += std::to_wstring(status);
  MessageBoxW(nullptr, message.c_str(), L"vDS Setup", MB_OK | MB_ICONERROR);
}

} // namespace

extern "C" {
extern int __argc;
extern wchar_t **__wargv;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  configure_process_ui();

  bool uninstall = false;
  for (int index = 1; index < __argc; ++index) {
    if (std::wstring_view(__wargv[index]) == L"--uninstall") {
      uninstall = true;
    }
  }

  if (!process_is_elevated()) {
    return relaunch_elevated(uninstall);
  }

  std::filesystem::path work_dir;
  try {
    const std::filesystem::path program_data_removal_marker =
        program_data_path() / L"vDS.remove-program-data";
    if (uninstall) {
      std::error_code error;
      std::filesystem::remove(program_data_removal_marker, error);
      if (error) {
        throw std::system_error(error,
                                "failed to clear ProgramData removal marker");
      }
    }

    reset_installer_log();
    {
      std::wstring message = L"vDS setup version: ";
      message += kVdsSetupVersion;
      append_installer_log(message);
    }
    append_installer_log(uninstall ? L"vDS setup launcher started: uninstall"
                                   : L"vDS setup launcher started: install");

    work_dir = setup_work_dir();
    {
      std::wstring message = L"work dir: ";
      message += work_dir.wstring();
      append_installer_log(message);
    }
    write_payloads(work_dir);
    append_installer_log(L"payloads extracted");

    const std::filesystem::path dependency_script =
        work_dir / L"install_dependencies.ps1";
    const std::filesystem::path dependency_download_dir =
        work_dir / L"dependencies";
    if (!uninstall) {
      const int response = MessageBoxW(
          nullptr,
          L"vDS requires USB/IP and HidHide.\n\n"
          L"Setup will download the latest stable x64 installers from their "
          L"official GitHub releases when either dependency is missing. Each "
          L"download must have a valid Windows digital signature.\n\n"
          L"The USB/IP project recommends creating a Windows system restore "
          L"point before installation. Installing USB/IP also temporarily "
          L"restarts USB 3 hubs. Connected USB devices may disconnect, so save "
          L"any work that uses them before continuing.\n\n"
          L"Already installed dependencies are left unchanged. Continue?",
          L"vDS Setup", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
      if (response != IDOK) {
        std::filesystem::remove_all(work_dir);
        append_installer_log(L"dependency installation was canceled");
        return ERROR_CANCELLED;
      }

      const int download_status = run_dependency_script(
          dependency_script, dependency_download_dir, L"Download");
      if (!is_success_exit_code(download_status)) {
        show_failed_status(L"downloading and verifying dependencies",
                           download_status);
        std::filesystem::remove_all(work_dir);
        append_installer_log(L"dependency download failed");
        return download_status;
      }
    }

    const std::filesystem::path main_msi = work_dir / L"vDS-setup.msi";
    const int main_status =
        uninstall ? run_main_msi_uninstall(main_msi, current_executable_path())
                  : run_main_msi(main_msi, current_executable_path());
    if (!is_success_exit_code(main_status)) {
      if (main_status != 1602) {
        show_failed_status(L"running the main installer", main_status);
      }
      std::filesystem::remove_all(work_dir);
      append_installer_log(L"main installer failed");
      return main_status;
    }

    if (uninstall) {
      std::error_code marker_error;
      const bool remove_program_data =
          std::filesystem::exists(program_data_removal_marker, marker_error);
      if (marker_error) {
        throw std::system_error(marker_error,
                                "failed to read ProgramData removal marker");
      }

      std::filesystem::remove_all(work_dir);
      remove_setup_cache_after_exit();
      append_installer_log(L"vDS setup uninstall finished");

      if (remove_program_data) {
        append_installer_log(L"removing vDS ProgramData after MSI exit");
        std::error_code error;
        std::filesystem::remove(program_data_removal_marker, error);
        if (!error) {
          std::filesystem::remove_all(program_data_path() / L"vDS", error);
        }
        if (error) {
          std::wstring message = L"failed to remove vDS ProgramData: ";
          message += wide_from_ascii(error.message());
          append_installer_log(message);
          MessageBoxW(nullptr,
                      L"vDS was uninstalled, but C:\\ProgramData\\vDS could "
                      L"not be removed.",
                      L"vDS Setup", MB_OK | MB_ICONWARNING);
        }
      }
      return 0;
    }

    bool restart_required = is_restart_exit_code(main_status);
    const int dependency_status = run_dependency_script(
        dependency_script, dependency_download_dir, L"Install");
    if (!is_success_exit_code(dependency_status)) {
      show_failed_status(L"installing USB/IP and HidHide", dependency_status);
      std::filesystem::remove_all(work_dir);
      append_installer_log(
          L"dependency installation failed after the vDS MSI completed");
      return dependency_status;
    }
    restart_required =
        restart_required || is_restart_exit_code(dependency_status);

    const std::filesystem::path install_dir = installed_vds_dir();
    {
      std::wstring message = L"installed vDS dir: ";
      message += install_dir.wstring();
      append_installer_log(message);
    }
    ensure_vdsd_service_registered(install_dir);
    ensure_machine_path(install_dir);
    if (restart_required) {
      append_installer_log(
          L"vdsd service start deferred until Windows restarts");
    } else {
      start_vdsd_service();
    }

    std::filesystem::remove_all(work_dir);
    append_installer_log(L"vDS setup install finished");
    if (restart_required) {
      MessageBoxW(nullptr,
                  L"vDS was installed successfully. Restart Windows before "
                  L"using vDS to finish installing USB/IP and HidHide.",
                  L"vDS Setup", MB_OK | MB_ICONINFORMATION);
      return 3010;
    }
    return 0;
  } catch (const std::exception &error) {
    if (!work_dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(work_dir, ignored);
    }
    std::wstring message = L"vDS setup failed unexpectedly: ";
    message += wide_from_ascii(error.what());
    append_installer_log(message);
    MessageBoxW(nullptr, L"vDS setup failed unexpectedly.", L"vDS Setup",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
