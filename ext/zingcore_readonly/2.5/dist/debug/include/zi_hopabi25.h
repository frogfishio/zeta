#pragma once

#include <stdint.h>

#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional extension: Hopper guest ABI (v1)
//
// Goal: one guest-facing API that can be:
// - linked directly in native builds,
// - provided as imports in WASM builds,
// - exposed as callgates to a JIT capsule.
//
// Error convention for functions returning int32:
// - < 0  : ZI_E_* (transport/runtime errors: bounds, nosys, internal)
// - >= 0 : Hopper error code (0 = HOPPER_OK, >0 = HOPPER_E_*)
//
// Any out-parameters are written to guest memory as little-endian.

#define ZI_HOPABI25_VERSION 1u

// Open params (packed little-endian, 16 bytes):
//   u32 version (must be 1)
//   u32 arena_bytes
//   u32 ref_count
//   u32 flags (reserved; must be 0 for now)
// If params_len==0, defaults are used.

// Creates a Hopper instance and returns a small hop_id (>=0) or a negative ZI_E_* error.
int32_t zi_hop_open(zi_ptr_t params_ptr, zi_size32_t params_len);

// Closes a Hopper instance.
// Returns 0 on success, negative ZI_E_* on failure.
int32_t zi_hop_close(int32_t hop_id);

// Resets the Hopper arena + ref table.
// wipe_arena: 0/1
// Returns Hopper error (>=0) or negative ZI_E_*.
int32_t zi_hop_reset(int32_t hop_id, uint32_t wipe_arena);

// Allocates an untyped buffer.
// Writes i32 ref to out_ref_ptr (little-endian).
int32_t zi_hop_alloc(int32_t hop_id, uint32_t size, uint32_t align, zi_ptr_t out_ref_ptr);

// Releases a ref slot (does not reclaim arena bytes).
int32_t zi_hop_free(int32_t hop_id, int32_t ref);

// Allocates a record by layout_id (requires a catalog).
// Writes i32 ref to out_ref_ptr (little-endian).
int32_t zi_hop_record(int32_t hop_id, uint32_t layout_id, zi_ptr_t out_ref_ptr);

// Bytes field set/get (requires catalog).
int32_t zi_hop_field_set_bytes(int32_t hop_id, int32_t ref, uint32_t field_index,
                               zi_ptr_t bytes_ptr, zi_size32_t bytes_len);

// Copies exactly the field width into dst (requires dst_cap >= field width).
// Writes u32 written_len to out_written_ptr (little-endian).
int32_t zi_hop_field_get_bytes(int32_t hop_id, int32_t ref, uint32_t field_index,
                               zi_ptr_t dst_ptr, zi_size32_t dst_cap,
                               zi_ptr_t out_written_ptr);

int32_t zi_hop_field_set_i32(int32_t hop_id, int32_t ref, uint32_t field_index, int32_t v);

// Writes i32 value to out_v_ptr (little-endian).
int32_t zi_hop_field_get_i32(int32_t hop_id, int32_t ref, uint32_t field_index, zi_ptr_t out_v_ptr);

#ifdef __cplusplus
} // extern "C"
#endif
