#include "zi_sys_loop25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"
#include "zi_zcl1.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <time.h>
#endif

// ---- cap descriptor ----

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_SYS,
    .name = ZI_CAP_NAME_LOOP,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN | ZI_CAP_MAY_BLOCK,
    .meta = NULL,
    .meta_len = 0,
};

const zi_cap_v1 *zi_sys_loop25_cap(void) { return &CAP; }

int zi_sys_loop25_register(void) { return zi_cap_register(&CAP); }

// ---- helpers ----

static void write_u32le(uint8_t *p, uint32_t v) { zi_zcl1_write_u32(p, v); }

static void write_u64le(uint8_t *p, uint64_t v) {
  write_u32le(p + 0, (uint32_t)(v & 0xFFFFFFFFu));
  write_u32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_u32le(const uint8_t *p) { return zi_zcl1_read_u32(p); }

static uint64_t read_u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)read_u32le(p + 0);
  uint64_t hi = (uint64_t)read_u32le(p + 4);
  return lo | (hi << 32);
}

static uint64_t now_monotonic_ns(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
  return 0;
}

static int64_t ns_to_ms_ceil(uint64_t ns) {
  if (ns == 0) return 0;
  uint64_t ms = (ns + 999999ull) / 1000000ull;
  if (ms > 0x7FFFFFFFull) return 0x7FFFFFFF;
  return (int64_t)ms;
}

// ---- handle implementation ----

enum {
  ZI_SYS_LOOP_EV_READY = 1,
  ZI_SYS_LOOP_EV_TIMER = 2,
};

enum {
  ZI_SYS_LOOP_E_READABLE = 0x1,
  ZI_SYS_LOOP_E_WRITABLE = 0x2,
  ZI_SYS_LOOP_E_HUP = 0x4,
  ZI_SYS_LOOP_E_ERROR = 0x8,
};

typedef struct {
  uint64_t watch_id;
  zi_handle_t h;
  uint32_t events;
  int in_use;
} zi_sys_loop_watch;

typedef struct {
  uint64_t timer_id;
  uint64_t due_ns;
  uint64_t interval_ns;
  int in_use;
} zi_sys_loop_timer;

#ifndef ZI_SYS_LOOP_MAX_WATCH
#define ZI_SYS_LOOP_MAX_WATCH 1024
#endif

#ifndef ZI_SYS_LOOP_MAX_TIMERS
#define ZI_SYS_LOOP_MAX_TIMERS 1024
#endif

typedef struct {
  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t outbuf[65536];
  uint32_t out_len;
  uint32_t out_off;

  int closed;

  zi_sys_loop_watch watches[ZI_SYS_LOOP_MAX_WATCH];
  zi_sys_loop_timer timers[ZI_SYS_LOOP_MAX_TIMERS];
} zi_sys_loop_handle_ctx;

static int32_t loop_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_sys_loop_handle_ctx *h = (zi_sys_loop_handle_ctx *)ctx;
  if (!h || h->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  if (h->out_off >= h->out_len) return ZI_E_AGAIN;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  uint32_t avail = h->out_len - h->out_off;
  uint32_t n = (avail < (uint32_t)cap) ? avail : (uint32_t)cap;
  memcpy(dst, h->outbuf + h->out_off, n);
  h->out_off += n;
  if (h->out_off == h->out_len) {
    h->out_off = 0;
    h->out_len = 0;
  }
  return (int32_t)n;
}

static int append_out(zi_sys_loop_handle_ctx *h, const uint8_t *data, uint32_t n) {
  if (!h) return 0;
  if (n == 0) return 1;
  if (h->out_len + n > (uint32_t)sizeof(h->outbuf)) return 0;
  memcpy(h->outbuf + h->out_len, data, n);
  h->out_len += n;
  return 1;
}

static int watch_find_idx(zi_sys_loop_handle_ctx *h, uint64_t watch_id) {
  if (!h || watch_id == 0) return -1;
  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_WATCH; i++) {
    if (h->watches[i].in_use && h->watches[i].watch_id == watch_id) return i;
  }
  return -1;
}

static int watch_alloc(zi_sys_loop_handle_ctx *h, uint64_t watch_id, zi_handle_t handle, uint32_t events) {
  if (!h || watch_id == 0 || handle < 3) return 0;
  if (events == 0) return 0;
  if (watch_find_idx(h, watch_id) >= 0) return 0;

  // Ensure handle is pollable at registration time.
  int fd = -1;
  if (!zi_handle25_poll_fd(handle, &fd)) return 0;

  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_WATCH; i++) {
    if (!h->watches[i].in_use) {
      h->watches[i].in_use = 1;
      h->watches[i].watch_id = watch_id;
      h->watches[i].h = handle;
      h->watches[i].events = events;
      return 1;
    }
  }
  return 0;
}

