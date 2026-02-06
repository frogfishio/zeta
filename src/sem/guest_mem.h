#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t zi_ptr_t;
typedef uint32_t zi_size32_t;

typedef struct sem_guest_mem {
  uint8_t* buf;
  uint32_t cap;
  uint32_t brk;
  uint64_t base;
} sem_guest_mem_t;

// Initializes guest memory to a zeroed heap of `cap` bytes.
// Guest pointers are offsets from `base` (base != 0).
bool sem_guest_mem_init(sem_guest_mem_t* m, uint32_t cap, uint64_t base);
void sem_guest_mem_dispose(sem_guest_mem_t* m);

// Maps guest memory into host pointers for copying.
bool sem_guest_mem_map_ro(const sem_guest_mem_t* m, zi_ptr_t ptr, zi_size32_t len, const uint8_t** out);
bool sem_guest_mem_map_rw(sem_guest_mem_t* m, zi_ptr_t ptr, zi_size32_t len, uint8_t** out);

// Simple deterministic bump allocator (MVP). `free` does not reuse memory.
zi_ptr_t sem_guest_alloc(sem_guest_mem_t* m, zi_size32_t size, zi_size32_t align);
int32_t sem_guest_free(sem_guest_mem_t* m, zi_ptr_t ptr);
