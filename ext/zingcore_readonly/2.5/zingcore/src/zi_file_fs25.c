#include "zi_file_fs25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  int fd;
} zi_fd_stream;

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
    case ELOOP:
      return ZI_E_DENIED;
    case ENOENT:
      return ZI_E_NOENT;
    case ENOTDIR:
      return ZI_E_NOENT;
    case EISDIR:
      return ZI_E_INVALID;
    case ENOMEM:
      return ZI_E_OOM;
    default:
      return ZI_E_IO;
  }
}

static int32_t fd_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_fd_stream *s = (zi_fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;
  if (dst_ptr == 0) return ZI_E_BOUNDS;

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;

  ssize_t n = read(s->fd, dst, (size_t)cap);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_fd_stream *s = (zi_fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (len == 0) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;
  if (src_ptr == 0) return ZI_E_BOUNDS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  ssize_t n = write(s->fd, src, (size_t)len);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_end(void *ctx) {
  zi_fd_stream *s = (zi_fd_stream *)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (s->fd >= 0) {
    (void)close(s->fd);
    s->fd = -1;
  }
  free(s);
  return 0;
}

static const zi_handle_ops_v1 fd_ops = {
    .read = fd_read,
    .write = fd_write,
    .end = fd_end,
};

static int has_embedded_nul(const uint8_t *p, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (p[i] == 0) return 1;
  }
  return 0;
}

static int open_under_root(const char *root, const uint8_t *guest_path, uint32_t guest_len, int flags, mode_t mode,
                           int *out_fd) {
  if (!root || root[0] == '\0' || !guest_path || guest_len == 0 || !out_fd) return 0;
  *out_fd = -1;

  // Must be an absolute guest path.
  if (guest_path[0] != (uint8_t)'/') return 0;

  int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rootfd < 0) return (int)map_errno_to_zi(errno);

  int dirfd = rootfd;
  int r = 0;

  uint32_t i = 1; // skip leading '/'
  while (i < guest_len) {
    while (i < guest_len && guest_path[i] == (uint8_t)'/') i++;
    if (i >= guest_len) break;

    uint32_t seg_start = i;
    while (i < guest_len && guest_path[i] != (uint8_t)'/') i++;
    uint32_t seg_len = i - seg_start;
    if (seg_len == 0) continue;

    char seg[256];
    if (seg_len >= (uint32_t)sizeof(seg)) {
      r = ZI_E_INVALID;
      goto done;
    }
    memcpy(seg, guest_path + seg_start, seg_len);
    seg[seg_len] = '\0';

    if (seg_len == 1 && seg[0] == '.') {
      continue;
    }
    if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
      r = ZI_E_DENIED;
      goto done;
    }

    // Determine whether this is the last segment.
    uint32_t j = i;
    while (j < guest_len && guest_path[j] == (uint8_t)'/') j++;
    int is_last = (j >= guest_len);

    if (!is_last) {
      int nextfd = openat(dirfd, seg, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (nextfd < 0) {
        r = (int)map_errno_to_zi(errno);
        goto done;
      }
      if (dirfd != rootfd) (void)close(dirfd);
      dirfd = nextfd;
      continue;
    }

    int open_flags = flags | O_NOFOLLOW | O_CLOEXEC;
    int fd = -1;
    if (open_flags & O_CREAT) fd = openat(dirfd, seg, open_flags, mode);
    else fd = openat(dirfd, seg, open_flags);
    if (fd < 0) {
      r = (int)map_errno_to_zi(errno);
      goto done;
    }
    *out_fd = fd;
    r = 1;
    goto done;
  }

  // Path was "/" or empty after normalization.
  r = ZI_E_INVALID;

done:
  if (dirfd != rootfd) (void)close(dirfd);
  (void)close(rootfd);
  return r;
}

static uint32_t u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64le(const uint8_t *p) {
  uint64_t lo = u32le(p);
  uint64_t hi = u32le(p + 4);
  return lo | (hi << 32);
}

