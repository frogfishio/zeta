#include "sem_host.h"
#include "zi_tape.h"
#include "zcl1.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIR_VERSION
#define SIR_VERSION "0.0.0"
#endif

typedef struct dyn_cap {
  char* kind;
  char* name;
  uint32_t flags;
} dyn_cap_t;

static void sem_print_help(FILE* out) {
  fprintf(out,
          "sem — SIR emulator host frontend (MVP)\n"
          "\n"
          "Usage:\n"
          "  sem [--help] [--version]\n"
          "  sem --caps [--json]\n"
          "      [--cap KIND:NAME[:FLAGS]]...\n"
          "      [--cap-file-fs] [--cap-async-default] [--cap-sys-info]\n"
          "      [--tape-out PATH] [--tape-in PATH] [--tape-lax]\n"
          "\n"
          "Options:\n"
          "  --help        Show this help message\n"
          "  --version     Show version information (from ./VERSION)\n"
          "  --caps        Issue zi_ctl CAPS_LIST and print capabilities\n"
          "  --json        Emit --caps output as JSON (stdout)\n"
          "\n"
          "  --cap KIND:NAME[:FLAGS]\n"
          "      Add a capability entry. FLAGS is a comma-list of:\n"
          "        open (ZI_CAP_CAN_OPEN), pure (ZI_CAP_PURE), block (ZI_CAP_MAY_BLOCK)\n"
          "\n"
          "  --cap-file-fs       Sugar for --cap file:fs:open,block\n"
          "  --cap-async-default Sugar for --cap async:default:open,block\n"
          "  --cap-sys-info      Sugar for --cap sys:info:pure\n"
          "\n"
          "  --tape-out PATH  Record all zi_ctl requests/responses to a tape file\n"
          "  --tape-in PATH   Replay zi_ctl from a tape file (no real host)\n"
          "  --tape-lax       Do not require request bytes to match tape (unsafe)\n"
          "\n"
          "License: GPLv3+\n"
          "© 2026 Frogfish — Author: Alexander Croft\n");
}

static void sem_print_version(FILE* out) {
  fprintf(out, "sem %s\n", SIR_VERSION);
}

static char* sem_strdup(const char* s) {
  if (!s) return NULL;
  const size_t n = strlen(s);
  char* r = (char*)malloc(n + 1);
  if (!r) return NULL;
  memcpy(r, s, n + 1);
  return r;
}

static bool sem_parse_flags(const char* s, uint32_t* out_flags) {
  if (!out_flags) return false;
  uint32_t flags = 0;
  if (!s || s[0] == '\0') {
    *out_flags = 0;
    return true;
  }

  const char* p = s;
  while (*p) {
    const char* comma = strchr(p, ',');
    const size_t n = comma ? (size_t)(comma - p) : strlen(p);
    if (n == 0) return false;

    if (n == 4 && memcmp(p, "open", 4) == 0) flags |= SEM_ZI_CAP_CAN_OPEN;
    else if (n == 4 && memcmp(p, "pure", 4) == 0) flags |= SEM_ZI_CAP_PURE;
    else if (n == 5 && memcmp(p, "block", 5) == 0) flags |= SEM_ZI_CAP_MAY_BLOCK;
    else return false;

    p = comma ? comma + 1 : p + n;
  }

  *out_flags = flags;
  return true;
}

