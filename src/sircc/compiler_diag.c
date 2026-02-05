// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool want_color(const SirccOptions* opt) {
  if (!opt) return false;
  if (opt->color == SIRCC_COLOR_NEVER) return false;
  if (opt->color == SIRCC_COLOR_ALWAYS) return true;
  // auto
  if (!isatty(fileno(stderr))) return false;
  const char* term = getenv("TERM");
  if (!term || strcmp(term, "dumb") == 0) return false;
  return true;
}

SirDiagSaved sir_diag_push(SirProgram* p, const char* kind, int64_t rec_id, const char* rec_tag) {
  SirDiagSaved saved = {0};
  if (!p) return saved;
  saved.kind = p->cur_kind;
  saved.rec_id = p->cur_rec_id;
  saved.rec_tag = p->cur_rec_tag;
  p->cur_kind = kind;
  p->cur_rec_id = rec_id;
  p->cur_rec_tag = rec_tag;
  return saved;
}

SirDiagSaved sir_diag_push_node(SirProgram* p, const NodeRec* n) {
  return sir_diag_push(p, "node", n ? n->id : -1, n ? n->tag : NULL);
}

void sir_diag_pop(SirProgram* p, SirDiagSaved saved) {
  if (!p) return;
  p->cur_kind = saved.kind;
  p->cur_rec_id = saved.rec_id;
  p->cur_rec_tag = saved.rec_tag;
}

void bump_exit_code(SirProgram* p, int code) {
  if (!p) return;
  // Keep internal errors sticky, otherwise prefer toolchain over generic error.
  if (p->exit_code == SIRCC_EXIT_INTERNAL) return;
  if (code == SIRCC_EXIT_INTERNAL) {
    p->exit_code = SIRCC_EXIT_INTERNAL;
    return;
  }
  if (code == SIRCC_EXIT_TOOLCHAIN) {
    p->exit_code = SIRCC_EXIT_TOOLCHAIN;
    return;
  }
  if (p->exit_code == 0) p->exit_code = code;
}