static int watch_free(zi_sys_loop_handle_ctx *h, uint64_t watch_id) {
  int idx = watch_find_idx(h, watch_id);
  if (idx < 0) return 0;
  h->watches[idx].in_use = 0;
  h->watches[idx].watch_id = 0;
  h->watches[idx].h = 0;
  h->watches[idx].events = 0;
  return 1;
}

static int timer_find_idx(zi_sys_loop_handle_ctx *h, uint64_t timer_id) {
  if (!h || timer_id == 0) return -1;
  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_TIMERS; i++) {
    if (h->timers[i].in_use && h->timers[i].timer_id == timer_id) return i;
  }
  return -1;
}

static int timer_arm(zi_sys_loop_handle_ctx *h, uint64_t timer_id, uint64_t due_mono_ns, uint64_t interval_ns, uint32_t flags) {
  if (!h || timer_id == 0) return 0;
  if ((flags & ~0x1u) != 0) return 0;

  uint64_t now = now_monotonic_ns();
  uint64_t due = due_mono_ns;
  if (flags & 0x1u) {
    // relative
    due = now + due_mono_ns;
  }

  int idx = timer_find_idx(h, timer_id);
  if (idx < 0) {
    for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_TIMERS; i++) {
      if (!h->timers[i].in_use) {
        h->timers[i].in_use = 1;
        h->timers[i].timer_id = timer_id;
        h->timers[i].due_ns = due;
        h->timers[i].interval_ns = interval_ns;
        return 1;
      }
    }
    return 0;
  }

  // Replace existing.
  h->timers[idx].due_ns = due;
  h->timers[idx].interval_ns = interval_ns;
  return 1;
}

static int timer_cancel(zi_sys_loop_handle_ctx *h, uint64_t timer_id) {
  int idx = timer_find_idx(h, timer_id);
  if (idx < 0) return 0;
  h->timers[idx].in_use = 0;
  h->timers[idx].timer_id = 0;
  h->timers[idx].due_ns = 0;
  h->timers[idx].interval_ns = 0;
  return 1;
}

static uint64_t timers_next_due_ns(zi_sys_loop_handle_ctx *h) {
  uint64_t best = 0;
  if (!h) return 0;
  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_TIMERS; i++) {
    if (!h->timers[i].in_use) continue;
    uint64_t due = h->timers[i].due_ns;
    if (due == 0) continue;
    if (best == 0 || due < best) best = due;
  }
  return best;
}

static uint32_t map_poll_revents(short revents, uint32_t wanted) {
  uint32_t ev = 0;
#if defined(POLLIN)
  if (revents & POLLIN) ev |= ZI_SYS_LOOP_E_READABLE;
#endif
#if defined(POLLOUT)
  if (revents & POLLOUT) ev |= ZI_SYS_LOOP_E_WRITABLE;
#endif
#if defined(POLLHUP)
  if (revents & POLLHUP) ev |= ZI_SYS_LOOP_E_HUP;
#endif
#if defined(POLLERR)
  if (revents & POLLERR) ev |= ZI_SYS_LOOP_E_ERROR;
#endif
  ev &= wanted;
  return ev;
}

static int emit_ok_empty(zi_sys_loop_handle_ctx *h, const zi_zcl1_frame *z) {
  uint8_t fr[64];
  int n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, NULL, 0);
  if (n < 0) return 0;
  return append_out(h, fr, (uint32_t)n);
}

static int emit_error(zi_sys_loop_handle_ctx *h, const zi_zcl1_frame *z, const char *trace, const char *msg) {
  uint8_t fr[256];
  int n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, trace, msg);
  if (n < 0) return 0;
  return append_out(h, fr, (uint32_t)n);
}

