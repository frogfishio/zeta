// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: Apache-2.0
// Author: Alexander Croft <alex@frogfish.io>

#include <limits.h>
#include <string.h>

#include "vendor/hopper/pic.h"

static int pow10_u64(uint32_t exp, uint64_t *out) {
  uint64_t v = 1;
  if (exp > 19) { // Avoid overflow in uint64_t.
    return 0;
  }
  for (uint32_t i = 0; i < exp; i++) {
    v *= 10;
  }
  *out = v;
  return 1;
}

static hopper_err_t check_field_numeric(const hopper_field_t *field) {
  if (!field) {
    return HOPPER_E_BAD_FIELD;
  }
  if (field->kind != HOPPER_FIELD_NUM_I32) {
    return HOPPER_E_BAD_FIELD;
  }
  if (field->pic.digits == 0) {
    return HOPPER_E_BAD_FIELD;
  }
  return HOPPER_OK;
}

static hopper_err_t check_sign(int32_t v, int is_signed) {
  if (!is_signed && v < 0) {
    return HOPPER_E_PIC_INVALID;
  }
  return HOPPER_OK;
}

static hopper_err_t check_digit_limit(uint16_t digits, int32_t value) {
  uint64_t limit = 0;
  int64_t abs_v = value < 0 ? -(int64_t)value : (int64_t)value;
  if (!pow10_u64(digits, &limit)) {
    return HOPPER_E_BAD_FIELD;
  }
  if ((uint64_t)abs_v >= limit) {
    // Example: digits=2, value=100 should overflow (limit=100).
    return HOPPER_E_OVERFLOW;
  }
  return HOPPER_OK;
}

static uint32_t display_expected_size(const hopper_field_t *field) {
  return field->pic.is_signed ? (uint32_t)(field->pic.digits + 1) : (uint32_t)field->pic.digits;
}

static hopper_err_t encode_display(const hopper_field_t *field, int32_t value, uint8_t *dst, uint32_t len) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_DISPLAY) {
    return HOPPER_E_UNSUPPORTED;
  }
  if (len != display_expected_size(field)) {
    return HOPPER_E_BAD_FIELD;
  }
  err = check_sign(value, field->pic.is_signed);
  if (err != HOPPER_OK) {
    return err;
  }
  err = check_digit_limit(field->pic.digits, value);
  if (err != HOPPER_OK) {
    return err;
  }

  uint16_t digits = field->pic.digits;
  uint64_t abs_v = value < 0 ? -(int64_t)value : (uint64_t)value;

  if (field->pic.is_signed) {
    dst[0] = value < 0 ? '-' : '+';
  }

  for (int i = (int)digits - 1; i >= 0; i--) {
    uint8_t d = (uint8_t)(abs_v % 10);
    dst[field->pic.is_signed ? 1 + i : i] = (uint8_t)('0' + d);
    abs_v /= 10;
  }
  return HOPPER_OK;
}

static hopper_err_t decode_display(const hopper_field_t *field, const uint8_t *src, uint32_t len, int32_t *out) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_DISPLAY) {
    return HOPPER_E_UNSUPPORTED;
  }
  if (len != display_expected_size(field)) {
    return HOPPER_E_BAD_FIELD;
  }

  int sign = 1;
  uint32_t idx = 0;
  if (field->pic.is_signed) {
    if (src[0] == '-') {
      sign = -1;
    } else if (src[0] == '+') {
      sign = 1;
    } else {
      return HOPPER_E_PIC_INVALID;
    }
    idx = 1;
  }

  uint64_t acc = 0;
  for (; idx < len; idx++) {
    uint8_t c = src[idx];
    if (c < '0' || c > '9') {
      return HOPPER_E_PIC_INVALID;
    }
    acc = (acc * 10u) + (uint64_t)(c - '0');
  }

  err = check_digit_limit(field->pic.digits, (int32_t)acc);
  if (err != HOPPER_OK) {
    return err;
  }

  int64_t signed_val = (int64_t)acc * (int64_t)sign;
  if (signed_val > INT32_MAX || signed_val < INT32_MIN) {
    return HOPPER_E_OVERFLOW;
  }
  *out = (int32_t)signed_val;
  return HOPPER_OK;
}

static hopper_err_t encode_comp(const hopper_field_t *field, int32_t value, uint8_t *dst, uint32_t len) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_COMP) {
    return HOPPER_E_UNSUPPORTED;
  }
  if (!(len == 2 || len == 4)) {
    return HOPPER_E_BAD_FIELD;
  }
  err = check_sign(value, field->pic.is_signed);
  if (err != HOPPER_OK) {
    return err;
  }
  err = check_digit_limit(field->pic.digits, value);
  if (err != HOPPER_OK) {
    return err;
  }

  if (len == 2) {
    if (value > INT16_MAX || value < INT16_MIN) {
      return HOPPER_E_OVERFLOW;
    }
    uint16_t v = (uint16_t)(uint16_t)value;
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
  } else {
    uint32_t v = (uint32_t)value;
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
  }
  return HOPPER_OK;
}

static hopper_err_t decode_comp(const hopper_field_t *field, const uint8_t *src, uint32_t len, int32_t *out) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_COMP) {
    return HOPPER_E_UNSUPPORTED;
  }
  if (!(len == 2 || len == 4)) {
    return HOPPER_E_BAD_FIELD;
  }

  int32_t v = 0;
  if (len == 2) {
    int16_t s = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
    v = (int32_t)s;
  } else {
    v = (int32_t)((uint32_t)src[0] |
                  ((uint32_t)src[1] << 8) |
                  ((uint32_t)src[2] << 16) |
                  ((uint32_t)src[3] << 24));
  }

  if (!field->pic.is_signed && v < 0) {
    return HOPPER_E_PIC_INVALID;
  }
  err = check_digit_limit(field->pic.digits, v);
  if (err != HOPPER_OK) {
    return err;
  }
  *out = v;
  return HOPPER_OK;
}

