#include "semrt_file_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  ZI_E_INVALID = -1,
  ZI_E_BOUNDS = -2,
  ZI_E_NOENT = -3,
  ZI_E_DENIED = -4,
  ZI_E_CLOSED = -5,
  ZI_E_AGAIN = -6,
  ZI_E_NOSYS = -7,
  ZI_E_OOM = -8,
  ZI_E_IO = -9,
  ZI_E_INTERNAL = -10,
};

typedef struct {
  int fd;
} semrt_fd_stream_t;

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

static int32_t fd_read(void* ctx, sem_guest_mem_t* mem, zi_ptr_t dst_ptr, zi_size32_t cap) {
  semrt_fd_stream_t* s = (semrt_fd_stream_t*)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (cap == 0) return 0;

  uint8_t* dst = NULL;
  if (!sem_guest_mem_map_rw(mem, dst_ptr, cap, &dst) || !dst) return ZI_E_BOUNDS;
  const ssize_t n = read(s->fd, dst, (size_t)cap);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_write(void* ctx, sem_guest_mem_t* mem, zi_ptr_t src_ptr, zi_size32_t len) {
  semrt_fd_stream_t* s = (semrt_fd_stream_t*)ctx;
  if (!s) return ZI_E_INTERNAL;
  if (len == 0) return 0;

  const uint8_t* src = NULL;
  if (!sem_guest_mem_map_ro(mem, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;
  const ssize_t n = write(s->fd, src, (size_t)len);
  if (n < 0) return map_errno_to_zi(errno);
  return (int32_t)n;
}

static int32_t fd_end(void* ctx, sem_guest_mem_t* mem) {
  (void)mem;
  semrt_fd_stream_t* s = (semrt_fd_stream_t*)ctx;
  if (!s) return 0;
  if (s->fd >= 0) {
    (void)close(s->fd);
    s->fd = -1;
  }
  free(s);
  return 0;
}

static const sem_handle_ops_t fd_ops = {
    .read = fd_read,
    .write = fd_write,
    .end = fd_end,
};

void semrt_file_fs_init(semrt_file_fs_t* fs, semrt_file_fs_cfg_t cfg) {
  if (!fs) return;
  fs->cfg = cfg;
}

static int has_embedded_nul(const uint8_t* p, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (p[i] == 0) return 1;
  }
  return 0;
}

static uint32_t u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64le(const uint8_t* p) {
  uint64_t lo = u32le(p);
  uint64_t hi = u32le(p + 4);
  return lo | (hi << 32);
}

static int open_under_root(const char* root, const uint8_t* guest_path, uint32_t guest_len, int flags, mode_t mode,
                           int* out_fd) {
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

  r = ZI_E_INVALID;

done:
  if (dirfd != rootfd) (void)close(dirfd);
  (void)close(rootfd);
  return r;
}

zi_handle_t semrt_file_fs_open_from_params(semrt_file_fs_t* fs, sem_handles_t* hs, sem_guest_mem_t* mem, zi_ptr_t params_ptr,
                                           zi_size32_t params_len) {
  if (!fs || !hs || !mem) return (zi_handle_t)ZI_E_INTERNAL;
  if (!fs->cfg.fs_root || fs->cfg.fs_root[0] == '\0') return (zi_handle_t)ZI_E_DENIED;

  // params: u64 path_ptr, u32 path_len, u32 oflags, u32 create_mode
  if (params_len < 20u) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t* p = NULL;
  if (!sem_guest_mem_map_ro(mem, params_ptr, params_len, &p) || !p) return (zi_handle_t)ZI_E_BOUNDS;

  const zi_ptr_t path_ptr = (zi_ptr_t)u64le(p + 0);
  const uint32_t path_len = u32le(p + 8);
  const uint32_t of = u32le(p + 12);
  const uint32_t create_mode = u32le(p + 16);

  if (path_len == 0) return (zi_handle_t)ZI_E_INVALID;

  const uint8_t* path_bytes = NULL;
  if (!sem_guest_mem_map_ro(mem, path_ptr, (zi_size32_t)path_len, &path_bytes) || !path_bytes) {
    return (zi_handle_t)ZI_E_BOUNDS;
  }

  if (has_embedded_nul(path_bytes, path_len)) {
    return (zi_handle_t)ZI_E_INVALID;
  }

  int flags = 0;
  const int want_r = (of & ZI_FILE_O_READ) != 0;
  const int want_w = (of & ZI_FILE_O_WRITE) != 0;
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

  const mode_t mode = (mode_t)(create_mode ? create_mode : 0644);

  int fd = -1;
  const int rr = open_under_root(fs->cfg.fs_root, path_bytes, path_len, flags, mode, &fd);
  if (rr != 1) {
    if (rr == 0) return (zi_handle_t)ZI_E_DENIED;
    return (zi_handle_t)rr;
  }

  semrt_fd_stream_t* s = (semrt_fd_stream_t*)calloc(1, sizeof(*s));
  if (!s) {
    (void)close(fd);
    return (zi_handle_t)ZI_E_OOM;
  }
  s->fd = fd;

  uint32_t hflags = ZI_H_ENDABLE;
  if (want_r) hflags |= ZI_H_READABLE;
  if (want_w) hflags |= ZI_H_WRITABLE;

  const zi_handle_t h = sem_handle_alloc(hs, (sem_handle_entry_t){.ops = &fd_ops, .ctx = s, .hflags = hflags});
  if (h < 0) {
    (void)close(fd);
    free(s);
    return h;
  }
  return h;
}