static int handle_poll(zi_sys_loop_handle_ctx *h, const zi_zcl1_frame *z) {
  if (!h || !z) return 0;
  if (z->payload_len != 8) return emit_error(h, z, "sys.loop", "bad POLL payload");

  uint32_t max_events = read_u32le(z->payload + 0);
  uint32_t timeout_ms = read_u32le(z->payload + 4);
  if (max_events == 0) return emit_error(h, z, "sys.loop", "max_events must be >= 1");

  // Determine effective timeout considering timers.
  uint64_t now = now_monotonic_ns();
  uint64_t next_due = timers_next_due_ns(h);

  int timeout_eff_ms = 0;
  if (timeout_ms == 0) {
    timeout_eff_ms = 0;
  } else if (timeout_ms == 0xFFFFFFFFu) {
    timeout_eff_ms = -1;
  } else {
    timeout_eff_ms = (timeout_ms > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)timeout_ms;
  }

  if (next_due != 0 && now != 0) {
    if (next_due <= now) {
      timeout_eff_ms = 0;
    } else {
      uint64_t delta_ns = next_due - now;
      int64_t delta_ms = ns_to_ms_ceil(delta_ns);
      if (delta_ms < 0) delta_ms = 0;
      if (timeout_eff_ms < 0) {
        timeout_eff_ms = (int)delta_ms;
      } else {
        if ((int)delta_ms < timeout_eff_ms) timeout_eff_ms = (int)delta_ms;
      }
    }
  }

#if defined(__unix__) || defined(__APPLE__)
  // Build pollfd array from watches.
  uint32_t watch_count = 0;
  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_WATCH; i++) {
    if (h->watches[i].in_use) watch_count++;
  }

  struct pollfd *pfds = NULL;
  zi_sys_loop_watch **watch_ptrs = NULL;
  const zi_handle_poll_ops_v1 **poll_ops = NULL;
  void **poll_ctx = NULL;

  if (watch_count) {
    pfds = (struct pollfd *)calloc((size_t)watch_count, sizeof(struct pollfd));
    watch_ptrs = (zi_sys_loop_watch **)calloc((size_t)watch_count, sizeof(zi_sys_loop_watch *));
    poll_ops = (const zi_handle_poll_ops_v1 **)calloc((size_t)watch_count, sizeof(const zi_handle_poll_ops_v1 *));
    poll_ctx = (void **)calloc((size_t)watch_count, sizeof(void *));
    if (!pfds || !watch_ptrs || !poll_ops || !poll_ctx) {
      free(pfds);
      free(watch_ptrs);
      free(poll_ops);
      free(poll_ctx);
      return emit_error(h, z, "sys.loop", "oom");
    }

    uint32_t j = 0;
    for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_WATCH; i++) {
      if (!h->watches[i].in_use) continue;
      int fd = -1;
      const zi_handle_poll_ops_v1 *pops = NULL;
      void *pctx = NULL;
      if (!zi_handle25_poll_fd(h->watches[i].h, &fd)) continue;
      (void)zi_handle25_poll_ops(h->watches[i].h, &pops, &pctx);

      // For handles with custom readiness, treat the fd as a wakeup notifier and
      // only poll for readability to wake the loop.
      short events = 0;
      if (pops && pops->get_ready) {
        events |= POLLIN;
      } else {
        if (h->watches[i].events & ZI_SYS_LOOP_E_READABLE) events |= POLLIN;
        if (h->watches[i].events & ZI_SYS_LOOP_E_WRITABLE) events |= POLLOUT;
      }
      // hup/error are reported via revents.
      pfds[j].fd = fd;
      pfds[j].events = events;
      pfds[j].revents = 0;
      watch_ptrs[j] = &h->watches[i];
      poll_ops[j] = pops;
      poll_ctx[j] = pctx;
      j++;
    }
    watch_count = j;
  }

  // If any custom-ready watch is already ready, force an immediate poll(0) so
  // level-triggered readiness is reported without blocking.
  int timeout_poll_ms = timeout_eff_ms;
  if (watch_count && timeout_poll_ms != 0) {
    for (uint32_t i = 0; i < watch_count; i++) {
      if (!watch_ptrs[i]) continue;
      if (!poll_ops[i] || !poll_ops[i]->get_ready) continue;
      uint32_t ready = poll_ops[i]->get_ready(poll_ctx[i]);
      if (ready & watch_ptrs[i]->events) {
        timeout_poll_ms = 0;
        break;
      }
    }
  }

  int poll_rc = 0;
  if (watch_count) {
    poll_rc = poll(pfds, (nfds_t)watch_count, timeout_poll_ms);
    if (poll_rc < 0) {
      if (errno == EINTR) poll_rc = 0;
      else {
        free(pfds);
        free(watch_ptrs);
        free(poll_ops);
        free(poll_ctx);
        return emit_error(h, z, "sys.loop", "poll failed");
      }
    }
  } else {
    // No watches; if timeout_eff_ms is blocking, just sleep using poll with no fds.
    poll_rc = poll(NULL, 0, timeout_eff_ms);
    if (poll_rc < 0) {
      if (errno == EINTR) poll_rc = 0;
      else return emit_error(h, z, "sys.loop", "poll failed");
    }
  }

  (void)poll_rc;

  // Collect due timers and ready watches.
  uint8_t payload[65536];
  uint32_t off = 0;
  write_u32le(payload + off, 1u);
  off += 4;
  uint32_t flags_off = off;
  write_u32le(payload + off, 0u);
  off += 4;
  uint32_t count_off = off;
  write_u32le(payload + off, 0u);
  off += 4;
  write_u32le(payload + off, 0u);
  off += 4;

  uint32_t emitted = 0;
  uint32_t more_pending = 0;

  uint64_t now2 = now_monotonic_ns();

  // Emit READY events.
  for (uint32_t i = 0; i < watch_count && emitted < max_events; i++) {
    if (!watch_ptrs[i]) continue;
    uint32_t ev = 0;
    if (poll_ops[i] && poll_ops[i]->get_ready) {
      // Drain wakeups so future polls can block.
      if ((pfds[i].revents & POLLIN) && poll_ops[i]->drain_wakeup) {
        poll_ops[i]->drain_wakeup(poll_ctx[i]);
      }
      ev = poll_ops[i]->get_ready(poll_ctx[i]);
      ev &= watch_ptrs[i]->events;
      // Preserve error/hup reporting from the underlying fd.
      ev |= (map_poll_revents(pfds[i].revents, ZI_SYS_LOOP_E_ERROR | ZI_SYS_LOOP_E_HUP) & (ZI_SYS_LOOP_E_ERROR | ZI_SYS_LOOP_E_HUP));
    } else {
      ev = map_poll_revents(pfds[i].revents, watch_ptrs[i]->events);
    }
    if (ev == 0) continue;

    // event entry
    if (off + 32u > (uint32_t)sizeof(payload)) break;
    write_u32le(payload + off + 0, (uint32_t)ZI_SYS_LOOP_EV_READY);
    write_u32le(payload + off + 4, ev);
    write_u32le(payload + off + 8, (uint32_t)watch_ptrs[i]->h);
    write_u32le(payload + off + 12, 0u);
    write_u64le(payload + off + 16, watch_ptrs[i]->watch_id);
    write_u64le(payload + off + 24, 0ull);
    off += 32u;
    emitted++;
  }

  // Emit TIMER events.
  for (int i = 0; i < (int)ZI_SYS_LOOP_MAX_TIMERS && emitted < max_events; i++) {
    if (!h->timers[i].in_use) continue;
    if (h->timers[i].due_ns == 0 || now2 == 0) continue;
    if (h->timers[i].due_ns > now2) continue;

    if (off + 32u > (uint32_t)sizeof(payload)) break;
    write_u32le(payload + off + 0, (uint32_t)ZI_SYS_LOOP_EV_TIMER);
    write_u32le(payload + off + 4, 0u);
    write_u32le(payload + off + 8, 0u);
    write_u32le(payload + off + 12, 0u);
    write_u64le(payload + off + 16, h->timers[i].timer_id);
    write_u64le(payload + off + 24, now2);
    off += 32u;
    emitted++;

    if (h->timers[i].interval_ns != 0) {
      // Reschedule repeating.
      h->timers[i].due_ns = now2 + h->timers[i].interval_ns;
    } else {
      // One-shot.
      h->timers[i].in_use = 0;
    }
  }

  // Determine if more events are pending.
  if (emitted >= max_events) {
    more_pending = 1;
  } else {
    // crude check: any remaining ready pollfd or due timers
    for (uint32_t i = 0; i < watch_count; i++) {
      if (!watch_ptrs[i]) continue;
      if (map_poll_revents(pfds[i].revents, watch_ptrs[i]->events) != 0) {
        // already possibly emitted; can't tell; be conservative if max_events was low.
        // nothing
        break;
      }
    }
    // If timers are due but we ran out of space for them, signal more pending.
    if (off + 32u > (uint32_t)sizeof(payload)) more_pending = 1;
  }

  uint32_t hdr_flags = 0;
  if (more_pending) hdr_flags |= 0x1u;

  write_u32le(payload + flags_off, hdr_flags);
  write_u32le(payload + count_off, emitted);

  uint8_t fr[65536];
  int n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)z->op, z->rid, payload, off);
  free(pfds);
  free(watch_ptrs);
  free(poll_ops);
  free(poll_ctx);
  if (n < 0) return emit_error(h, z, "sys.loop", "response too large");
  return append_out(h, fr, (uint32_t)n);