// Open a host file and return a zingcore handle.
zi_handle_t zi_file_fs25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return (zi_handle_t)ZI_E_NOSYS;

  // params: u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  if (params_len < 20u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *p = NULL;
  if (!mem->map_ro(mem->ctx, params_ptr, params_len, &p) || !p) return (zi_handle_t)ZI_E_BOUNDS;

  zi_ptr_t path_ptr = (zi_ptr_t)u64le(p + 0);
  uint32_t path_len = u32le(p + 8);
  uint32_t of = u32le(p + 12);
  uint32_t create_mode = u32le(p + 16);

  if (path_len == 0) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t *path_bytes = NULL;
  if (!mem->map_ro(mem->ctx, path_ptr, (zi_size32_t)path_len, &path_bytes) || !path_bytes) {
    return (zi_handle_t)ZI_E_BOUNDS;
  }

  if (has_embedded_nul(path_bytes, path_len)) {
    return (zi_handle_t)ZI_E_INVALID;
  }

  int flags = 0;
  int want_r = (of & ZI_FILE_O_READ) != 0;
  int want_w = (of & ZI_FILE_O_WRITE) != 0;
  if (!want_r && !want_w) return (zi_handle_t)ZI_E_INVALID;
  if (want_r && want_w) flags |= O_RDWR;
  else if (want_w) flags |= O_WRONLY;
  else flags |= O_RDONLY;

  if (of & ZI_FILE_O_CREATE) flags |= O_CREAT;
  if (of & ZI_FILE_O_TRUNC) flags |= O_TRUNC;
  if (of & ZI_FILE_O_APPEND) flags |= O_APPEND;

  if ((of & (ZI_FILE_O_TRUNC | ZI_FILE_O_APPEND)) && !want_w) {
    return (zi_handle_t)ZI_E_INVALID;
  }

  mode_t mode = (mode_t)(create_mode ? create_mode : 0644);

  int fd = -1;
  const char *root = getenv("ZI_FS_ROOT");
  if (root && root[0] != '\0') {
    int rr = open_under_root(root, path_bytes, path_len, flags, mode, &fd);
    if (rr != 1) {
      if (rr == 0) return (zi_handle_t)ZI_E_DENIED;
      return (zi_handle_t)rr;
    }
  } else {
    // No sandbox mapping: interpret guest path directly.
    // NUL-terminate into a bounded buffer.
    if (path_len >= 4096u) return (zi_handle_t)ZI_E_INVALID;
    char host_path[4096];
    memcpy(host_path, path_bytes, path_len);
    host_path[path_len] = '\0';

    int open_flags = flags | O_CLOEXEC;
    if (open_flags & O_CREAT) fd = open(host_path, open_flags, mode);
    else fd = open(host_path, open_flags);
    if (fd < 0) return (zi_handle_t)map_errno_to_zi(errno);
  }

  zi_fd_stream *s = (zi_fd_stream *)calloc(1, sizeof(*s));
  if (!s) {
    (void)close(fd);
    return (zi_handle_t)ZI_E_OOM;
  }
  s->fd = fd;

  uint32_t hflags = ZI_H_ENDABLE;
  if (want_r) hflags |= ZI_H_READABLE;
  if (want_w) hflags |= ZI_H_WRITABLE;

  zi_handle_t h = zi_handle25_alloc(&fd_ops, s, hflags);
  if (h == 0) {
    (void)close(fd);
    free(s);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}

static const uint8_t cap_meta[] =
    "{\"kind\":\"file\",\"name\":\"fs\",\"open\":{\"params\":\"u64 path_ptr; u32 path_len; u32 oflags; u32 create_mode\"}}";

static const zi_cap_v1 cap_file_fs_v1 = {
    .kind = ZI_CAP_KIND_FILE,
    .name = ZI_CAP_NAME_FS,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = cap_meta,
    .meta_len = (uint32_t)(sizeof(cap_meta) - 1),
};

const zi_cap_v1 *zi_file_fs25_cap(void) { return &cap_file_fs_v1; }

int zi_file_fs25_register(void) { return zi_cap_register(&cap_file_fs_v1); }
