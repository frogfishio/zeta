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
#include <dirent.h>
#include <sys/stat.h>

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

typedef enum sem_check_format {
  SEM_CHECK_TEXT = 0,
  SEM_CHECK_JSON = 1,
} sem_check_format_t;

typedef enum sem_list_format {
  SEM_LIST_TEXT = 0,
  SEM_LIST_JSON = 1,
} sem_list_format_t;

static void sem_print_help(FILE* out) {
  fprintf(out,
          "sem — SIR emulator host frontend (MVP)\n"
          "\n"
          "Usage:\n"
          "  sem [--help] [--version]\n"
          "  sem --print-support [--json]\n"
          "  sem --caps [--json]\n"
          "      [--cap KIND:NAME[:FLAGS]]...\n"
          "      [--enable WHAT]...\n"
          "      [--cap-file-fs] [--cap-async-default] [--cap-sys-info]\n"
          "      [--fs-root PATH]\n"
          "      [--tape-out PATH] [--tape-in PATH] [--tape-lax]\n"
          "  sem --list <input.sir.jsonl|dir>... [--format text|json]\n"
          "  sem --check <input.sir.jsonl|dir>... [--check-run] [--format text|json] [--diagnostics text|json] [--all]\n"
          "  sem --cat GUEST_PATH --fs-root PATH\n"
          "  sem --sir-hello\n"
          "  sem --sir-module-hello\n"
          "  sem --run FILE.sir.jsonl [--trace-jsonl-out PATH] [--coverage-jsonl-out PATH] [--diagnostics text|json] [--fs-root PATH] [--cap ...]\n"
          "  sem --verify FILE.sir.jsonl [--diagnostics text|json]\n"
          "\n"
          "Options:\n"
          "  --help        Show this help message\n"
          "  --version     Show version information (from ./VERSION)\n"
          "  --print-support  Print the supported SIR subset for `sem --run`\n"
          "  --caps        Issue zi_ctl CAPS_LIST and print capabilities\n"
          "  --list        List `*.sir.jsonl` inputs without running\n"
          "  --check       Batch-verify one or more inputs (files or dirs)\n"
          "  --check-run   For --check, run cases (not just verify)\n"
          "  --format      For --check, emit results as: text (default) or json (JSON is written to stderr)\n"
          "  --cat PATH    Read PATH via file/fs and write to stdout\n"
          "  --sir-hello   Run a tiny built-in sircore VM smoke program\n"
          "  --sir-module-hello  Run a tiny built-in sircore module smoke program\n"
          "  --run FILE    Run a small supported SIR subset (MVP)\n"
          "  --verify FILE Validate + lower (no execution)\n"
          "  --trace-jsonl-out PATH  Write execution trace JSONL to PATH (for --run)\n"
          "  --coverage-jsonl-out PATH  Write execution coverage JSONL to PATH (for --run)\n"
          "  --trace-func NAME  For --trace-jsonl-out, only emit events in function NAME\n"
          "  --trace-op OP      For --trace-jsonl-out, only emit step events matching OP (e.g. i32.add, term.cbr)\n"
          "  --json        Emit --caps output as JSON (stdout)\n"
          "  --diagnostics Emit --run/--verify diagnostics as: text (default) or json\n"
          "  --all         For --run/--verify, try to emit multiple diagnostics (best-effort)\n"
          "\n"
          "  --cap KIND:NAME[:FLAGS]\n"
          "      Add a capability entry. FLAGS is a comma-list of:\n"
          "        open (ZI_CAP_CAN_OPEN), pure (ZI_CAP_PURE), block (ZI_CAP_MAY_BLOCK)\n"
          "\n"
          "  --enable WHAT\n"
          "      Convenience enablement. Supported WHAT values:\n"
          "        file:fs | async:default | sys:info | env | argv\n"
          "\n"
          "  --inherit-env    Snapshot host env into zi_ctl env ops (enables env)\n"
          "  --clear-env      Clear env snapshot (enables env, empty)\n"
          "  --env KEY=VAL    Set/override one env var in snapshot (enables env)\n"
          "  --params ARG     Append one guest argv param (enables argv)\n"
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
  fprintf(out, "License: GPLv3+\n");
  fprintf(out, "© 2026 Frogfish — Author: Alexander Croft\n");
}

static bool sem_path_is_dir(const char* path) {
  if (!path || !path[0]) return false;
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISDIR(st.st_mode);
}

static bool sem_path_is_file(const char* path) {
  if (!path || !path[0]) return false;
  struct stat st;
  if (stat(path, &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

static bool sem_has_suffix(const char* s, const char* suffix) {
  if (!s || !suffix) return false;
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);
  if (m > n) return false;
  return memcmp(s + (n - m), suffix, m) == 0;
}

static bool sem_is_sir_jsonl_path(const char* path) {
  return sem_has_suffix(path, ".sir.jsonl");
}

static int sem_do_list_one(const char* path, sem_list_format_t fmt) {
  if (!path || !path[0]) return 2;
  if (sem_path_is_file(path)) {
    if (!sem_is_sir_jsonl_path(path)) {
      fprintf(stderr, "sem: --list: skipping non-.sir.jsonl file: %s\n", path);
      return 0;
    }
    if (fmt == SEM_LIST_JSON) {
      fprintf(stdout, "{\"tool\":\"sem\",\"k\":\"list_case\",\"path\":\"%s\"}\n", path);
    } else {
      fprintf(stdout, "%s\n", path);
    }
    return 0;
  }
  if (!sem_path_is_dir(path)) {
    fprintf(stderr, "sem: --list: not a file/dir: %s\n", path);
    return 2;
  }

  DIR* d = opendir(path);
  if (!d) {
    fprintf(stderr, "sem: --list: failed to open dir: %s\n", path);
    return 2;
  }
  struct dirent* ent = NULL;
  while ((ent = readdir(d)) != NULL) {
    const char* nm = ent->d_name;
    if (!nm || nm[0] == '\0') continue;
    if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
    if (!sem_is_sir_jsonl_path(nm)) continue;

    char full[1024];
    const int n = snprintf(full, sizeof(full), "%s/%s", path, nm);
    if (n <= 0 || (size_t)n >= sizeof(full)) {
      fprintf(stderr, "sem: --list: path too long: %s/%s\n", path, nm);
      closedir(d);
      return 2;
    }
    if (!sem_path_is_file(full)) continue;
    if (fmt == SEM_LIST_JSON) {
      fprintf(stdout, "{\"tool\":\"sem\",\"k\":\"list_case\",\"path\":\"%s\"}\n", full);
    } else {
      fprintf(stdout, "%s\n", full);
    }
  }
  closedir(d);
  return 0;
}

static void sem_emit_check_case(sem_check_format_t fmt, const char* mode, const char* path, bool ok, int tool_rc, int prog_rc) {
  FILE* out = (fmt == SEM_CHECK_JSON) ? stderr : stdout;
  if (fmt != SEM_CHECK_JSON) {
    if (mode && strcmp(mode, "run") == 0) {
      if (ok)
        fprintf(out, "OK   %s rc=%d\n", path, prog_rc);
      else
        fprintf(out, "FAIL %s\n", path);
    } else {
      if (ok)
        fprintf(out, "OK   %s\n", path);
      else
        fprintf(out, "FAIL %s\n", path);
    }
    return;
  }

  // JSONL; one record per case. Keep it small and stable for CI.
  fprintf(out, "{\"tool\":\"sem\",\"k\":\"check_case\",\"mode\":\"%s\",\"path\":\"", mode ? mode : "verify");
  for (const char* p = path; p && *p; p++) {
    const unsigned char ch = (unsigned char)*p;
    if (ch == '\\' || ch == '"') {
      fputc('\\', out);
      fputc((int)ch, out);
    } else if (ch >= 0x20) {
      fputc((int)ch, out);
    }
  }
  fprintf(out, "\",\"ok\":%s", ok ? "true" : "false");
  if (!ok) fprintf(out, ",\"tool_rc\":%d", tool_rc);
  if (mode && strcmp(mode, "run") == 0) {
    if (ok) fprintf(out, ",\"rc\":%d", prog_rc);
  }
  fprintf(out, "}\n");
}

static int sem_do_check_one(const char* path, bool do_run, sem_run_host_cfg_t host_cfg, sem_check_format_t check_format,
                            sem_diag_format_t diag_format, bool diag_all) {
  if (do_run) {
    int prog_rc = 0;
    const int tool_rc = sem_run_sir_jsonl_capture_host_ex(path, host_cfg, diag_format, diag_all, &prog_rc);
    sem_emit_check_case(check_format, "run", path, tool_rc == 0, tool_rc, prog_rc);
    return tool_rc;
  }

  const int rc = sem_verify_sir_jsonl_ex(path, diag_format, diag_all);
  sem_emit_check_case(check_format, "verify", path, rc == 0, rc, 0);
  return rc;
}

static int sem_do_check_dir(const char* dir, bool do_run, sem_run_host_cfg_t host_cfg, sem_check_format_t check_format,
                            sem_diag_format_t diag_format, bool diag_all, uint32_t* inout_ok, uint32_t* inout_fail) {
  if (!dir || !inout_ok || !inout_fail) return 2;
  DIR* d = opendir(dir);
  if (!d) {
    fprintf(stderr, "sem: --check: failed to open dir: %s\n", dir);
    return 2;
  }

  struct dirent* ent = NULL;
  while ((ent = readdir(d)) != NULL) {
    const char* nm = ent->d_name;
    if (!nm || nm[0] == '\0') continue;
    if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
    if (!sem_is_sir_jsonl_path(nm)) continue;

    char full[1024];
    const int n = snprintf(full, sizeof(full), "%s/%s", dir, nm);
    if (n <= 0 || (size_t)n >= sizeof(full)) {
      fprintf(stderr, "sem: --check: path too long: %s/%s\n", dir, nm);
      closedir(d);
      return 2;
    }
    if (!sem_path_is_file(full)) continue;

    const int rc = sem_do_check_one(full, do_run, host_cfg, check_format, diag_format, diag_all);
    if (rc == 0)
      (*inout_ok)++;
    else
      (*inout_fail)++;
  }

  closedir(d);
  return 0;
}

static void sem_print_support(FILE* out, bool json) {
  static const char* items[] = {
      // values/exprs
      "const.i1",
      "const.i8",
      "const.i16",
      "const.i32",
      "const.i64",
      "const.bool",
      "const.f32 (bits, NaN-canon)",
      "const.f64 (bits, NaN-canon)",
      "const.zero (global init)",
      "const.array (global init)",
      "const.repeat (global init)",
      "const.struct (global init)",
      "cstr",
      "name",
      "i32.add",
      "i32.sub",
      "i32.mul",
      "i32.and",
      "i32.or",
      "i32.xor",
      "i32.not",
      "i32.neg",
      "i32.shl",
      "i32.shr.s / i32.shr.u",
      "i32.div.s.sat / i32.div.u.sat",
      "i32.div.s.trap",
      "i32.rem.s.sat / i32.rem.u.sat",
      "binop.add",
      "i32.cmp.eq",
      "i32.cmp.ne",
      "i32.cmp.slt / sle / sgt / sge",
      "i32.cmp.ult / ule / ugt / uge",
      "i32.zext.i8",
      "i64.zext.i32",
      "i32.trunc.i64",
      "bool.not",
      "bool.and / bool.or / bool.xor",
      "ptr.to_i64",
      "ptr.from_i64",
      "ptr.add",
      "ptr.sub",
      "ptr.cmp.eq",
      "ptr.cmp.ne",
      "ptr.sizeof",
      "ptr.alignof",
      "ptr.offset",
      "select",

      // memory (MVP)
      "alloca (core, typed)",
      "alloca.i8",
      "alloca.i16",
      "alloca.i32",
      "alloca.i64",
      "alloca.f32",
      "alloca.f64",
      "load.i8",
      "load.i16",
      "load.i32",
      "load.i64",
      "load.f32",
      "load.f64",
      "store.i8",
      "store.i16",
      "store.i32",
      "store.i64",
      "store.f32",
      "store.f64",
      "load.ptr",
      "store.ptr",
      "mem.copy",
      "mem.fill",
      "atomic.load.i8/i16/i32/i64 (atomics:v1, single-thread semantics; ordering validated, ignored)",
      "atomic.store.i8/i16/i32/i64 (atomics:v1, single-thread semantics; ordering validated, ignored)",
      "atomic.rmw.*.i8/i16/i32/i64 (atomics:v1, returns old only; ordering validated, ignored)",
      "atomic.cmpxchg.i8/i16/i32/i64 (atomics:v1, returns old only; ok=(old==expected); ordering validated, ignored)",
      "load.vec (simd:v1, executed as scalar lanes)",
      "store.vec (simd:v1, executed as scalar lanes)",
      "vec.splat (simd:v1, executed as scalar lanes)",
      "vec.add (simd:v1, i32 lanes only)",
      "vec.cmp.eq (simd:v1, i32 -> bool lanes)",
      "vec.cmp.lt (simd:v1, i32 -> bool lanes)",
      "vec.select (simd:v1, bool mask + i32 lanes)",
      "vec.extract (simd:v1, i32/bool lanes)",
      "vec.replace (simd:v1, i32/bool lanes)",
      "vec.shuffle (simd:v1, i32 lanes only)",

      // calls
      "decl.fn (extern import)",
      "sym (globals)",
      "ptr.sym (in-module fn by name, or global addr)",
      "fun.sym (fun:v1, MVP)",
      "fun.cmp.eq / fun.cmp.ne (fun:v1)",
      "call.fun (fun:v1, MVP)",
      "closure.sym / closure.make (closure:v1, MVP)",
      "closure.code / closure.env (closure:v1)",
      "closure.cmp.eq / closure.cmp.ne (closure:v1)",
      "call.closure (closure:v1, MVP)",
      "call",
      "call.indirect",

      // adt pack (adt:v1, MVP)
      "adt.make",
      "adt.tag",
      "adt.is",
      "adt.get",

      // sem intent (sem:v1)
      "sem.if",
      "sem.cond",
      "sem.and_sc / sem.or_sc",
      "sem.switch",
      "sem.match_sum",
      "sem.defer",
      "sem.scope",
      "sem.while",
      "sem.break (MVP)",
      "sem.continue (MVP)",

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

static void sem_free_argv(char** argv, uint32_t argc) {
  if (!argv) return;
  for (uint32_t i = 0; i < argc; i++) free(argv[i]);
}

static void sem_free_env(sem_env_kv_t* env, uint32_t n) {
  if (!env) return;
  for (uint32_t i = 0; i < n; i++) {
    free((void*)env[i].key);
    free((void*)env[i].val);
  }
}

static bool sem_env_set(sem_env_kv_t* env, uint32_t* inout_n, uint32_t max, const char* key, const char* val) {
  if (!env || !inout_n || !key || !key[0]) return false;
  const char* v = val ? val : "";

  for (uint32_t i = 0; i < *inout_n; i++) {
    if (!env[i].key) continue;
    if (strcmp(env[i].key, key) != 0) continue;
    char* nv = sem_strdup(v);
    if (!nv) return false;
    free((void*)env[i].val);
    env[i].val = nv;
    return true;
  }

  if (*inout_n >= max) return false;
  char* nk = sem_strdup(key);
  char* nv = sem_strdup(v);
  if (!nk || !nv) {
    free(nk);
    free(nv);
    return false;
  }
  env[*inout_n] = (sem_env_kv_t){.key = nk, .val = nv};
  (*inout_n)++;
  return true;
}

static bool sem_env_set_kv(sem_env_kv_t* env, uint32_t* inout_n, uint32_t max, const char* kv) {
  if (!kv) return false;
  const char* eq = strchr(kv, '=');
  if (!eq || eq == kv) return false;
  const size_t klen = (size_t)(eq - kv);
  if (klen >= 1024) return false;
  char kbuf[1024];
  memcpy(kbuf, kv, klen);
  kbuf[klen] = '\0';
  return sem_env_set(env, inout_n, max, kbuf, eq + 1);
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
  const char* verify_path = NULL;
  const char* check_paths_buf[256];
  const char** check_paths = NULL;
  uint32_t check_path_count = 0;
  bool check_mode = false;
  const char* list_paths_buf[256];
  const char** list_paths = NULL;
  uint32_t list_path_count = 0;
  bool list_mode = false;
  sem_diag_format_t diag_format = SEM_DIAG_TEXT;
  bool diag_all = false;
  const char* tape_out = NULL;
  const char* tape_in = NULL;
  bool tape_strict = true;
  bool check_run = false;
  sem_check_format_t check_format = SEM_CHECK_TEXT;
  sem_list_format_t list_format = SEM_LIST_TEXT;
  const char* format_opt = NULL;
  const char* trace_jsonl_out = NULL;
  const char* coverage_jsonl_out = NULL;
  const char* trace_func = NULL;
  const char* trace_op = NULL;

  dyn_cap_t dyn_caps[64];
  uint32_t dyn_n = 0;
  memset(dyn_caps, 0, sizeof(dyn_caps));

  bool argv_enabled = false;
  char* guest_argv[128];
  uint32_t guest_argc = 0;
  memset(guest_argv, 0, sizeof(guest_argv));

  bool env_enabled = false;
  sem_env_kv_t env_buf[256];
  uint32_t env_n = 0;
  memset(env_buf, 0, sizeof(env_buf));

  bool env_inherited = false;

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
  extern char** environ;
#endif

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--help") == 0) {
      sem_print_help(stdout);
      sem_free_caps(dyn_caps, dyn_n);
      sem_free_argv(guest_argv, guest_argc);
      sem_free_env(env_buf, env_n);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      sem_print_version(stdout);
      sem_free_caps(dyn_caps, dyn_n);
      sem_free_argv(guest_argv, guest_argc);
      sem_free_env(env_buf, env_n);
      return 0;
    }
    if (strcmp(a, "--caps") == 0) {
      want_caps = true;
      continue;
    }
    if (strcmp(a, "--list") == 0) {
      list_mode = true;
      continue;
    }
    if (strcmp(a, "--check") == 0) {
      check_mode = true;
      continue;
    }
    if (strcmp(a, "--check-run") == 0) {
      check_run = true;
      check_mode = true;
      continue;
    }
    if (strcmp(a, "--format") == 0 && i + 1 < argc) {
      format_opt = argv[++i];
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
    if (strcmp(a, "--verify") == 0 && i + 1 < argc) {
      verify_path = argv[++i];
      continue;
    }
    if (strcmp(a, "--diagnostics") == 0 && i + 1 < argc) {
      const char* f = argv[++i];
      if (strcmp(f, "text") == 0) diag_format = SEM_DIAG_TEXT;
      else if (strcmp(f, "json") == 0) diag_format = SEM_DIAG_JSON;
      else {
        fprintf(stderr, "sem: bad --diagnostics value (expected text|json)\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
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
    if (strcmp(a, "--trace-jsonl-out") == 0 && i + 1 < argc) {
      trace_jsonl_out = argv[++i];
      continue;
    }
    if (strcmp(a, "--coverage-jsonl-out") == 0 && i + 1 < argc) {
      coverage_jsonl_out = argv[++i];
      continue;
    }
    if (strcmp(a, "--trace-func") == 0 && i + 1 < argc) {
      trace_func = argv[++i];
      continue;
    }
    if (strcmp(a, "--trace-op") == 0 && i + 1 < argc) {
      trace_op = argv[++i];
      continue;
    }

    if (strcmp(a, "--enable") == 0 && i + 1 < argc) {
      const char* what = argv[++i];
      if (!what || !what[0]) {
        fprintf(stderr, "sem: bad --enable value\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      if (strcmp(what, "env") == 0) {
        env_enabled = true;
        continue;
      }
      if (strcmp(what, "argv") == 0) {
        argv_enabled = true;
        continue;
      }
      if (strcmp(what, "file:fs") == 0) {
        if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "file:fs:open,block")) {
          fprintf(stderr, "sem: failed to add cap\n");
          sem_free_caps(dyn_caps, dyn_n);
          sem_free_argv(guest_argv, guest_argc);
          sem_free_env(env_buf, env_n);
          return 2;
        }
        continue;
      }
      if (strcmp(what, "async:default") == 0) {
        if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "async:default:open,block")) {
          fprintf(stderr, "sem: failed to add cap\n");
          sem_free_caps(dyn_caps, dyn_n);
          sem_free_argv(guest_argv, guest_argc);
          sem_free_env(env_buf, env_n);
          return 2;
        }
        continue;
      }
      if (strcmp(what, "sys:info") == 0) {
        if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "sys:info:pure")) {
          fprintf(stderr, "sem: failed to add cap\n");
          sem_free_caps(dyn_caps, dyn_n);
          sem_free_argv(guest_argv, guest_argc);
          sem_free_env(env_buf, env_n);
          return 2;
        }
        continue;
      }

      fprintf(stderr, "sem: unknown --enable value: %s\n", what);
      sem_free_caps(dyn_caps, dyn_n);
      sem_free_argv(guest_argv, guest_argc);
      sem_free_env(env_buf, env_n);
      return 2;
    }

    if (strcmp(a, "--params") == 0 && i + 1 < argc) {
      const char* p = argv[++i];
      if (!p) p = "";
      if (guest_argc >= (uint32_t)(sizeof(guest_argv) / sizeof(guest_argv[0]))) {
        fprintf(stderr, "sem: too many --params\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      guest_argv[guest_argc] = sem_strdup(p);
      if (!guest_argv[guest_argc]) {
        fprintf(stderr, "sem: out of memory\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      guest_argc++;
      argv_enabled = true;
      continue;
    }

    if (strcmp(a, "--inherit-env") == 0) {
      env_enabled = true;
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
      if (!env_inherited) {
        env_inherited = true;
        if (environ) {
          for (char** p = environ; *p; p++) {
            (void)sem_env_set_kv(env_buf, &env_n, (uint32_t)(sizeof(env_buf) / sizeof(env_buf[0])), *p);
          }
        }
      }
#endif
      continue;
    }

    if (strcmp(a, "--clear-env") == 0) {
      env_enabled = true;
      sem_free_env(env_buf, env_n);
      env_n = 0;
      continue;
    }

    if (strcmp(a, "--env") == 0 && i + 1 < argc) {
      env_enabled = true;
      const char* kv = argv[++i];
      if (!sem_env_set_kv(env_buf, &env_n, (uint32_t)(sizeof(env_buf) / sizeof(env_buf[0])), kv)) {
        fprintf(stderr, "sem: bad --env (expected KEY=VAL)\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      continue;
    }

    if (strcmp(a, "--cap") == 0 && i + 1 < argc) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), argv[++i])) {
        fprintf(stderr, "sem: bad --cap spec\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-file-fs") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "file:fs:open,block")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-async-default") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])),
                       "async:default:open,block")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      continue;
    }
    if (strcmp(a, "--cap-sys-info") == 0) {
      if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "sys:info:pure")) {
        fprintf(stderr, "sem: failed to add cap\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      continue;
    }

    if (check_mode && a[0] != '-') {
      if (check_path_count >= (uint32_t)(sizeof(check_paths_buf) / sizeof(check_paths_buf[0]))) {
        fprintf(stderr, "sem: --check: too many paths\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      check_paths_buf[check_path_count++] = a;
      continue;
    }
    if (list_mode && a[0] != '-') {
      if (list_path_count >= (uint32_t)(sizeof(list_paths_buf) / sizeof(list_paths_buf[0]))) {
        fprintf(stderr, "sem: --list: too many paths\n");
        sem_free_caps(dyn_caps, dyn_n);
        sem_free_argv(guest_argv, guest_argc);
        sem_free_env(env_buf, env_n);
        return 2;
      }
      list_paths_buf[list_path_count++] = a;
      continue;
    }

    fprintf(stderr, "sem: unknown argument: %s\n", a);
    sem_print_help(stderr);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 2;
  }

  if (check_path_count) check_paths = check_paths_buf;
  if (list_path_count) list_paths = list_paths_buf;

  if (format_opt && format_opt[0]) {
    if (strcmp(format_opt, "text") == 0) {
      check_format = SEM_CHECK_TEXT;
      list_format = SEM_LIST_TEXT;
    } else if (strcmp(format_opt, "json") == 0) {
      check_format = SEM_CHECK_JSON;
      list_format = SEM_LIST_JSON;
    } else {
      fprintf(stderr, "sem: bad --format value (expected text|json)\n");
      sem_free_caps(dyn_caps, dyn_n);
      sem_free_argv(guest_argv, guest_argc);
      sem_free_env(env_buf, env_n);
      return 2;
    }
  }

  if (want_support) {
    sem_print_support(stdout, json);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 0;
  }

  if (run_path && verify_path) {
    fprintf(stderr, "sem: choose either --run or --verify\n");
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 2;
  }
  if (list_path_count && check_path_count) {
    fprintf(stderr, "sem: choose either --list or --check\n");
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 2;
  }

  if (check_mode && check_path_count == 0) {
    fprintf(stderr, "sem: --check: expected at least one file/dir path\n");
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 2;
  }
  if (list_mode && list_path_count == 0) {
    fprintf(stderr, "sem: --list: expected at least one file/dir path\n");
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 2;
  }

  if (!want_caps && !cat_path && !sir_hello && !sir_module_hello && !run_path && !verify_path && !check_path_count && !list_path_count) {
    sem_print_help(stdout);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return 0;
  }

  // If user provided a file sandbox root, ensure file/fs is at least listed (openable depends on fs_root).
  if (fs_root && fs_root[0] != '\0' && !sem_has_cap(dyn_caps, dyn_n, "file", "fs")) {
    if (!sem_add_cap(dyn_caps, &dyn_n, (uint32_t)(sizeof(dyn_caps) / sizeof(dyn_caps[0])), "file:fs:open,block")) {
      fprintf(stderr, "sem: failed to add file/fs cap\n");
      sem_free_caps(dyn_caps, dyn_n);
      sem_free_argv(guest_argv, guest_argc);
      sem_free_env(env_buf, env_n);
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

  const sem_run_host_cfg_t host_cfg = {
      .caps = caps,
      .cap_count = cap_n,
      .fs_root = fs_root,
      .argv_enabled = argv_enabled,
      .argv = (const char* const*)guest_argv,
      .argv_count = guest_argc,
      .env_enabled = env_enabled,
      .env = env_buf,
      .env_count = env_n,
  };

  if (cat_path) {
    const int rc = sem_do_cat(caps, cap_n, fs_root, cat_path);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return rc;
  }
  if (list_path_count) {
    int tool_rc = 0;
    for (uint32_t i = 0; i < list_path_count; i++) {
      const char* p = list_paths[i];
      if (!p || p[0] == '\0') continue;
      const int rc = sem_do_list_one(p, list_format);
      if (rc != 0) tool_rc = rc;
    }
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return tool_rc;
  }
  if (sir_hello) {
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return sem_do_sir_hello();
  }
  if (sir_module_hello) {
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return sem_do_sir_module_hello();
  }
  if (run_path) {
    const int rc = sem_run_sir_jsonl_events_host_ex(run_path, host_cfg, diag_format, diag_all, trace_jsonl_out, coverage_jsonl_out, trace_func, trace_op);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return rc;
  }
  if (verify_path) {
    const int rc = sem_verify_sir_jsonl_ex(verify_path, diag_format, diag_all);
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    return rc;
  }
  if (check_path_count) {
    uint32_t ok = 0, fail = 0;
    int tool_rc = 0;

    for (uint32_t i = 0; i < check_path_count; i++) {
      const char* p = check_paths[i];
      if (!p || p[0] == '\0') continue;
      if (sem_path_is_dir(p)) {
        const int rc = sem_do_check_dir(p, check_run, host_cfg, check_format, diag_format, diag_all, &ok, &fail);
        if (rc != 0) tool_rc = rc;
      } else if (sem_path_is_file(p)) {
        if (!sem_is_sir_jsonl_path(p)) {
          fprintf(stderr, "sem: --check: skipping non-.sir.jsonl file: %s\n", p);
          continue;
        }
        const int rc = sem_do_check_one(p, check_run, host_cfg, check_format, diag_format, diag_all);
        if (rc == 0)
          ok++;
        else
          fail++;
      } else {
        fprintf(stderr, "sem: --check: not a file/dir: %s\n", p);
        tool_rc = 2;
      }
    }

    if (check_format == SEM_CHECK_JSON) {
      fprintf(stderr, "{\"tool\":\"sem\",\"k\":\"check_summary\",\"ok\":%u,\"fail\":%u}\n", (unsigned)ok, (unsigned)fail);
    } else {
      fprintf(stdout, "sem: --check: ok=%u fail=%u\n", (unsigned)ok, (unsigned)fail);
    }
    sem_free_caps(dyn_caps, dyn_n);
    sem_free_argv(guest_argv, guest_argc);
    sem_free_env(env_buf, env_n);
    if (tool_rc != 0) return tool_rc;
    return (fail == 0) ? 0 : 1;
  }

  sem_host_t host;
  sem_host_init(&host, (sem_host_cfg_t){
                         .caps = caps,
                         .cap_count = cap_n,
                         .argv_enabled = argv_enabled,
                         .argv = (const char* const*)guest_argv,
                         .argv_count = guest_argc,
                         .env_enabled = env_enabled,
                         .env = env_buf,
                         .env_count = env_n,
                     });

  const int rc = sem_do_caps(&host, json, tape_out, tape_in, tape_strict);
  sem_free_caps(dyn_caps, dyn_n);
  sem_free_argv(guest_argv, guest_argc);
  sem_free_env(env_buf, env_n);
  return rc;
}
