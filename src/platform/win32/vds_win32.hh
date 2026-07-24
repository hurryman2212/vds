// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vds::win {

std::string win32_error_message(DWORD error);

} // namespace vds::win
