#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "guest_mem.h"
#include "handles.h"
#include "sem_host.h"

// Hosted zABI-ish runtime core used by sem/instrument/VM.
//
// This is the "hosted version of zABI": guest pointers are validated/mapped
// through `mem`, and syscalls operate against a handle table.

typedef struct semrt {
  sem_guest_mem_t mem;
  sem_handles_t handles;
  sem_host_t ctl_host; // zi_ctl ops (e.g. CAPS_LIST)
  uint32_t abi_version;
} semrt_t;

typedef struct semrt_cfg {
  uint32_t abi_version;   // e.g. 0x00020005
  uint32_t guest_mem_cap; // bytes
  uint64_t guest_mem_base;

  // Capability entries exposed by CAPS_LIST.
  const sem_cap_t* caps;
  uint32_t cap_count;
} semrt_cfg_t;

bool semrt_init(semrt_t* rt, semrt_cfg_t cfg);
void semrt_dispose(semrt_t* rt);

// --- zABI core surface (hosted) ---
uint32_t semrt_zi_abi_version(const semrt_t* rt);
int32_t semrt_zi_ctl(semrt_t* rt, zi_ptr_t req_ptr, zi_size32_t req_len, zi_ptr_t resp_ptr, zi_size32_t resp_cap);
int32_t semrt_zi_read(semrt_t* rt, zi_handle_t h, zi_ptr_t dst_ptr, zi_size32_t cap);
int32_t semrt_zi_write(semrt_t* rt, zi_handle_t h, zi_ptr_t src_ptr, zi_size32_t len);
int32_t semrt_zi_end(semrt_t* rt, zi_handle_t h);
zi_ptr_t semrt_zi_alloc(semrt_t* rt, zi_size32_t size);
int32_t semrt_zi_free(semrt_t* rt, zi_ptr_t ptr);
int32_t semrt_zi_telemetry(semrt_t* rt, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr, zi_size32_t msg_len);