static bool sem_add_cap(dyn_cap_t* caps, uint32_t* inout_n, uint32_t cap_max, const char* spec) {
  if (!caps || !inout_n || !spec) return false;
  if (*inout_n >= cap_max) return false;

  const char* c1 = strchr(spec, ':');
  if (!c1) return false;
  const char* c2 = strchr(c1 + 1, ':');

  const size_t kind_len = (size_t)(c1 - spec);
  const size_t name_len = c2 ? (size_t)(c2 - (c1 + 1)) : strlen(c1 + 1);
  const char* flags_s = c2 ? (c2 + 1) : "";

  if (kind_len == 0 || name_len == 0) return false;

  char kind_buf[128];
  char name_buf[128];
  if (kind_len >= sizeof(kind_buf) || name_len >= sizeof(name_buf)) return false;
  memcpy(kind_buf, spec, kind_len);
  kind_buf[kind_len] = '\0';
  memcpy(name_buf, c1 + 1, name_len);
  name_buf[name_len] = '\0';

  uint32_t flags = 0;
  if (!sem_parse_flags(flags_s, &flags)) return false;

  dyn_cap_t dc = {0};
  dc.kind = sem_strdup(kind_buf);
  dc.name = sem_strdup(name_buf);
  dc.flags = flags;
  if (!dc.kind || !dc.name) {
    free(dc.kind);
    free(dc.name);
    return false;
  }

  caps[*inout_n] = dc;
  (*inout_n)++;
  return true;
}

static void sem_free_caps(dyn_cap_t* caps, uint32_t n) {
  if (!caps) return;
  for (uint32_t i = 0; i < n; i++) {
    free(caps[i].kind);
    free(caps[i].name);
  }
}

static bool sem_parse_caps_list_payload(const uint8_t* payload, uint32_t payload_len, bool json) {
  if (payload_len < 8) return false;
  uint32_t off = 0;
  const uint32_t version = zcl1_read_u32le(payload + off);
  off += 4;
  const uint32_t count = zcl1_read_u32le(payload + off);
  off += 4;
  if (version != 1) return false;

  if (json) {
    printf("{\"caps_version\":%u,\"cap_count\":%u,\"caps\":[", version, count);
  } else {
    printf("caps_version=%u cap_count=%u\n", version, count);
  }

  for (uint32_t i = 0; i < count; i++) {
    if (off + 4 > payload_len) return false;
    const uint32_t kind_len = zcl1_read_u32le(payload + off);
    off += 4;
    if (off + kind_len + 4 > payload_len) return false;
    const char* kind = (const char*)(payload + off);
    off += kind_len;

    const uint32_t name_len = zcl1_read_u32le(payload + off);
    off += 4;
    if (off + name_len + 4 > payload_len) return false;
    const char* name = (const char*)(payload + off);
    off += name_len;

    const uint32_t flags = zcl1_read_u32le(payload + off);
    off += 4;

    if (json) {
      if (i) printf(",");
      printf("{\"kind\":\"%.*s\",\"name\":\"%.*s\",\"flags\":%u}", (int)kind_len, kind, (int)name_len, name, flags);
    } else {
      printf("  - %.*s:%.*s flags=0x%08x\n", (int)kind_len, kind, (int)name_len, name, flags);
    }
  }

  if (json) {
    printf("]}\n");
  }
  return off == payload_len;
}

