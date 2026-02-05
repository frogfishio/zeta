// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compiler_zasm_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void zasm_err_nodef(SirProgram* p, int64_t node_id, const char* node_tag, const char* fmt, ...) {
  if (!p || !fmt) return;

  // Save current diagnostic context.
  const char* saved_kind = p->cur_kind;
  int64_t saved_id = p->cur_rec_id;
  const char* saved_tag = p->cur_rec_tag;

  p->cur_kind = "node";
  p->cur_rec_id = node_id;
  p->cur_rec_tag = node_tag;

  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  va_start(ap, fmt);
  char stack_buf[512];
  char* heap_buf = NULL;
  const char* msg = NULL;
  if (need >= 0 && (size_t)need < sizeof(stack_buf)) {
    vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    msg = stack_buf;
  } else if (need >= 0) {
    heap_buf = (char*)malloc((size_t)need + 1);
    if (heap_buf) {
      vsnprintf(heap_buf, (size_t)need + 1, fmt, ap);
      msg = heap_buf;
    } else {
      msg = "(out of memory while formatting diagnostic)";
    }
  } else {
    msg = "(diagnostic formatting failed)";
  }
  va_end(ap);

  errf(p, "%s", msg);

  free(heap_buf);

  // Restore context.
  p->cur_kind = saved_kind;
  p->cur_rec_id = saved_id;
  p->cur_rec_tag = saved_tag;
}

void zasm_err_node_codef(SirProgram* p, int64_t node_id, const char* node_tag, const char* code, const char* fmt, ...) {
  if (!p || !fmt) return;

  // Save current diagnostic context.
  const char* saved_kind = p->cur_kind;
  int64_t saved_id = p->cur_rec_id;
  const char* saved_tag = p->cur_rec_tag;

  p->cur_kind = "node";
  p->cur_rec_id = node_id;
  p->cur_rec_tag = node_tag;

  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  va_start(ap, fmt);
  char stack_buf[512];
  char* heap_buf = NULL;
  const char* msg = NULL;
  if (need >= 0 && (size_t)need < sizeof(stack_buf)) {
    vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    msg = stack_buf;
  } else if (need >= 0) {
    heap_buf = (char*)malloc((size_t)need + 1);
    if (heap_buf) {
      vsnprintf(heap_buf, (size_t)need + 1, fmt, ap);
      msg = heap_buf;
    } else {
      msg = "(out of memory while formatting diagnostic)";
    }
  } else {
    msg = "(diagnostic formatting failed)";
  }
  va_end(ap);

  err_codef(p, code, "%s", msg);

  free(heap_buf);

  // Restore context.
  p->cur_kind = saved_kind;
  p->cur_rec_id = saved_id;
  p->cur_rec_tag = saved_tag;
}