static void err_vimpl(SirProgram* p, const char* diag_code, const char* fmt, va_list ap0) {
  const SirccOptions* opt = p ? p->opt : NULL;
  bool as_json = opt && opt->diagnostics == SIRCC_DIAG_JSON;
  bool color = want_color(opt);
  const char* json_code = diag_code;
  if (as_json && (!json_code || !*json_code)) json_code = "sircc.error";

  va_list ap;
  va_copy(ap, ap0);

  // Determine best-effort location.
  const char* file = NULL;
  int64_t line = 0;
  int64_t col = 0;
  int64_t src_ref = -1;
  if (p) {
    src_ref = p->cur_src_ref;
    if (p->cur_loc.line > 0) {
      file = p->cur_loc.unit ? p->cur_loc.unit : p->cur_path;
      line = p->cur_loc.line;
      col = p->cur_loc.col;
    } else if (p->cur_src_ref >= 0 && (size_t)p->cur_src_ref < p->srcs_cap) {
      SrcRec* sr = p->srcs[p->cur_src_ref];
      if (sr) {
        file = sr->file ? sr->file : p->cur_path;
        line = sr->line;
        col = sr->col;
      }
    } else if (p->cur_path) {
      file = p->cur_path;
      line = (int64_t)p->cur_line;
    }
  }

  // Format message once (so JSON mode can embed it).
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  va_copy(ap, ap0);
  char* msg = NULL;
  char stack_buf[512];
  if (need >= 0 && (size_t)need < sizeof(stack_buf)) {
    vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    msg = stack_buf;
  } else if (need >= 0) {
    msg = (char*)malloc((size_t)need + 1);
    if (msg) vsnprintf(msg, (size_t)need + 1, fmt, ap);
  }
  va_end(ap);

  if (as_json) {
    fprintf(stderr, "{\"ir\":\"sir-v1.0\",\"k\":\"diag\",\"level\":\"error\",\"msg\":");
    json_write_escaped(stderr, msg ? msg : "(unknown)");
    if (json_code && *json_code) {
      fprintf(stderr, ",\"code\":");
      json_write_escaped(stderr, json_code);
    }
    if (p && p->cur_kind) {
      fprintf(stderr, ",\"about\":{");
      fprintf(stderr, "\"k\":");
      json_write_escaped(stderr, p->cur_kind);
      if (p->cur_rec_id >= 0) fprintf(stderr, ",\"id\":%lld", (long long)p->cur_rec_id);
      if (p->cur_rec_tag) {
        fprintf(stderr, ",\"tag\":");
        json_write_escaped(stderr, p->cur_rec_tag);
      }
      fprintf(stderr, "}");
    }
    if (src_ref >= 0) fprintf(stderr, ",\"src_ref\":%lld", (long long)src_ref);
    if (file || line > 0 || col > 0) {
      fprintf(stderr, ",\"loc\":{");
      bool any = false;
      if (file) {
        fprintf(stderr, "\"unit\":");
        json_write_escaped(stderr, file);
        any = true;
      }
      if (line > 0) {
        if (any) fprintf(stderr, ",");
        fprintf(stderr, "\"line\":%lld", (long long)line);
        any = true;
      }
      if (col > 0) {
        if (any) fprintf(stderr, ",");
        fprintf(stderr, "\"col\":%lld", (long long)col);
      }
      fprintf(stderr, "}");
    }

    // JSON source context (mirrors --diag-context in text mode).
    if (p && opt && opt->diag_context > 0 && p->cur_path && p->cur_line > 0) {
      size_t ctx = (size_t)opt->diag_context;
      if (ctx > 200) ctx = 200;  // avoid accidentally emitting huge blobs

      FILE* ctxf = fopen(p->cur_path, "rb");
      if (ctxf) {
        size_t lo = (p->cur_line > ctx) ? (p->cur_line - ctx) : 1;
        size_t hi = p->cur_line + ctx;
        fprintf(stderr, ",\"context\":[");

        char* lbuf = NULL;
        size_t lcap = 0;
        size_t llen = 0;
        size_t lno = 0;
        bool first = true;
        while (read_line(ctxf, &lbuf, &lcap, &llen)) {
          lno++;
          if (lno < lo) continue;
          if (lno > hi) break;
          if (!first) fprintf(stderr, ",");
          first = false;
          fprintf(stderr, "{\"line\":%zu,\"text\":", lno);
          json_write_escaped(stderr, lbuf ? lbuf : "");
          fprintf(stderr, "}");
        }
        free(lbuf);
        fclose(ctxf);
        fprintf(stderr, "],\"context_line\":%zu", (size_t)p->cur_line);
      }
    }

    fprintf(stderr, "}\n");
  } else {
    if (file) {
      if (line > 0) {
        if (col > 0) fprintf(stderr, "%s:%lld:%lld: ", file, (long long)line, (long long)col);
        else fprintf(stderr, "%s:%lld: ", file, (long long)line);
      } else {
        fprintf(stderr, "%s: ", file);
      }
    }
    if (color) fprintf(stderr, "\x1b[31merror:\x1b[0m ");
    else fprintf(stderr, "error: ");
    fprintf(stderr, "%s\n", msg ? msg : "(unknown)");
    if (diag_code && *diag_code) fprintf(stderr, "  code: %s\n", diag_code);

    if (p && p->cur_kind) {
      fprintf(stderr, "  record: k=%s", p->cur_kind);
      if (p->cur_rec_id >= 0) fprintf(stderr, " id=%lld", (long long)p->cur_rec_id);
      if (p->cur_rec_tag) fprintf(stderr, " tag=%s", p->cur_rec_tag);
      fputc('\n', stderr);
    }

    // Print source context from the JSONL input (best-effort). Keep this out of JSON diagnostics.
    if (p && opt && opt->diag_context > 0 && p->cur_path && p->cur_line > 0) {
      FILE* ctxf = fopen(p->cur_path, "rb");
      if (ctxf) {
        size_t ctx = (size_t)opt->diag_context;
        size_t lo = (p->cur_line > ctx) ? (p->cur_line - ctx) : 1;
        size_t hi = p->cur_line + ctx;

        // Determine width for line numbers.
        size_t tmp = hi;
        int w = 1;
        while (tmp >= 10) {
          tmp /= 10;
          w++;
        }

        fprintf(stderr, "  |\n");
        char* lbuf = NULL;
        size_t lcap = 0;
        size_t llen = 0;
        size_t lno = 0;
        while (read_line(ctxf, &lbuf, &lcap, &llen)) {
          lno++;
          if (lno < lo) continue;
          if (lno > hi) break;
          char marker = (lno == p->cur_line) ? '>' : ' ';
          fprintf(stderr, "%c %*zu| %s\n", marker, w, lno, lbuf ? lbuf : "");
        }
        free(lbuf);
        fclose(ctxf);
        fprintf(stderr, "  |\n");
      }
    }
  }

  if (msg && msg != stack_buf) free(msg);
}

void errf(SirProgram* p, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  err_vimpl(p, NULL, fmt, ap);
  va_end(ap);
}

void err_codef(SirProgram* p, const char* code, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  err_vimpl(p, code, fmt, ap);
  va_end(ap);
}