#else
  (void)timeout_eff_ms;
  return emit_error(h, z, "sys.loop", "unsupported platform");
#endif
}

static int handle_req(zi_sys_loop_handle_ctx *h, const zi_zcl1_frame *z) {
  if (!h || !z) return 0;

  switch (z->op) {
    case ZI_SYS_LOOP_OP_WATCH: {
      if (z->payload_len != 20) return emit_error(h, z, "sys.loop", "bad WATCH payload");
      uint32_t handle = read_u32le(z->payload + 0);
      uint32_t events = read_u32le(z->payload + 4);
      uint64_t watch_id = read_u64le(z->payload + 8);
      uint32_t flags = read_u32le(z->payload + 16);
      if (flags != 0) return emit_error(h, z, "sys.loop", "flags must be 0");
      if (!watch_alloc(h, watch_id, (zi_handle_t)handle, events)) {
        return emit_error(h, z, "sys.loop", "watch failed");
      }
      return emit_ok_empty(h, z);
    }

    case ZI_SYS_LOOP_OP_UNWATCH: {
      if (z->payload_len != 8) return emit_error(h, z, "sys.loop", "bad UNWATCH payload");
      uint64_t watch_id = read_u64le(z->payload);
      if (!watch_free(h, watch_id)) return emit_error(h, z, "sys.loop", "unknown watch_id");
      return emit_ok_empty(h, z);
    }

    case ZI_SYS_LOOP_OP_TIMER_ARM: {
      if (z->payload_len != 28) return emit_error(h, z, "sys.loop", "bad TIMER_ARM payload");
      uint64_t timer_id = read_u64le(z->payload + 0);
      uint64_t due = read_u64le(z->payload + 8);
      uint64_t interval = read_u64le(z->payload + 16);
      uint32_t flags = read_u32le(z->payload + 24);
      if (!timer_arm(h, timer_id, due, interval, flags)) return emit_error(h, z, "sys.loop", "timer arm failed");
      return emit_ok_empty(h, z);
    }

    case ZI_SYS_LOOP_OP_TIMER_CANCEL: {
      if (z->payload_len != 8) return emit_error(h, z, "sys.loop", "bad TIMER_CANCEL payload");
      uint64_t timer_id = read_u64le(z->payload);
      if (!timer_cancel(h, timer_id)) return emit_error(h, z, "sys.loop", "unknown timer_id");
      return emit_ok_empty(h, z);
    }

    case ZI_SYS_LOOP_OP_POLL:
      return handle_poll(h, z);

    default:
      return emit_error(h, z, "sys.loop", "unknown op");
  }
}

