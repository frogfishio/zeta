#pragma once

#include <stdint.h>

#include "zi_caps.h"
#include "zi_sysabi25.h"

#ifdef __cplusplus
extern "C" {
#endif

// Standard capability identity for the 2.5 file-system capability.
//
// This capability is opened via zi_cap_open() and yields stream handles usable
// with zi_read/zi_write/zi_end.
//
// kind = "file"
// name = "fs"

#define ZI_CAP_KIND_FILE "file"
#define ZI_CAP_NAME_FS "fs"

// Capability-specific params for zi_cap_open when opening kind="file", name="fs".
//
// The params blob is a fixed packed little-endian struct:
//   u64 path_ptr   (guest pointer to UTF-8 path bytes)
//   u32 path_len
//   u32 oflags     (ZI_FILE_O_*)
//   u32 create_mode (POSIX mode bits used when ZI_FILE_O_CREATE is set; e.g. 0644)
//
// Notes:
// - Paths are UTF-8 bytes; not NUL-terminated.
// - If env var ZI_FS_ROOT is set, absolute guest paths like "/a/b.txt" are resolved
//   under that host folder using openat(); ".." is rejected and symlinks are rejected
//   in any path segment.

enum {
  ZI_FILE_O_READ = 1u << 0,
  ZI_FILE_O_WRITE = 1u << 1,
  ZI_FILE_O_CREATE = 1u << 2,
  ZI_FILE_O_TRUNC = 1u << 3,
  ZI_FILE_O_APPEND = 1u << 4,
};

// Returns the cap descriptor for file/fs (version 1).
const zi_cap_v1 *zi_file_fs25_cap(void);

// Convenience: registers file/fs into the global cap registry.
// Requires zi_caps_init() to have been called.
int zi_file_fs25_register(void);

// Implementation hook used by zi_cap_open() when the file/fs cap is selected.
// Returns a handle (>=3) on success or a negative ZI_E_* error.
zi_handle_t zi_file_fs25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len);

#ifdef __cplusplus
} // extern "C"
#endif
