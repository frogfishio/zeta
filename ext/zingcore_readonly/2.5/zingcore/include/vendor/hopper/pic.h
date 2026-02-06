// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: Apache-2.0
// Author: Alexander Croft <alex@frogfish.io>

// Internal PIC helpers for Hopper runtime (not part of the public ABI).
#ifndef HOPPER_PIC_H
#define HOPPER_PIC_H

#include "vendor/hopper/hopper.h"

hopper_err_t hopper_pic_encode_i32(const hopper_field_t *field, int32_t value, uint8_t *dst, uint32_t len);
hopper_err_t hopper_pic_decode_i32(const hopper_field_t *field, const uint8_t *src, uint32_t len, int32_t *out);

#endif // HOPPER_PIC_H
