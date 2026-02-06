#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct zi_tape_writer zi_tape_writer_t;
typedef struct zi_tape_reader zi_tape_reader_t;

zi_tape_writer_t* zi_tape_writer_open(const char* path);
void zi_tape_writer_close(zi_tape_writer_t* w);
bool zi_tape_writer_write(zi_tape_writer_t* w, const uint8_t* req, uint32_t req_len, int32_t rc, const uint8_t* resp,
                          uint32_t resp_len);

zi_tape_reader_t* zi_tape_reader_open(const char* path);
void zi_tape_reader_close(zi_tape_reader_t* r);

// Reads the next record. Returns false on EOF or error.
// The returned pointers are owned by the reader and are valid until the next call.
bool zi_tape_reader_next(zi_tape_reader_t* r, const uint8_t** out_req, uint32_t* out_req_len, int32_t* out_rc,
                         const uint8_t** out_resp, uint32_t* out_resp_len);

typedef int32_t (*sir_zi_ctl_fn)(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap);

typedef struct zi_ctl_record_ctx {
  sir_zi_ctl_fn inner;
  void* inner_user;
  zi_tape_writer_t* tape;
} zi_ctl_record_ctx_t;

typedef struct zi_ctl_replay_ctx {
  zi_tape_reader_t* tape;
  bool strict_match;
} zi_ctl_replay_ctx_t;

int32_t zi_ctl_record(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap);
int32_t zi_ctl_replay(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap);
