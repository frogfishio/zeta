#pragma once

#include <stdint.h>

#include "guest_mem.h"
#include "handles.h"

// file/fs open flags (zABI 2.5)
enum {
  ZI_FILE_O_READ = 1u << 0,
  ZI_FILE_O_WRITE = 1u << 1,
  ZI_FILE_O_CREATE = 1u << 2,
  ZI_FILE_O_TRUNC = 1u << 3,
  ZI_FILE_O_APPEND = 1u << 4,
};

typedef struct semrt_file_fs_cfg {
  const char* fs_root; // if NULL/empty, capability should not be openable
} semrt_file_fs_cfg_t;

typedef struct semrt_file_fs {
  semrt_file_fs_cfg_t cfg;
} semrt_file_fs_t;

void semrt_file_fs_init(semrt_file_fs_t* fs, semrt_file_fs_cfg_t cfg);

// Implements the zABI open-from-params contract for file/fs:
// params: u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
//
// Returns:
// - handle >=3 on success
// - negative ZI_E_* on failure
zi_handle_t semrt_file_fs_open_from_params(semrt_file_fs_t* fs, sem_handles_t* hs, sem_guest_mem_t* mem, zi_ptr_t params_ptr,
                                           zi_size32_t params_len);