static int32_t loop_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_sys_loop_handle_ctx *h = (zi_sys_loop_handle_ctx *)ctx;
  if (!h || h->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  if (len > (zi_size32_t)(sizeof(h->inbuf) - h->in_len)) return ZI_E_BOUNDS;
  memcpy(h->inbuf + h->in_len, src, (size_t)len);
  h->in_len += (uint32_t)len;

  // Process as many full frames as present.
  uint32_t off = 0;
  while (h->in_len - off >= 24u) {
    uint32_t payload_len = zi_zcl1_read_u32(h->inbuf + off + 20);
    uint32_t frame_len = 24u + payload_len;
    if (h->in_len - off < frame_len) break;

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(h->inbuf + off, frame_len, &z)) {
      off += 1;
      continue;
    }

    (void)handle_req(h, &z);
    off += frame_len;
  }

  if (off > 0) {
    uint32_t remain = h->in_len - off;
    if (remain) memmove(h->inbuf, h->inbuf + off, remain);
    h->in_len = remain;
  }

  return (int32_t)len;
}

static int32_t loop_end(void *ctx) {
  zi_sys_loop_handle_ctx *h = (zi_sys_loop_handle_ctx *)ctx;
  if (!h) return 0;
  h->closed = 1;
  memset(h, 0, sizeof(*h));
  free(h);
  return 0;
}

static const zi_handle_ops_v1 OPS = {
    .read = loop_read,
    .write = loop_write,
    .end = loop_end,
};

zi_handle_t zi_sys_loop25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

  if (!zi_handles25_init()) {
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  zi_sys_loop_handle_ctx *ctx = (zi_sys_loop_handle_ctx *)calloc(1u, sizeof(*ctx));
  if (!ctx) return (zi_handle_t)ZI_E_OOM;

  zi_handle_t h = zi_handle25_alloc(&OPS, ctx, ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE);
  if (h < 3) {
    loop_end(ctx);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  return h;
}
