// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum SirIdKind {
  SIR_ID_SRC = 1,
  SIR_ID_SYM = 2,
  SIR_ID_TYPE = 3,
  SIR_ID_NODE = 4,
} SirIdKind;

typedef struct SirIdMapEntry {
  uint64_t hash;
  bool used;
  bool is_str;
  int64_t ikey;
  const char* skey;
  size_t slen;
  int64_t val;
} SirIdMapEntry;

typedef struct SirIdMap {
  SirIdMapEntry* entries;
  size_t cap;
  size_t len;
  int64_t next_id; // internal dense ids start at 1; 0 reserved for "absent" where applicable
} SirIdMap;

struct SirProgram;
typedef struct JsonValue JsonValue;

void sir_idmaps_init(struct SirProgram* p);
void sir_idmaps_free(struct SirProgram* p);

bool sir_intern_id(struct SirProgram* p, SirIdKind kind, const JsonValue* v, int64_t* out_id, const char* ctx);

// If `internal_id` originated from a string id, returns that string. Otherwise returns NULL.
const char* sir_id_str_for_internal(struct SirProgram* p, SirIdKind kind, int64_t internal_id);

// Parse/validate common ref forms.
bool parse_node_ref_id(struct SirProgram* p, const JsonValue* v, int64_t* out_id);
bool parse_type_ref_id(struct SirProgram* p, const JsonValue* v, int64_t* out_id);
bool parse_sym_ref_id(struct SirProgram* p, const JsonValue* v, int64_t* out_id);