static int sem_do_caps(const sem_host_t* host, bool json, const char* tape_out, const char* tape_in,
                       bool tape_strict) {
  uint8_t req[ZCL1_HDR_SIZE];
  uint32_t req_len = 0;
  if (!sem_build_caps_list_req(1, req, (uint32_t)sizeof(req), &req_len)) {
    fprintf(stderr, "sem: internal: failed to build CAPS_LIST request\n");
    return 1;
  }

  uint8_t resp[4096];
  int32_t rc = 0;

  zi_tape_writer_t* tw = NULL;
  zi_tape_reader_t* tr = NULL;

  if (tape_in) {
    tr = zi_tape_reader_open(tape_in);
    if (!tr) {
      fprintf(stderr, "sem: failed to open tape for replay: %s\n", tape_in);
      return 1;
    }
    zi_ctl_replay_ctx_t ctx = {.tape = tr, .strict_match = tape_strict};
    rc = zi_ctl_replay(&ctx, req, req_len, resp, (uint32_t)sizeof(resp));
  } else {
    if (tape_out) {
      tw = zi_tape_writer_open(tape_out);
      if (!tw) {
        fprintf(stderr, "sem: failed to open tape for record: %s\n", tape_out);
        return 1;
      }
      zi_ctl_record_ctx_t ctx = {.inner = sem_zi_ctl, .inner_user = (void*)host, .tape = tw};
      rc = zi_ctl_record(&ctx, req, req_len, resp, (uint32_t)sizeof(resp));
    } else {
      rc = sem_zi_ctl((void*)host, req, req_len, resp, (uint32_t)sizeof(resp));
    }
  }

  if (tw) zi_tape_writer_close(tw);
  if (tr) zi_tape_reader_close(tr);

  if (rc < 0) {
    fprintf(stderr, "sem: zi_ctl transport error: %d\n", rc);
    return 1;
  }

  zcl1_hdr_t h = {0};
  const uint8_t* payload = NULL;
  if (!zcl1_parse(resp, (uint32_t)rc, &h, &payload)) {
    fprintf(stderr, "sem: invalid ZCL1 response\n");
    return 1;
  }

  if (h.op != SEM_ZI_CTL_OP_CAPS_LIST || h.rid != 1) {
    fprintf(stderr, "sem: unexpected response op=%u rid=%u\n", (unsigned)h.op, (unsigned)h.rid);
    return 1;
  }

  if (h.status == 0) {
    fprintf(stderr, "sem: CAPS_LIST failed (status=0)\n");
    return 1;
  }

  if (!sem_parse_caps_list_payload(payload, h.payload_len, json)) {
    fprintf(stderr, "sem: malformed CAPS_LIST payload\n");
    return 1;
  }

  return 0;
}

int main(int argc, char** argv) {
  bool want_caps = false;
  bool json = false;
  const char* tape_out = NULL;
  const char* tape_in = NULL;
  bool tape_strict = true;

  dyn_cap_t dyn_caps[64];
  uint32_t dyn_n = 0;
  memset(dyn_caps, 0, sizeof(dyn_caps));

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--help") == 0) {
      sem_print_help(stdout);
      sem_free_caps(dyn_caps, dyn_n);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      sem_print_version(stdout);
      sem_free_caps(dyn_caps, dyn_n);
      return 0;
    }
    if (strcmp(a, "--caps") == 0) {
      want_caps = true;
      continue;
    }
    if (strcmp(a, "--json") == 0) {
      json = true;
      continue;
    }
    if (strcmp(a, "--tape-out") == 0 && i + 1 < argc) {
      tape_out = argv[++i];
      continue;
    }
    if (strcmp(a, "--tape-in") == 0 && i + 1 < argc) {
      tape_in = argv[++i];
      continue;
    }
    if (strcmp(a, "--tape-lax") == 0) {
      tape_strict = false;
      continue;
    }
    if (strcmp(a, "--cap") == 0 && i + 1 < argc) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), argv[++i])) {
        fprintf(stderr, "sem: bad --cap spec\n");
        sem_free_caps(dyn_caps, dyn_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-file-fs") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "file:fs:open,block")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-async-default") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])),
                       "async:default:open,block")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-sys-info") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "sys:info:pure")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        return 2;
      }
      continue;
    }

    fprintf(stderr, "sem: unknown argument: %s\n", a);
    sem_print_help(stderr);
    sem_free_caps(dyn_caps, dyn_n);
    return 2;
  }

  if (!want_caps) {
    sem_print_help(stdout);
    sem_free_caps(dyn_caps, dyn_n);
    return 0;
  }

  sem_cap_t caps[64];
  const uint32_t cap_n = dyn_n;
  for (uint32_t i = 0; i < cap_n; i++) {
    caps[i].kind = dyn_caps[i].kind;
    caps[i].name = dyn_caps[i].name;
    caps[i].flags = dyn_caps[i].flags;
  }

  sem_host_t host;
  sem_host_init(&host, (sem_host_cfg_t){.caps = caps, .cap_count = cap_n});

  const int rc = sem_do_caps(&host, json, tape_out, tape_in, tape_strict);
  sem_free_caps(dyn_caps, dyn_n);
  return rc;
}

