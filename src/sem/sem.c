#include "sem_host.h"
#include "hosted_zabi.h"
#include "sir_module.h"
#include "sircore_vm.h"
#include "sem_hosted.h"
#include "sir_jsonl.h"
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
  uint8_t* meta;
  uint32_t meta_len;
} dyn_cap_t;

static void sem_print_help(FILE* out) {
  fprintf(out,
          "sem — SIR emulator host frontend (MVP)\n"
          "\n"
          "Usage:\n"
          "  sem [--help] [--version]\n"
          "  sem --print-support [--json]\n"
          "  sem --caps [--json]\n"
          "      [--cap KIND:NAME[:FLAGS]]...\n"
          "      [--cap-file-fs] [--cap-async-default] [--cap-sys-info]\n"
          "      [--fs-root PATH]\n"
          "      [--tape-out PATH] [--tape-in PATH] [--tape-lax]\n"
          "  sem --cat GUEST_PATH --fs-root PATH\n"
          "  sem --sir-hello\n"
          "  sem --sir-module-hello\n"
          "  sem --run FILE.sir.jsonl [--diagnostics text|json] [--fs-root PATH] [--cap ...]\n"
          "\n"
          "Options:\n"
          "  --help        Show this help message\n"
          "  --version     Show version information (from ./VERSION)\n"
          "  --print-support  Print the supported SIR subset for `sem --run`\n"
          "  --caps        Issue zi_ctl CAPS_LIST and print capabilities\n"
          "  --cat PATH    Read PATH via file/fs and write to stdout\n"
          "  --sir-hello   Run a tiny built-in sircore VM smoke program\n"
          "  --sir-module-hello  Run a tiny built-in sircore module smoke program\n"
          "  --run FILE    Run a small supported SIR subset (MVP)\n"
          "  --json        Emit --caps output as JSON (stdout)\n"
          "  --diagnostics Emit --run diagnostics as: text (default) or json\n"
          "  --all         For --run, try to emit multiple diagnostics (best-effort)\n"
          "\n"
          "  --cap KIND:NAME[:FLAGS]\n"
          "      Add a capability entry. FLAGS is a comma-list of:\n"
          "        open (ZI_CAP_CAN_OPEN), pure (ZI_CAP_PURE), block (ZI_CAP_MAY_BLOCK)\n"
          "\n"
          "  --cap-file-fs       Sugar for --cap file:fs:open,block\n"
          "  --cap-async-default Sugar for --cap async:default:open,block\n"
          "  --cap-sys-info      Sugar for --cap sys:info:pure\n"
          "  --fs-root PATH      Sandbox root for file/fs (enables open)\n"
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

static void sem_print_support(FILE* out, bool json) {
  static const char* items[] = {
      // values/exprs
      "const.i8",
      "const.i32",
      "const.i64",
      "const.array (global init)",
      "const.repeat (global init)",
      "cstr",
      "name",
      "i32.add",
      "binop.add",
      "i32.cmp.eq",
      "ptr.to_i64 (passthrough)",
      "ptr.add",
      "ptr.sub",
      "ptr.cmp.eq",
      "ptr.cmp.ne",

      // memory (MVP)
      "alloca.i8",
      "alloca.i32",
      "alloca.i64",
      "load.i8",
      "load.i32",
      "load.i64",
      "store.i8",
      "store.i32",
      "store.i64",
      "load.ptr",
      "store.ptr",
      "mem.copy",
      "mem.fill",

      // calls
      "decl.fn (extern import)",
      "sym (globals)",
      "ptr.sym (in-module fn by name, or global addr)",
      "call.indirect",

      // statements
      "let",

      // control flow
      "bparam",
      "term.br (+args)",
      "term.cbr / term.condbr",
      "term.switch (i32 scrut, const.i32 cases)",
      "term.ret",
      "term.trap",
      "term.unreachable",
  };

  if (json) {
    fprintf(out, "{\"tool\":\"sem\",\"run_support\":[");
    for (size_t i = 0; i < (sizeof(items) / sizeof(items[0])); i++) {
      if (i) fputc(',', out);
      fputc('"', out);
      fputs(items[i], out);
      fputc('"', out);
    }
    fprintf(out, "]}\n");
    return;
  }

  fprintf(out, "sem --run supports (MVP):\n");
  for (size_t i = 0; i < (sizeof(items) / sizeof(items[0])); i++) {
    fprintf(out, "  - %s\n", items[i]);
  }
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
  dc.meta = NULL;
  dc.meta_len = 0;
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
    free(caps[i].meta);
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

    if (off + 4 > payload_len) return false;
    const uint32_t meta_len = zcl1_read_u32le(payload + off);
    off += 4;
    if (off + meta_len > payload_len) return false;
    const uint8_t* meta = payload + off;
    off += meta_len;

    if (json) {
      if (i) printf(",");
      printf("{\"kind\":\"%.*s\",\"name\":\"%.*s\",\"flags\":%u", (int)kind_len, kind, (int)name_len, name, flags);
      if (meta_len) {
        printf(",\"meta_len\":%u", meta_len);
      }
      printf("}");
    } else {
      printf("  - %.*s:%.*s flags=0x%08x\n", (int)kind_len, kind, (int)name_len, name, flags);
      if (meta_len) {
        printf("    meta_len=%u\n", meta_len);
        (void)meta;
      }
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

static bool sem_has_cap(const dyn_cap_t* caps, uint32_t n, const char* kind, const char* name) {
  if (!caps || !kind || !name) return false;
  for (uint32_t i = 0; i < n; i++) {
    if (!caps[i].kind || !caps[i].name) continue;
    if (strcmp(caps[i].kind, kind) != 0) continue;
    if (strcmp(caps[i].name, name) != 0) continue;
    return true;
  }
  return false;
}

static int sem_do_cat(const sem_cap_t* caps, uint32_t cap_n, const char* fs_root, const char* guest_path) {
  if (!fs_root || fs_root[0] == '\0') {
    fprintf(stderr, "sem: --cat requires --fs-root\n");
    return 2;
  }
  if (!guest_path || guest_path[0] != '/') {
    fprintf(stderr, "sem: --cat requires an absolute guest path like /a.txt\n");
    return 2;
  }

  sir_hosted_zabi_t rt;
  if (!sir_hosted_zabi_init(&rt, (sir_hosted_zabi_cfg_t){.guest_mem_cap = 16u * 1024u * 1024u, .guest_mem_base = 0x10000ull, .caps = caps, .cap_count = cap_n, .fs_root = fs_root})) {
    fprintf(stderr, "sem: failed to init runtime\n");
    return 1;
  }

  // Allocate + write guest path.
  const zi_size32_t guest_path_len = (zi_size32_t)strlen(guest_path);
  const zi_ptr_t guest_path_ptr = sir_zi_alloc(&rt, guest_path_len);
  if (!guest_path_ptr) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: alloc failed\n");
    return 1;
  }
  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(rt.mem, guest_path_ptr, guest_path_len, &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: map failed\n");
    return 1;
  }
  memcpy(w, guest_path, guest_path_len);

  // Params: u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  uint8_t params[20];
  zcl1_write_u32le(params + 0, (uint32_t)((uint64_t)guest_path_ptr & 0xFFFFFFFFu));
  zcl1_write_u32le(params + 4, (uint32_t)(((uint64_t)guest_path_ptr >> 32) & 0xFFFFFFFFu));
  zcl1_write_u32le(params + 8, guest_path_len);
  zcl1_write_u32le(params + 12, 1u << 0); // ZI_FILE_O_READ
  zcl1_write_u32le(params + 16, 0);

  const zi_ptr_t params_ptr = sir_zi_alloc(&rt, (zi_size32_t)sizeof(params));
  if (!params_ptr) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: alloc failed\n");
    return 1;
  }
  if (!sem_guest_mem_map_rw(rt.mem, params_ptr, (zi_size32_t)sizeof(params), &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: map failed\n");
    return 1;
  }
  memcpy(w, params, sizeof(params));

  // kind/name bytes in guest memory.
  const char* kind = "file";
  const char* name = "fs";
  const uint32_t kind_len = (uint32_t)strlen(kind);
  const uint32_t name_len = (uint32_t)strlen(name);
  const zi_ptr_t kind_ptr = sir_zi_alloc(&rt, kind_len);
  const zi_ptr_t name_ptr = sir_zi_alloc(&rt, name_len);
  if (!kind_ptr || !name_ptr) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: alloc failed\n");
    return 1;
  }
  if (!sem_guest_mem_map_rw(rt.mem, kind_ptr, kind_len, &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: map failed\n");
    return 1;
  }
  memcpy(w, kind, kind_len);
  if (!sem_guest_mem_map_rw(rt.mem, name_ptr, name_len, &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: map failed\n");
    return 1;
  }
  memcpy(w, name, name_len);

  // zi_cap_open request: u64 kind_ptr, u32 kind_len, u64 name_ptr, u32 name_len, u32 mode, u64 params_ptr, u32 params_len
  uint8_t open_req[40];
  memset(open_req, 0, sizeof(open_req));
  zcl1_write_u32le(open_req + 0, (uint32_t)((uint64_t)kind_ptr & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 4, (uint32_t)(((uint64_t)kind_ptr >> 32) & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 8, kind_len);
  zcl1_write_u32le(open_req + 12, (uint32_t)((uint64_t)name_ptr & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 16, (uint32_t)(((uint64_t)name_ptr >> 32) & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 20, name_len);
  zcl1_write_u32le(open_req + 24, 0);
  zcl1_write_u32le(open_req + 28, (uint32_t)((uint64_t)params_ptr & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 32, (uint32_t)(((uint64_t)params_ptr >> 32) & 0xFFFFFFFFu));
  zcl1_write_u32le(open_req + 36, (uint32_t)sizeof(params));

  const zi_ptr_t open_req_ptr = sir_zi_alloc(&rt, (zi_size32_t)sizeof(open_req));
  if (!open_req_ptr) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: alloc failed\n");
    return 1;
  }
  if (!sem_guest_mem_map_rw(rt.mem, open_req_ptr, (zi_size32_t)sizeof(open_req), &w) || !w) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: map failed\n");
    return 1;
  }
  memcpy(w, open_req, sizeof(open_req));

  const zi_handle_t h = sir_zi_cap_open(&rt, open_req_ptr);
  if (h < 0) {
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: cap_open failed: %d\n", h);
    return 1;
  }

  const zi_ptr_t buf_ptr = sir_zi_alloc(&rt, 4096);
  if (!buf_ptr) {
    (void)sir_zi_end(&rt, h);
    sir_hosted_zabi_dispose(&rt);
    fprintf(stderr, "sem: alloc failed\n");
    return 1;
  }

  for (;;) {
    const int32_t n = sir_zi_read(&rt, h, buf_ptr, 4096);
    if (n < 0) {
      (void)sir_zi_end(&rt, h);
      sir_hosted_zabi_dispose(&rt);
      fprintf(stderr, "sem: read failed: %d\n", n);
      return 1;
    }
    if (n == 0) break;
    const uint8_t* r = NULL;
    if (!sem_guest_mem_map_ro(rt.mem, buf_ptr, (zi_size32_t)n, &r) || !r) {
      (void)sir_zi_end(&rt, h);
      sir_hosted_zabi_dispose(&rt);
      fprintf(stderr, "sem: map failed\n");
      return 1;
    }
    (void)fwrite(r, 1, (size_t)n, stdout);
  }

  (void)sir_zi_end(&rt, h);
  sir_hosted_zabi_dispose(&rt);
  return 0;
}

static int sem_do_sir_hello(void) {
  // Initialize a VM memory arena.
  sir_vm_t vm;
  if (!sir_vm_init(&vm, (sir_vm_cfg_t){.guest_mem_cap = 1024 * 1024, .guest_mem_base = 0x10000ull, .host = (sir_host_t){0}})) {
    fprintf(stderr, "sem: sircore_vm init failed\n");
    return 1;
  }

  // Hosted zABI implementation, bound to the VM's guest memory.
  sir_hosted_zabi_t hz;
  if (!sir_hosted_zabi_init_with_mem(&hz, &vm.mem, (sir_hosted_zabi_cfg_t){.abi_version = 0x00020005u})) {
    sir_vm_dispose(&vm);
    fprintf(stderr, "sem: hosted zabi init failed\n");
    return 1;
  }

  vm.host = sem_hosted_make_host(&hz);

  static const uint8_t msg[] = "hello from sircore_vm\n";
  const sir_ins_t ins[] = {
      {.k = SIR_INS_WRITE_BYTES, .u.write_bytes = {.h = 1, .bytes = msg, .len = (uint32_t)(sizeof(msg) - 1)}},
      {.k = SIR_INS_EXIT, .u.exit_ = {.code = 0}},
  };

  const int32_t rc = sir_vm_run(&vm, ins, sizeof(ins) / sizeof(ins[0]));
  sir_hosted_zabi_dispose(&hz);
  sir_vm_dispose(&vm);
  return (rc < 0) ? 1 : rc;
}

static int sem_do_sir_module_hello(void) {
  sir_hosted_zabi_t hz;
  if (!sir_hosted_zabi_init(&hz, (sir_hosted_zabi_cfg_t){.abi_version = 0x00020005u, .guest_mem_cap = 1024 * 1024, .guest_mem_base = 0x10000ull})) {
    fprintf(stderr, "sem: hosted zabi init failed\n");
    return 1;
  }

  const sir_host_t host = sem_hosted_make_host(&hz);

  sir_module_builder_t* b = sir_mb_new();
  if (!b) {
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module builder alloc failed\n");
    return 1;
  }

  const sir_type_id_t ty_i32 = sir_mb_type_prim(b, SIR_PRIM_I32);
  const sir_type_id_t ty_i64 = sir_mb_type_prim(b, SIR_PRIM_I64);
  const sir_type_id_t ty_ptr = sir_mb_type_prim(b, SIR_PRIM_PTR);
  if (!ty_i32 || !ty_i64 || !ty_ptr) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module type init failed\n");
    return 1;
  }

  const sir_type_id_t zi_write_params[] = {ty_i32, ty_ptr, ty_i64};
  sir_sig_t zi_write_sig = {
      .params = zi_write_params,
      .param_count = (uint32_t)(sizeof(zi_write_params) / sizeof(zi_write_params[0])),
      .results = NULL,
      .result_count = 0,
  };
  const sir_sym_id_t sym_zi_write = sir_mb_sym_extern_fn(b, "zi_write", zi_write_sig);
  if (!sym_zi_write) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module extern init failed\n");
    return 1;
  }

  const sir_func_id_t f = sir_mb_func_begin(b, "main");
  if (!f || !sir_mb_func_set_entry(b, f) || !sir_mb_func_set_value_count(b, f, 3)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module func init failed\n");
    return 1;
  }

  static const uint8_t msg[] = "hello from sir_module\n";
  if (!sir_mb_emit_const_i32(b, f, 0, 1)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module emit failed\n");
    return 1;
  }
  if (!sir_mb_emit_const_bytes(b, f, 1, 2, msg, (uint32_t)(sizeof(msg) - 1))) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module emit failed\n");
    return 1;
  }

  const sir_val_id_t args[] = {0, 1, 2};
  if (!sir_mb_emit_call_extern(b, f, sym_zi_write, args, (uint32_t)(sizeof(args) / sizeof(args[0])))) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module emit failed\n");
    return 1;
  }
  if (!sir_mb_emit_exit(b, f, 0)) {
    sir_mb_free(b);
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module emit failed\n");
    return 1;
  }

  sir_module_t* m = sir_mb_finalize(b);
  sir_mb_free(b);
  if (!m) {
    sir_hosted_zabi_dispose(&hz);
    fprintf(stderr, "sem: sir module finalize failed\n");
    return 1;
  }

  const int32_t rc = sir_module_run(m, hz.mem, host);
  sir_module_free(m);
  sir_hosted_zabi_dispose(&hz);
  return (rc < 0) ? 1 : rc;
}

int main(int argc, char** argv) {
  bool want_caps = false;
  bool want_support = false;
  bool json = false;
  const char* fs_root = NULL;
  const char* cat_path = NULL;
  bool sir_hello = false;
  bool sir_module_hello = false;
  const char* run_path = NULL;
  sem_diag_format_t diag_format = SEM_DIAG_TEXT;
  bool diag_all = false;
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
    if (strcmp(a, "--print-support") == 0) {
      want_support = true;
      continue;
    }
    if (strcmp(a, "--sir-hello") == 0) {
      sir_hello = true;
      continue;
    }
    if (strcmp(a, "--sir-module-hello") == 0) {
      sir_module_hello = true;
      continue;
    }
    if (strcmp(a, "--run") == 0 && i + 1 < argc) {
      run_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--diagnostics") == 0 && i + 1 < argc) {
      const char* f = argv[++i];
      if (strcmp(f, "text") == 0) diag_format = SEM_DIAG_TEXT;
      else if (strcmp(f, "json") == 0) diag_format = SEM_DIAG_JSON;
      else {
        fprintf(stderr, "sem: bad --diagnostics value (expected text|json)\n");
        sem_free_caps(dyn_caps, dyn_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--all") == 0) {
      diag_all = true;
      continue;
    }
    if (strcmp(a, "--cat") == 0 && i + 1 < argc) {
      cat_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--json") == 0) {
      json = true;
      continue;
    }
    if (strcmp(a, "--fs-root") == 0 && i + 1 < argc) {
      fs_root = argv[++i];
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

  if (want_support) {
    sem_print_support(stdout, json);
    sem_free_caps(dyn_caps, dyn_n);
    return 0;
  }

  if (!want_caps && !cat_path && !sir_hello && !sir_module_hello && !run_path) {
    sem_print_help(stdout);
    sem_free_caps(dyn_caps, dyn_n);
    return 0;
  }

  // If user provided a file sandbox root, ensure file/fs is at least listed (openable depends on fs_root).
  if (fs_root && fs_root[0] != '\0' && !sem_has_cap(dyn_caps, dyn_n, "file", "fs")) {
    if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "file:fs:open,block")) {
      fprintf(stderr, "sem: failed to add file/fs cap\n");
      sem_free_caps(dyn_caps, dyn_n);
      return 2;
    }
  }

  sem_cap_t caps[64];
  const uint32_t cap_n = dyn_n;
  for (uint32_t i = 0; i < cap_n; i++) {
    caps[i].kind = dyn_caps[i].kind;
    caps[i].name = dyn_caps[i].name;
    caps[i].flags = dyn_caps[i].flags;
    caps[i].meta = dyn_caps[i].meta;
    caps[i].meta_len = dyn_caps[i].meta_len;
  }

  if (cat_path) {
    const int rc = sem_do_cat(caps, cap_n, fs_root, cat_path);
    sem_free_caps(dyn_caps, dyn_n);
    return rc;
  }
  if (sir_hello) {
    sem_free_caps(dyn_caps, dyn_n);
    return sem_do_sir_hello();
  }
  if (sir_module_hello) {
    sem_free_caps(dyn_caps, dyn_n);
    return sem_do_sir_module_hello();
  }
  if (run_path) {
    const int rc = sem_run_sir_jsonl_ex(run_path, caps, cap_n, fs_root, diag_format, diag_all);
    sem_free_caps(dyn_caps, dyn_n);
    return rc;
  }

  sem_host_t host;
  sem_host_init(&host, (sem_host_cfg_t){.caps = caps, .cap_count = cap_n});

  const int rc = sem_do_caps(&host, json, tape_out, tape_in, tape_strict);
  sem_free_caps(dyn_caps, dyn_n);
  return rc;
}
