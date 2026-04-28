
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Only provide stb_image_write implementation here.
// stb_image implementation is provided by: src/common/stb.cpp

#define STBIW_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "core/libraries/jpeg/third_party/stb_image_write.h"