static hopper_err_t encode_comp3(const hopper_field_t *field, int32_t value, uint8_t *dst, uint32_t len) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_COMP3) {
    return HOPPER_E_UNSUPPORTED;
  }
  uint32_t expected = (uint32_t)((field->pic.digits + 2) / 2);
  if (len != expected) {
    return HOPPER_E_BAD_FIELD;
  }
  err = check_sign(value, field->pic.is_signed);
  if (err != HOPPER_OK) {
    return err;
  }
  err = check_digit_limit(field->pic.digits, value);
  if (err != HOPPER_OK) {
    return err;
  }

  uint64_t abs_v = value < 0 ? -(int64_t)value : (uint64_t)value;
  uint16_t digits = field->pic.digits;
  char digit_buf[32];
  if (digits > (uint16_t)(sizeof(digit_buf))) {
    return HOPPER_E_BAD_FIELD;
  }
  for (int i = (int)digits - 1; i >= 0; i--) {
    digit_buf[i] = (char)('0' + (abs_v % 10u));
    abs_v /= 10u;
  }

  memset(dst, 0, len);
  uint8_t sign_nibble = field->pic.is_signed ? (value < 0 ? 0xD : 0xC) : 0xF;
  dst[len - 1] |= sign_nibble;

  int nib = (int)(len * 2 - 2); // last digit nibble position
  for (int di = (int)digits - 1; di >= 0; di--, nib--) {
    int d = digit_buf[di] - '0';
    if (nib < 0) {
      return HOPPER_E_BAD_FIELD;
    }
    uint32_t byte_index = (uint32_t)(nib / 2);
    if ((nib % 2) == 0) {
      dst[byte_index] |= (uint8_t)(d << 4);
    } else {
      dst[byte_index] |= (uint8_t)d;
    }
  }

  return HOPPER_OK;
}

static hopper_err_t decode_comp3(const hopper_field_t *field, const uint8_t *src, uint32_t len, int32_t *out) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (field->pic.usage != HOPPER_USAGE_COMP3) {
    return HOPPER_E_UNSUPPORTED;
  }
  uint32_t expected = (uint32_t)((field->pic.digits + 2) / 2);
  if (len != expected) {
    return HOPPER_E_BAD_FIELD;
  }

  uint8_t sign_n = (uint8_t)(src[len - 1] & 0x0Fu);
  int sign = 1;
  if (field->pic.is_signed) {
    if (sign_n == 0xD) {
      sign = -1;
    } else if (sign_n == 0xC) {
      sign = 1;
    } else {
      return HOPPER_E_PIC_INVALID;
    }
  }

  uint64_t acc = 0;
  uint16_t digits = field->pic.digits;
  int nib = (int)(len * 2 - 2);
  for (int di = (int)digits - 1; di >= 0; di--, nib--) {
    if (nib < 0) {
      return HOPPER_E_BAD_FIELD;
    }
    uint32_t byte_index = (uint32_t)(nib / 2);
    uint8_t b = src[byte_index];
    uint8_t d = (nib % 2) == 0 ? (uint8_t)((b >> 4) & 0x0Fu) : (uint8_t)(b & 0x0Fu);
    if (d > 9) {
      return HOPPER_E_PIC_INVALID;
    }
    acc = (acc * 10u) + (uint64_t)d;
  }

  err = check_digit_limit(field->pic.digits, (int32_t)acc);
  if (err != HOPPER_OK) {
    return err;
  }

  int64_t signed_val = (int64_t)acc * (int64_t)sign;
  if (signed_val > INT32_MAX || signed_val < INT32_MIN) {
    return HOPPER_E_OVERFLOW;
  }
  *out = (int32_t)signed_val;
  return HOPPER_OK;
}

hopper_err_t hopper_pic_encode_i32(const hopper_field_t *field, int32_t value, uint8_t *dst, uint32_t len) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (!dst || len == 0) {
    return HOPPER_E_BAD_FIELD;
  }

  switch ((hopper_usage_t)field->pic.usage) {
    case HOPPER_USAGE_DISPLAY:
      return encode_display(field, value, dst, len);
    case HOPPER_USAGE_COMP:
      return encode_comp(field, value, dst, len);
    case HOPPER_USAGE_COMP3:
      return encode_comp3(field, value, dst, len);
    default:
      return HOPPER_E_UNSUPPORTED;
  }
}

hopper_err_t hopper_pic_decode_i32(const hopper_field_t *field, const uint8_t *src, uint32_t len, int32_t *out) {
  hopper_err_t err = check_field_numeric(field);
  if (err != HOPPER_OK) {
    return err;
  }
  if (!src || len == 0 || !out) {
    return HOPPER_E_BAD_FIELD;
  }

  switch ((hopper_usage_t)field->pic.usage) {
    case HOPPER_USAGE_DISPLAY:
      return decode_display(field, src, len, out);
    case HOPPER_USAGE_COMP:
      return decode_comp(field, src, len, out);
    case HOPPER_USAGE_COMP3:
      return decode_comp3(field, src, len, out);
    default:
      return HOPPER_E_UNSUPPORTED;
  }
}
