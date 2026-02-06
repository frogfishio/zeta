#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "guest_mem.h"
#include "handles.h"
#include "sem_host.h"

// Hosted zABI-ish runtime core used by emulators/tools/VM.
//
// This is a "hosted implementation" of the zABI syscall surface:
// - guest pointers (zi_ptr_t) refer to `mem` address space
// - zi_read/zi_write/zi_end dispatch through `handles`
// - caps are listed via CAPS_LIST and opened via zi_cap_open
//
// The intent is that `sircore` (the interpreter) can target this surface for
// development, while AOT-lowered binaries target the same zABI names.

typedef struct sir_hosted_zabi {
  // Guest memory is owned by the embedding VM/tool.
  // In standalone `sem` usage we create and own an arena, but in `sircore` VM
  // mode we typically reuse the VM's memory.
  sem_guest_mem_t* mem;
  bool owns_mem;
  sem_handles_t handles;
  sem_host_t ctl_host; // zi_ctl ops (e.g. CAPS_LIST)
  uint32_t abi_version;
  const char* fs_root;
} sir_hosted_zabi_t;

typedef struct sir_hosted_zabi_cfg {
  uint32_t abi_version;   // e.g. 0x00020005
  uint32_t guest_mem_cap; // bytes
  uint64_t guest_mem_base;

  // Capability entries exposed by CAPS_LIST.
  const sem_cap_t* caps;
  uint32_t cap_count;

  // Optional: enable file/fs sandbox.
  const char* fs_root;
} sir_hosted_zabi_cfg_t;

bool sir_hosted_zabi_init(sir_hosted_zabi_t* rt, sir_hosted_zabi_cfg_t cfg);
void sir_hosted_zabi_dispose(sir_hosted_zabi_t* rt);

// Initializes using an externally owned guest memory arena.
bool sir_hosted_zabi_init_with_mem(sir_hosted_zabi_t* rt, sem_guest_mem_t* mem, sir_hosted_zabi_cfg_t cfg);

// --- zABI core surface (hosted) ---
uint32_t sir_zi_abi_version(const sir_hosted_zabi_t* rt);
int32_t sir_zi_ctl(sir_hosted_zabi_t* rt, zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap);
int32_t sir_zi_read(sir_hosted_zabi_t* rt, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap);
int32_t sir_zi_write(sir_hosted_zabi_t* rt, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len);
int32_t sir_zi_end(sir_hosted_zabi_t* rt, zi_handle_t h);
zi_ptr_t sir_zi_alloc(sir_hosted_zabi_t* rt, zi_size32_t size);
int32_t sir_zi_free(sir_hosted_zabi_t* rt, zi_ptr_t ptr);
int32_t sir_zi_telemetry(sir_hosted_zabi_t* rt, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len);

// --- zABI caps extension (hosted) ---
int32_t sir_zi_cap_count(sir_hosted_zabi_t* rt);
int32_t sir_zi_cap_get_size(sir_hosted_zabi_t* rt, int32_t index);
int32_t sir_zi_cap_get(sir_hosted_zabi_t* rt, int32_t index, zi_ptr_t out_ptr, zi_size32_t out_cap);
zi_handle_t sir_zi_cap_open(sir_hosted_zabi_t* rt, zi_ptr_t req_ptr);
uint32_t sir_zi_handle_hflags(sir_hosted_zabi_t* rt, zi_handle_t h);
