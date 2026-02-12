#include "zingcore25.h"

#include "zi_caps.h"
#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_sysabi25.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int fd;
  int close_on_end;
} fd_stream;

static int32_t map_errno_to_zi(int e) {
  switch (e) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    case EWOULDBLOCK:
#endif
      return ZI_E_AGAIN;
    case EBADF:
      return ZI_E_CLOSED;
    case EACCES:
    case EPERM:
      return ZI_E_DENIED;
    case ENOMEM:
      return ZI_E_OOM;
    default:
      return ZI_E_IO;
  }
}

static int32_t fd_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  void *dst = (void *)(uintptr_t)dst_ptr; // native-guest mode
  ssize_t n = read(s->fd, dst, (size_t)cap);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (len == 0) return 0;
  if (src_ptr == 0) return ZI_E_BOUNDS;

  const void *src = (const void *)(uintptr_t)src_ptr; // native-guest mode
  ssize_t n = write(s->fd, src, (size_t)len);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_end(void *ctx) {
  fd_stream *s = (fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (!s->close_on_end) return 0;
  if (close(s->fd) != 0) return map_errno_to_zi(errno);
  s->fd = -1;
  return 0;
}

static const zi_handle_ops_v1 fd_ops = {
    .read = fd_read,
    .write = fd_write,
    .end = fd_end,
};

static int32_t host_telemetry(void *ctx, zi_ptr_t topic_ptr, zi_size32_t topic_len, zi_ptr_t msg_ptr,
                             zi_size32_t msg_len) {
  (void)ctx;
  const uint8_t *topic = (const uint8_t *)(uintptr_t)topic_ptr;
  const uint8_t *msg = (const uint8_t *)(uintptr_t)msg_ptr;

  // Best-effort: prefix with "telemetry:" and write to stderr (fd=2).
  write(2, "telemetry:", 10);
  if (topic && topic_len) {
    write(2, " ", 1);
    write(2, topic, (size_t)topic_len);
  }
  if (msg && msg_len) {
    write(2, " ", 1);
    write(2, msg, (size_t)msg_len);
  }
  write(2, "\n", 1);
  return 0;
}

static zi_cap_v1 cap_stdio_v1 = {
    .kind = "file",
    .name = "stdio",
    .version = 1,
    .cap_flags = 0,
    .meta = (const uint8_t *)"{\"handles\":[\"in\",\"out\",\"err\"]}",
    .meta_len = 34,
};

static zi_cap_v1 cap_demo_echo_v1 = {
    .kind = "demo",
    .name = "echo",
    .version = 1,
    .cap_flags = 0,
    .meta = NULL,
    .meta_len = 0,
};

static zi_cap_v1 cap_demo_version_v1 = {
    .kind = "demo",
    .name = "version",
    .version = 1,
    .cap_flags = 0,
    .meta = (const uint8_t *)"{\"impl\":\"stdio_caps_demo\"}",
    .meta_len = 26,
};

int main(void) {
  // Init built-in registries (caps + async).
  if (!zingcore25_init()) {
    fprintf(stderr, "zingcore25_init failed\n");
    return 1;
  }

  // Native memory mapping so zi_ctl can read/write request/response buffers.
  zi_mem_v1 mem;
  zi_mem_v1_native_init(&mem);
  zi_runtime25_set_mem(&mem);

  // Provide a telemetry sink (optional).
  zi_host_v1 host;
  memset(&host, 0, sizeof(host));
  host.telemetry = host_telemetry;
  zi_runtime25_set_host(&host);

  // Register a few caps (discovery works; opening is pack-specific).
  (void)zi_cap_register(&cap_stdio_v1);
  (void)zi_cap_register(&cap_demo_echo_v1);
  (void)zi_cap_register(&cap_demo_version_v1);

  // Wire three concrete stream handles via the handle table.
  (void)zi_handles25_init();

  static fd_stream s_in = {.fd = 0, .close_on_end = 0};
  static fd_stream s_out = {.fd = 1, .close_on_end = 0};
  static fd_stream s_err = {.fd = 2, .close_on_end = 0};

  zi_handle_t h_in = zi_handle25_alloc(&fd_ops, &s_in, ZI_H_READABLE);
  zi_handle_t h_out = zi_handle25_alloc(&fd_ops, &s_out, ZI_H_WRITABLE);
  zi_handle_t h_err = zi_handle25_alloc(&fd_ops, &s_err, ZI_H_WRITABLE);

  if (h_in == 0 || h_out == 0 || h_err == 0) {
    fprintf(stderr, "failed to allocate stdio handles\n");
    return 1;
  }

  const char *banner = "hello from zingcore25 demo\n";
  (void)zi_write(h_out, (zi_ptr_t)(uintptr_t)banner, (zi_size32_t)strlen(banner));

  const char *note = "(caps discoverable via zi_ctl CAPS_LIST)\n";
  (void)zi_write(h_err, (zi_ptr_t)(uintptr_t)note, (zi_size32_t)strlen(note));

  // Echo one line from stdin to stdout.
  char buf[256];
  int32_t n = zi_read(h_in, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)(sizeof(buf) - 1));
  if (n > 0) {
    buf[n] = '\0';
    (void)zi_write(h_out, (zi_ptr_t)(uintptr_t)buf, (zi_size32_t)strlen(buf));
  }

  return 0;
}
