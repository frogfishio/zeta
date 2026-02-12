#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "guest_mem.h"

#ifdef SIR_HAVE_ZINGCORE25
#include "zi_sysabi25.h"
#endif

typedef int32_t zi_handle_t;

#ifndef SIR_HAVE_ZINGCORE25
enum {
  ZI_H_READABLE = 1u << 0,
  ZI_H_WRITABLE = 1u << 1,
  ZI_H_ENDABLE = 1u << 2,
  ZI_H_SEEKABLE = 1u << 3,
};
#endif

typedef struct sem_handle_ops {
  int32_t (*read)(void* ctx, sem_guest_mem_t* mem, zi_ptr_t dst_ptr, zi_size32_t cap);
  int32_t (*write)(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len);
  int32_t (*end)(void* ctx, sem_guest_mem_t* mem);
} sem_handle_ops_t;

typedef struct sem_handle_entry {
  const sem_handle_ops_t* ops;
  void* ctx;
  uint32_t hflags;
} sem_handle_entry_t;

typedef struct sem_handles {
  sem_handle_entry_t* entries;
  uint32_t cap;
  zi_handle_t next;
} sem_handles_t;

bool sem_handles_init(sem_handles_t* hs, uint32_t cap);
void sem_handles_dispose(sem_handles_t* hs);

bool sem_handle_install(sem_handles_t* hs, zi_handle_t h, sem_handle_entry_t e);
zi_handle_t sem_handle_alloc(sem_handles_t* hs, sem_handle_entry_t e);
bool sem_handle_lookup(const sem_handles_t* hs, zi_handle_t h, sem_handle_entry_t* out);
bool sem_handle_release(sem_handles_t* hs, zi_handle_t h);
uint32_t sem_handle_hflags(const sem_handles_t* hs, zi_handle_t h);

