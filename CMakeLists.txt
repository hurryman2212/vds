cmake_minimum_required(VERSION 3.12)

project(vds LANGUAGES CXX)

option(INSTALL_SERVICE "Register vdsd as a system service during install" OFF)
option(STOP_SERVICE "Stop vdsd before service install or uninstall" ON)
mark_as_advanced(STOP_SERVICE)

include(GNUInstallDirs)
if(WIN32 AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
      "C:/Program Files/vDS"
      CACHE PATH "Install path prefix" FORCE)
endif()

if(WIN32)
  find_package(Opus CONFIG REQUIRED)
  set(VDS_OPUS_TARGET Opus::opus)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
  pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
  pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
  set(VDS_OPUS_TARGET PkgConfig::OPUS)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VDS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VDS_BUILD_INFO_HEADER ${VDS_GENERATED_DIR}/vds_build_info.hh)
set(VDS_BUILD_INFO_SCRIPT
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake)

add_custom_target(
  vds_build_info_header ALL
  COMMAND
    ${CMAKE_COMMAND} -DVDS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DVDS_BUILD_INFO_HEADER=${VDS_BUILD_INFO_HEADER} -P ${VDS_BUILD_INFO_SCRIPT}
  BYPRODUCTS ${VDS_BUILD_INFO_HEADER}
  COMMENT "Generating vDS build info header"
  VERBATIM)

add_library(vds_protocol OBJECT src/vds_protocol.cc)
target_include_directories(
  vds_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_protocol PRIVATE ${VDS_OPUS_TARGET})

