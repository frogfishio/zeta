#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest_mem.h"
#include "handles.h"

// Minimal `sircore` VM skeleton.
//
// This is not "full SIR" yet. It's a deterministic interpreter substrate that:
// - owns an emulated guest memory arena (zi_ptr_t space)
// - can call the minimal zABI primitives via a host vtable
//
// The next layer will be a structured SIR module builder + verifier, which will
// lower into this VM's internal instruction stream.

typedef struct sir_host_vtable {
  uint32_t (*zi_abi_version)(void* user);

  int32_t (*zi_ctl)(void* user, zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap);
  int32_t (*zi_read)(void* user, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap);
  int32_t (*zi_write)(void* user, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len);
  int32_t (*zi_end)(void* user, zi_handle_t h);

  zi_ptr_t (*zi_alloc)(void* user, zi_size32_t size);
  int32_t (*zi_free)(void* user, zi_ptr_t ptr);

  int32_t (*zi_telemetry)(void* user, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len);

  // Optional caps model (may be NULL).
  int32_t (*zi_cap_count)(void* user);
  int32_t (*zi_cap_get_size)(void* user, int32_t index);
  int32_t (*zi_cap_get)(void* user, int32_t index, zi_ptr_t out_ptr, zi_size32_t out_cap);
  zi_handle_t (*zi_cap_open)(void* user, zi_ptr_t req_ptr);
  uint32_t (*zi_handle_hflags)(void* user, zi_handle_t h);
} sir_host_vtable_t;

typedef struct sir_host {
  void* user;
  sir_host_vtable_t v;
} sir_host_t;

typedef enum sir_ins_kind {
  SIR_INS_NOP = 0,
  SIR_INS_WRITE_BYTES = 1, // write raw bytes to a handle
  SIR_INS_EXIT = 2,        // terminate with exit code
} sir_ins_kind_t;

typedef struct sir_ins {
  sir_ins_kind_t k;
  union {
    struct {
      zi_handle_t h;
      const uint8_t* bytes;
      uint32_t len;
    } write_bytes;
    struct {
      int32_t code;
    } exit_;
  } u;
} sir_ins_t;

typedef struct sir_vm {
  sem_guest_mem_t mem;
  sir_host_t host;
} sir_vm_t;

typedef struct sir_vm_cfg {
  uint32_t guest_mem_cap;
  uint64_t guest_mem_base;
  sir_host_t host;
} sir_vm_cfg_t;

bool sir_vm_init(sir_vm_t* vm, sir_vm_cfg_t cfg);
void sir_vm_dispose(sir_vm_t* vm);

// Runs a linear instruction stream. Returns an exit code (>=0) or negative error.
int32_t sir_vm_run(sir_vm_t* vm, const sir_ins_t* ins, size_t ins_count);

