#pragma once

// Shared file open flags for zingcore 2.5 file capabilities.
//
// These flags are used by file/aio@v1 (async completion-based file I/O).

enum {
  ZI_FILE_O_READ = 1u << 0,
  ZI_FILE_O_WRITE = 1u << 1,
  ZI_FILE_O_CREATE = 1u << 2,
  ZI_FILE_O_TRUNC = 1u << 3,
  ZI_FILE_O_APPEND = 1u << 4,
};