add_library(vds_common OBJECT src/vds_common.cc)
target_include_directories(vds_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(jsonl OBJECT src/jsonl.cc)
target_include_directories(jsonl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
  add_library(vds_win32 OBJECT src/platform/win32/vds_win32.cc)
  target_include_directories(
    vds_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/src)

  add_library(vds_io OBJECT src/platform/win32/vds_io.cc)
  target_include_directories(vds_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vds_io PUBLIC vds_protocol vds_win32)
else()
  add_library(vds_io OBJECT src/platform/linux/vds_io.cc)
  target_include_directories(vds_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

if(WIN32)
  set(VDS_FS_SOURCE src/platform/win32/vds_fs.cc)
else()
  set(VDS_FS_SOURCE src/platform/linux/vds_fs.cc)
endif()
add_library(vds_config OBJECT src/vds_config.cc ${VDS_FS_SOURCE})
target_include_directories(vds_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                             ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vds_config PUBLIC jsonl vds_common)

add_library(vds_log OBJECT src/vds_log.cc)
target_include_directories(vds_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(vdsctl_common OBJECT src/vdsctl_common.cc)
target_include_directories(vdsctl_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vdsctl_common PUBLIC jsonl vds_config)

add_library(vdsd_common OBJECT src/vdsd_common.cc)
target_include_directories(
  vdsd_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vdsd_common PUBLIC jsonl)

if(WIN32)
  add_library(vds_bt OBJECT src/platform/win32/vds_bt.cc)
  target_include_directories(vds_bt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vds_bt PUBLIC vds_protocol vds_common vds_io hid
                                      setupapi bthprops)

  add_executable(vdsd src/platform/win32/vdsd.cc)
  add_dependencies(vdsd vds_build_info_header)
  target_include_directories(vdsd PRIVATE ${VDS_GENERATED_DIR})
  target_link_libraries(
    vdsd
    PRIVATE vds_protocol
            jsonl
            vds_bt
            vds_common
            vds_config
            vds_log
            vds_io
            vds_win32
            vdsd_common
            winmm)

  add_executable(vdsctl src/platform/win32/vdsctl.cc)
  add_dependencies(vdsctl vds_build_info_header)
  target_include_directories(vdsctl PRIVATE ${VDS_GENERATED_DIR}
                                            ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(
    vdsctl
    PRIVATE vds_protocol
            jsonl
            vds_bt
            vds_common
            vds_config
            vds_io
            vds_win32
            vdsctl_common
            advapi32)
else()
  add_library(vds_bt OBJECT src/platform/linux/vds_bt.cc)
  target_link_libraries(vds_bt PUBLIC vds_protocol vds_io PkgConfig::BLUEZ)

  add_library(vds_bluez OBJECT src/platform/linux/vds_bluez.cc)
  target_include_directories(vds_bluez PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vds_bluez PUBLIC vds_config PkgConfig::DBUS)

  add_library(vds_udev OBJECT src/platform/linux/vds_udev.cc)
  target_include_directories(vds_udev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(vds_udev PUBLIC vds_config vds_common vds_io
                                        PkgConfig::UDEV)

  add_executable(vdsd src/platform/linux/vdsd.cc)
  add_dependencies(vdsd vds_build_info_header)
  target_include_directories(vdsd PRIVATE ${VDS_GENERATED_DIR})
  target_link_libraries(
    vdsd
    PRIVATE vds_protocol
            jsonl
            vds_bt
            vds_bluez
            vds_common
            vds_config
            vds_io
            vds_log
            vds_udev
            vdsd_common)

  add_executable(vdsctl src/platform/linux/vdsctl.cc)
  add_dependencies(vdsctl vds_build_info_header)
  target_include_directories(vdsctl PRIVATE ${VDS_GENERATED_DIR})
  target_link_libraries(
    vdsctl
    PRIVATE jsonl
            vds_bluez
            vds_common
            vds_config
            vds_io
            vdsctl_common
            vds_udev)
endif()

if(WIN32)
  set(VDS_RUNTIME_INSTALL_DIR ".")
  set(VDS_INSTALLED_VDSD "${CMAKE_INSTALL_PREFIX}/vdsd.exe")
  set(VDS_INSTALLED_VDSCTL "${CMAKE_INSTALL_PREFIX}/vdsctl.exe")
  set(VDS_INSTALLED_OPUS
      "${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:${VDS_OPUS_TARGET}>")
  set(VDS_POWERSHELL powershell.exe)
  set(VDS_STOP_SERVICE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/stop_service.ps1)
  set(VDS_INSTALL_SERVICE_SCRIPT
      ${CMAKE_CURRENT_SOURCE_DIR}/install_service.ps1)
  set(VDS_UNINSTALL_SERVICE_SCRIPT
      ${CMAKE_CURRENT_SOURCE_DIR}/uninstall_service.ps1)
  set(VDS_POWERSHELL_ARGS -NoProfile -ExecutionPolicy Bypass -File)
  set(VDS_POWERSHELL_ARGS_CODE "-NoProfile -ExecutionPolicy Bypass -File")
  set(VDS_STOP_SERVICE_CODE "execute_process(\n")
  string(
    APPEND
    VDS_STOP_SERVICE_CODE
    "  COMMAND \"${VDS_POWERSHELL}\" ${VDS_POWERSHELL_ARGS_CODE} \"${VDS_STOP_SERVICE_SCRIPT}\" -ServiceName vdsd\n"
  )
  string(APPEND VDS_STOP_SERVICE_CODE "  RESULT_VARIABLE VDS_SERVICE_RESULT)\n")
  string(APPEND VDS_STOP_SERVICE_CODE "if(NOT VDS_SERVICE_RESULT EQUAL 0)\n")
  string(APPEND VDS_STOP_SERVICE_CODE
         "  message(FATAL_ERROR \"failed to stop Windows service vdsd\")\n")
  string(APPEND VDS_STOP_SERVICE_CODE "endif()\n")

  set(VDS_INSTALL_SERVICE_CODE
      "set(VDS_SERVICE_BINARY \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/vdsd.exe\")\n"
  )
  string(
    APPEND
    VDS_INSTALL_SERVICE_CODE
    "file(TO_NATIVE_PATH \"\${VDS_SERVICE_BINARY}\" VDS_SERVICE_BINARY_NATIVE)\n"
  )
  string(APPEND VDS_INSTALL_SERVICE_CODE "execute_process(\n")
  string(
    APPEND
    VDS_INSTALL_SERVICE_CODE
    "  COMMAND \"${VDS_POWERSHELL}\" ${VDS_POWERSHELL_ARGS_CODE} \"${VDS_INSTALL_SERVICE_SCRIPT}\" -BinaryPath \"\${VDS_SERVICE_BINARY_NATIVE}\"\n"
  )
  string(APPEND VDS_INSTALL_SERVICE_CODE
         "  RESULT_VARIABLE VDS_SERVICE_RESULT)\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE "if(NOT VDS_SERVICE_RESULT EQUAL 0)\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE
         "  message(FATAL_ERROR \"failed to register Windows service vdsd\")\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE "endif()\n")

  if(STOP_SERVICE)
    set(VDS_UNINSTALL_SERVICE_STOP_ARG -Stop)
  else()
    set(VDS_UNINSTALL_SERVICE_STOP_ARG)
  endif()
else()
  set(VDS_RUNTIME_INSTALL_DIR ${CMAKE_INSTALL_BINDIR})
  set(VDS_INSTALLED_VDSD "${CMAKE_INSTALL_FULL_BINDIR}/vdsd")
  set(VDS_INSTALLED_VDSCTL "${CMAKE_INSTALL_FULL_BINDIR}/vdsctl")
  set(VDS_INSTALL_SERVICE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/install-service.sh)
  set(VDS_UNINSTALL_SERVICE_SCRIPT
      ${CMAKE_CURRENT_SOURCE_DIR}/uninstall-service.sh)
  set(VDS_SYSTEMD_UNIT_INSTALL_DIR
      "lib/systemd/system"
      CACHE PATH "systemd unit install directory")
  set(VDS_SYSTEMD_VDSD "${CMAKE_INSTALL_FULL_BINDIR}/vdsd")
  set(VDS_SYSTEMD_SERVICE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vdsd.service)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/vdsd.service.in
                 ${VDS_SYSTEMD_SERVICE_FILE} @ONLY)
  if(IS_ABSOLUTE "${VDS_SYSTEMD_UNIT_INSTALL_DIR}")
    set(VDS_INSTALLED_SYSTEMD_SERVICE
        "${VDS_SYSTEMD_UNIT_INSTALL_DIR}/vdsd.service")
  else()
    set(VDS_INSTALLED_SYSTEMD_SERVICE
        "${CMAKE_INSTALL_PREFIX}/${VDS_SYSTEMD_UNIT_INSTALL_DIR}/vdsd.service")
  endif()
  set(VDS_INSTALL_SERVICE_CODE
      "if(DEFINED ENV{DESTDIR} AND NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")\n")
  string(
    APPEND VDS_INSTALL_SERVICE_CODE
    "  message(STATUS \"Skipping systemd service enable under DESTDIR\")\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE "else()\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE "  execute_process(\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE
         "    COMMAND \"${VDS_INSTALL_SERVICE_SCRIPT}\" --enable\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE
         "    RESULT_VARIABLE VDS_SERVICE_RESULT)\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE
         "  if(NOT VDS_SERVICE_RESULT EQUAL 0)\n")
  string(
    APPEND
    VDS_INSTALL_SERVICE_CODE
    "    message(FATAL_ERROR \"failed to enable systemd service vdsd.service\")\n"
  )
  string(APPEND VDS_INSTALL_SERVICE_CODE "  endif()\n")
  string(APPEND VDS_INSTALL_SERVICE_CODE "endif()\n")
endif()

if(WIN32 AND INSTALL_SERVICE)
  if(STOP_SERVICE)
    install(CODE "${VDS_STOP_SERVICE_CODE}")
  endif()
endif()

install(TARGETS vdsd vdsctl RUNTIME DESTINATION ${VDS_RUNTIME_INSTALL_DIR})
if(WIN32)
  install(FILES "$<TARGET_FILE:${VDS_OPUS_TARGET}>"
          DESTINATION ${VDS_RUNTIME_INSTALL_DIR})
endif()

if(WIN32)
  if(INSTALL_SERVICE)
    install(CODE "${VDS_INSTALL_SERVICE_CODE}")
    add_custom_target(
      uninstall
      COMMAND
        ${VDS_POWERSHELL} ${VDS_POWERSHELL_ARGS} ${VDS_UNINSTALL_SERVICE_SCRIPT}
        -ServiceName vdsd ${VDS_UNINSTALL_SERVICE_STOP_ARG}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
              ${VDS_INSTALLED_VDSCTL} ${VDS_INSTALLED_OPUS})
  else()
    add_custom_target(
      uninstall COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
                        ${VDS_INSTALLED_VDSCTL} ${VDS_INSTALLED_OPUS})
  endif()
else()
  if(INSTALL_SERVICE)
    install(FILES ${VDS_SYSTEMD_SERVICE_FILE}
            DESTINATION ${VDS_SYSTEMD_UNIT_INSTALL_DIR})
    install(CODE "${VDS_INSTALL_SERVICE_CODE}")
    add_custom_target(
      uninstall
      COMMAND ${VDS_UNINSTALL_SERVICE_SCRIPT} --disable --remove-file
              ${VDS_INSTALLED_SYSTEMD_SERVICE}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
              ${VDS_INSTALLED_VDSCTL})
  else()
    add_custom_target(
      uninstall COMMAND ${CMAKE_COMMAND} -E rm -f ${VDS_INSTALLED_VDSD}
                        ${VDS_INSTALLED_VDSCTL})
  endif()
endif()
