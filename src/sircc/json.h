// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Arena Arena;

typedef enum JsonType {
  JSON_NULL = 0,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonArray {
  size_t len;
  JsonValue** items;
} JsonArray;

typedef struct JsonObjectItem {
  const char* key;
  JsonValue* value;
} JsonObjectItem;

typedef struct JsonObject {
  size_t len;
  JsonObjectItem* items;
} JsonObject;

struct JsonValue {
  JsonType type;
  union {
    bool b;
    int64_t i;
    const char* s;
    JsonArray arr;
    JsonObject obj;
  } v;
};

typedef struct JsonError {
  size_t offset;
  const char* msg;
} JsonError;

bool json_parse(Arena* arena, const char* input, JsonValue** out, JsonError* err);

JsonValue* json_obj_get(const JsonValue* obj, const char* key);
const char* json_get_string(const JsonValue* v);
bool json_get_i64(const JsonValue* v, int64_t* out);
bool json_is_object(const JsonValue* v);
bool json_is_array(const JsonValue* v);

