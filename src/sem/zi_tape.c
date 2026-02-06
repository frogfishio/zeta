#include "zi_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zi_tape_writer {
  FILE* f;
};

struct zi_tape_reader {
  FILE* f;
  uint8_t* req;
  uint8_t* resp;
  uint32_t req_cap;
  uint32_t resp_cap;
};

static bool io_write_u32(FILE* f, uint32_t v) {
  return fwrite(&v, 1, sizeof(v), f) == sizeof(v);
}

static bool io_write_i32(FILE* f, int32_t v) {
  return fwrite(&v, 1, sizeof(v), f) == sizeof(v);
}

static bool io_read_u32(FILE* f, uint32_t* out) {
  return fread(out, 1, sizeof(*out), f) == sizeof(*out);
}

static bool io_read_i32(FILE* f, int32_t* out) {
  return fread(out, 1, sizeof(*out), f) == sizeof(*out);
}

zi_tape_writer_t* zi_tape_writer_open(const char* path) {
  if (!path) return NULL;
  FILE* f = fopen(path, "wb");
  if (!f) return NULL;
  zi_tape_writer_t* w = (zi_tape_writer_t*)calloc(1, sizeof(*w));
  if (!w) {
    fclose(f);
    return NULL;
  }
  w->f = f;
  return w;
}

void zi_tape_writer_close(zi_tape_writer_t* w) {
  if (!w) return;
  if (w->f) fclose(w->f);
  free(w);
}

bool zi_tape_writer_write(zi_tape_writer_t* w, const uint8_t* req, uint32_t req_len, int32_t rc, const uint8_t* resp,
                          uint32_t resp_len) {
  if (!w || !w->f) return false;
  if (req_len && !req) return false;
  if (resp_len && !resp) return false;

  if (!io_write_u32(w->f, req_len)) return false;
  if (req_len && fwrite(req, 1, req_len, w->f) != req_len) return false;
  if (!io_write_i32(w->f, rc)) return false;
  if (!io_write_u32(w->f, resp_len)) return false;
  if (resp_len && fwrite(resp, 1, resp_len, w->f) != resp_len) return false;
  return fflush(w->f) == 0;
}

zi_tape_reader_t* zi_tape_reader_open(const char* path) {
  if (!path) return NULL;
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  zi_tape_reader_t* r = (zi_tape_reader_t*)calloc(1, sizeof(*r));
  if (!r) {
    fclose(f);
    return NULL;
  }
  r->f = f;
  return r;
}

void zi_tape_reader_close(zi_tape_reader_t* r) {
  if (!r) return;
  if (r->f) fclose(r->f);
  free(r->req);
  free(r->resp);
  free(r);
}

static bool ensure_cap(uint8_t** buf, uint32_t* cap, uint32_t need) {
  if (*cap >= need) return true;
  uint32_t new_cap = *cap ? *cap : 256;
  while (new_cap < need) new_cap *= 2;
  uint8_t* nb = (uint8_t*)realloc(*buf, new_cap);
  if (!nb) return false;
  *buf = nb;
  *cap = new_cap;
  return true;
}

bool zi_tape_reader_next(zi_tape_reader_t* r, const uint8_t** out_req, uint32_t* out_req_len, int32_t* out_rc,
                         const uint8_t** out_resp, uint32_t* out_resp_len) {
  if (!r || !r->f || !out_req || !out_req_len || !out_rc || !out_resp || !out_resp_len) return false;

  uint32_t req_len = 0;
  if (!io_read_u32(r->f, &req_len)) return false;
  if (!ensure_cap(&r->req, &r->req_cap, req_len)) return false;
  if (req_len && fread(r->req, 1, req_len, r->f) != req_len) return false;

  int32_t rc = 0;
  if (!io_read_i32(r->f, &rc)) return false;

  uint32_t resp_len = 0;
  if (!io_read_u32(r->f, &resp_len)) return false;
  if (!ensure_cap(&r->resp, &r->resp_cap, resp_len)) return false;
  if (resp_len && fread(r->resp, 1, resp_len, r->f) != resp_len) return false;

  *out_req = r->req;
  *out_req_len = req_len;
  *out_rc = rc;
  *out_resp = r->resp;
  *out_resp_len = resp_len;
  return true;
}

int32_t zi_ctl_record(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap) {
  zi_ctl_record_ctx_t* ctx = (zi_ctl_record_ctx_t*)user;
  if (!ctx || !ctx->inner) return -10;

  int32_t rc = ctx->inner(ctx->inner_user, req, req_len, resp, resp_cap);
  const uint32_t resp_len = (rc >= 0) ? (uint32_t)rc : 0;
  if (ctx->tape) {
    (void)zi_tape_writer_write(ctx->tape, req, req_len, rc, resp, resp_len);
  }
  return rc;
}

int32_t zi_ctl_replay(void* user, const uint8_t* req, uint32_t req_len, uint8_t* resp, uint32_t resp_cap) {
  zi_ctl_replay_ctx_t* ctx = (zi_ctl_replay_ctx_t*)user;
  if (!ctx || !ctx->tape) return -10;

  const uint8_t* tape_req = NULL;
  const uint8_t* tape_resp = NULL;
  uint32_t tape_req_len = 0;
  uint32_t tape_resp_len = 0;
  int32_t tape_rc = 0;
  if (!zi_tape_reader_next(ctx->tape, &tape_req, &tape_req_len, &tape_rc, &tape_resp, &tape_resp_len)) {
    return -3;
  }

  if (ctx->strict_match) {
    if (tape_req_len != req_len) return -1;
    if (req_len && memcmp(tape_req, req, req_len) != 0) return -1;
  }

  if (tape_rc >= 0) {
    if (tape_resp_len > resp_cap) return -2;
    if (tape_resp_len) memcpy(resp, tape_resp, tape_resp_len);
    return (int32_t)tape_resp_len;
  }

  return tape_rc;
}
