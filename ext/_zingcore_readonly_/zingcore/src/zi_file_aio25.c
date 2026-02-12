#include "zi_file_aio25.h"

#include "zi_handles25.h"
#include "zi_runtime25.h"
#include "zi_zcl1.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef ZI_FILE_AIO_MAX_JOBS
#define ZI_FILE_AIO_MAX_JOBS 128
#endif

#ifndef ZI_FILE_AIO_MAX_FILES
#define ZI_FILE_AIO_MAX_FILES 256
#endif

#ifndef ZI_FILE_AIO_MAX_INLINE
#define ZI_FILE_AIO_MAX_INLINE 60000u
#endif

#ifndef ZI_FILE_AIO_MAX_WRITE
#define ZI_FILE_AIO_MAX_WRITE (1024u * 1024u)
#endif

#ifndef ZI_FILE_AIO_MAX_OUT
#define ZI_FILE_AIO_MAX_OUT (1024u * 1024u)
#endif

typedef struct {
  uint64_t id;
  int fd;
  int in_use;
} zi_aio_file;

typedef struct {
  uint16_t op;
  uint32_t rid;
  // op-specific copied fields
  union {
    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t oflags;
      uint32_t create_mode;
    } open;
    struct {
      uint64_t file_id;
    } close;
    struct {
      uint64_t file_id;
      uint64_t offset;
      uint32_t max_len;
      uint32_t flags;
    } read;
    struct {
      uint64_t file_id;
      uint64_t offset;
      uint8_t *data;
      uint32_t len;
      uint32_t flags;
    } write;

    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t mode;
      uint32_t flags;
    } mkdir;

    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t flags;
    } rmdir;

    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t flags;
    } unlink;

    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t flags;
    } stat;

    struct {
      uint8_t *path;
      uint32_t path_len;
      uint32_t max_bytes;
      uint32_t flags;
    } readdir;
  } u;
} zi_aio_job;

typedef struct {
  uint8_t inbuf[65536];
  uint32_t in_len;

  uint8_t *outbuf;
  uint32_t out_cap;
  uint32_t out_len;
  uint32_t out_off;

  int closed;

  int notify_r;
  int notify_w;
  int notify_signaled;

  int submit_full;

  int root_enabled;
  int rootfd;
  int root_open_errno;

  pthread_t worker;
  int worker_started;

  pthread_mutex_t mu;
  pthread_cond_t cv;

  zi_aio_job jobs[ZI_FILE_AIO_MAX_JOBS];
  uint32_t job_head;
  uint32_t job_tail;
  uint32_t job_count;

  zi_aio_file files[ZI_FILE_AIO_MAX_FILES];
  uint64_t next_file_id;
} zi_file_aio_ctx;

static int handle_req_locked(zi_file_aio_ctx *c, const zi_zcl1_frame *z);

static void compact_out_locked(zi_file_aio_ctx *c) {
  if (!c || !c->outbuf) return;
  if (c->out_off == 0) return;
  if (c->out_off >= c->out_len) {
    c->out_off = 0;
    c->out_len = 0;
    return;
  }
  uint32_t remain = c->out_len - c->out_off;
  memmove(c->outbuf, c->outbuf + c->out_off, remain);
  c->out_off = 0;
  c->out_len = remain;
}

static int32_t map_errno_to_zi(int e) {
  switch (e) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    case EWOULDBLOCK:
#endif
      return ZI_E_AGAIN;
    case EEXIST:
    case ENOTEMPTY:
    case EINVAL:
      return ZI_E_INVALID;
    case EBADF:
      return ZI_E_CLOSED;
    case EACCES:
    case EPERM:
      return ZI_E_DENIED;
    case ELOOP:
      return ZI_E_DENIED;
    case ENOENT:
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

static int set_nonblocking_best_effort(int fd) {
  if (fd < 0) return 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return 0;
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return 1;
}

static int has_embedded_nul(const uint8_t *p, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (p[i] == 0) return 1;
  }
  return 0;
}

static uint32_t dtype_from_dirent(unsigned char dt);

static int open_under_root_fd(int rootfd, const uint8_t *guest_path, uint32_t guest_len, int flags, mode_t mode, int *out_fd) {
  if (rootfd < 0 || !guest_path || guest_len == 0 || !out_fd) return 0;
  *out_fd = -1;

  // Must be an absolute guest path.
  if (guest_path[0] != (uint8_t)'/') return 0;

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
  return r;
}

static int open_parent_under_root_fd(int rootfd, const uint8_t *guest_path, uint32_t guest_len,
                                     int *out_dirfd, int *out_need_close, char *out_name,
                                     size_t out_name_cap) {
  if (rootfd < 0 || !guest_path || guest_len == 0) return 0;
  if (!out_dirfd || !out_need_close || !out_name || out_name_cap < 2) return 0;
  *out_dirfd = -1;
  *out_need_close = 0;
  out_name[0] = '\0';

  // Must be an absolute guest path.
  if (guest_path[0] != (uint8_t)'/') return 0;

  int dirfd = rootfd;
  int need_close = 0;
  int r = 0;

  uint32_t i = 1; // skip leading '/'
  while (i < guest_len) {
    while (i < guest_len && guest_path[i] == (uint8_t)'/') i++;
    if (i >= guest_len) break;

    uint32_t seg_start = i;
    while (i < guest_len && guest_path[i] != (uint8_t)'/') i++;
    uint32_t seg_len = i - seg_start;
    if (seg_len == 0) continue;

    if (seg_len >= 256u) {
      r = ZI_E_INVALID;
      goto done;
    }
    char seg[256];
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
      if (need_close) (void)close(dirfd);
      dirfd = nextfd;
      need_close = 1;
      continue;
    }

    if (seg_len + 1 > out_name_cap) {
      r = ZI_E_INVALID;
      goto done;
    }
    memcpy(out_name, seg, seg_len + 1);
    *out_dirfd = dirfd;
    *out_need_close = need_close;
    r = 1;
    goto done;
  }

  r = ZI_E_INVALID;

done:
  if (r != 1) {
    if (need_close) (void)close(dirfd);
  }
  return r;
}

static int append_out_locked(zi_file_aio_ctx *c, const uint8_t *data, uint32_t n) {
  if (!c) return 0;
  if (n == 0) return 1;

  if (!c->outbuf || c->out_cap == 0) return 0;

  // Reclaim space from the front if we've already read some bytes.
  compact_out_locked(c);

  if (c->out_len + n > c->out_cap) {
    uint32_t need = c->out_len + n;
    if (need > (uint32_t)ZI_FILE_AIO_MAX_OUT) return 0;
    uint32_t new_cap = c->out_cap;
    while (new_cap < need && new_cap < (uint32_t)ZI_FILE_AIO_MAX_OUT) {
      uint32_t next = new_cap * 2u;
      if (next <= new_cap) {
        new_cap = (uint32_t)ZI_FILE_AIO_MAX_OUT;
        break;
      }
      new_cap = next;
    }
    if (new_cap < need) new_cap = need;
    if (new_cap > (uint32_t)ZI_FILE_AIO_MAX_OUT) new_cap = (uint32_t)ZI_FILE_AIO_MAX_OUT;
    if (new_cap < need) return 0;

    uint8_t *nb = (uint8_t *)realloc(c->outbuf, (size_t)new_cap);
    if (!nb) return 0;
    c->outbuf = nb;
    c->out_cap = new_cap;
  }

  memcpy(c->outbuf + c->out_len, data, n);
  c->out_len += n;
  return 1;
}

static int append_out_or_wait_locked(zi_file_aio_ctx *c, const uint8_t *data, uint32_t n) {
  if (!c) return 0;
  if (n > (uint32_t)ZI_FILE_AIO_MAX_OUT) return 0;

  for (;;) {
    if (append_out_locked(c, data, n)) return 1;
    if (c->closed) return 0;

    // If we can still grow and failed, treat as fatal (likely OOM).
    if (c->out_cap < (uint32_t)ZI_FILE_AIO_MAX_OUT) return 0;

    // At max buffer cap: wait for the guest to drain output.
    pthread_cond_wait(&c->cv, &c->mu);
  }
}

static void signal_wakeup_locked(zi_file_aio_ctx *c) {
  if (!c) return;
  if (c->notify_signaled) return;
  if (c->notify_w < 0) return;
  uint8_t b = 1;
  ssize_t n = write(c->notify_w, &b, 1);
  if (n == 1) c->notify_signaled = 1;
}

static void signal_readable_locked(zi_file_aio_ctx *c) {
  if (!c) return;
  if (c->out_len == 0) return;
  signal_wakeup_locked(c);
}

static void drain_wakeup_locked(zi_file_aio_ctx *c) {
  if (!c) return;
  uint8_t tmp[64];
  for (;;) {
    ssize_t n = read(c->notify_r, tmp, sizeof(tmp));
    if (n > 0) continue;
    break;
  }
  c->notify_signaled = 0;
}

static void drain_notify_if_empty(zi_file_aio_ctx *c) {
  if (!c) return;
  if (c->out_len != 0) return;
  drain_wakeup_locked(c);
}

static int ensure_out_headroom_locked(zi_file_aio_ctx *c, uint32_t need_free) {
  if (!c) return 0;
  if (!c->outbuf || c->out_cap == 0) return 0;
  compact_out_locked(c);
  if (c->out_cap - c->out_len >= need_free) return 1;

  uint32_t need = c->out_len + need_free;
  if (need > (uint32_t)ZI_FILE_AIO_MAX_OUT) return 0;
  uint32_t new_cap = c->out_cap;
  while (new_cap < need && new_cap < (uint32_t)ZI_FILE_AIO_MAX_OUT) {
    uint32_t next = new_cap * 2u;
    if (next <= new_cap) {
      new_cap = (uint32_t)ZI_FILE_AIO_MAX_OUT;
      break;
    }
    new_cap = next;
  }
  if (new_cap < need) new_cap = need;
  if (new_cap > (uint32_t)ZI_FILE_AIO_MAX_OUT) new_cap = (uint32_t)ZI_FILE_AIO_MAX_OUT;
  if (new_cap < need) return 0;

  uint8_t *nb = (uint8_t *)realloc(c->outbuf, (size_t)new_cap);
  if (!nb) return 0;
  c->outbuf = nb;
  c->out_cap = new_cap;
  return (c->out_cap - c->out_len >= need_free) ? 1 : 0;
}

static void job_free_payload(zi_aio_job *j) {
  if (!j) return;
  if (j->op == (uint16_t)ZI_FILE_AIO_OP_OPEN) {
    free(j->u.open.path);
    j->u.open.path = NULL;
    j->u.open.path_len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_WRITE) {
    free(j->u.write.data);
    j->u.write.data = NULL;
    j->u.write.len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_MKDIR) {
    free(j->u.mkdir.path);
    j->u.mkdir.path = NULL;
    j->u.mkdir.path_len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) {
    free(j->u.rmdir.path);
    j->u.rmdir.path = NULL;
    j->u.rmdir.path_len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_UNLINK) {
    free(j->u.unlink.path);
    j->u.unlink.path = NULL;
    j->u.unlink.path_len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_STAT) {
    free(j->u.stat.path);
    j->u.stat.path = NULL;
    j->u.stat.path_len = 0;
  } else if (j->op == (uint16_t)ZI_FILE_AIO_OP_READDIR) {
    free(j->u.readdir.path);
    j->u.readdir.path = NULL;
    j->u.readdir.path_len = 0;
  }
}

static int enqueue_job_locked(zi_file_aio_ctx *c, const zi_aio_job *src) {
  if (!c || !src) return 0;
  if (c->job_count >= ZI_FILE_AIO_MAX_JOBS) return 0;
  c->jobs[c->job_tail] = *src;
  c->job_tail = (c->job_tail + 1) % ZI_FILE_AIO_MAX_JOBS;
  c->job_count++;
  if (c->job_count >= ZI_FILE_AIO_MAX_JOBS) c->submit_full = 1;
  pthread_cond_signal(&c->cv);
  return 1;
}

static int dequeue_job_locked(zi_file_aio_ctx *c, zi_aio_job *out) {
  if (!c || !out) return 0;
  if (c->job_count == 0) return 0;
  *out = c->jobs[c->job_head];
  c->job_head = (c->job_head + 1) % ZI_FILE_AIO_MAX_JOBS;
  c->job_count--;
  if (c->submit_full && c->job_count == (ZI_FILE_AIO_MAX_JOBS - 1u)) {
    c->submit_full = 0;
    // Transition from full -> has space: wake sys/loop waiters.
    signal_wakeup_locked(c);
  }
  return 1;
}

static zi_aio_file *file_find_locked(zi_file_aio_ctx *c, uint64_t id) {
  if (!c || id == 0) return NULL;
  for (uint32_t i = 0; i < ZI_FILE_AIO_MAX_FILES; i++) {
    if (c->files[i].in_use && c->files[i].id == id) return &c->files[i];
  }
  return NULL;
}

static int file_alloc_locked(zi_file_aio_ctx *c, int fd, uint64_t *out_id) {
  if (!c || fd < 0 || !out_id) return 0;
  for (uint32_t i = 0; i < ZI_FILE_AIO_MAX_FILES; i++) {
    if (!c->files[i].in_use) {
      uint64_t id = c->next_file_id++;
      if (id == 0) id = c->next_file_id++;
      c->files[i].in_use = 1;
      c->files[i].id = id;
      c->files[i].fd = fd;
      *out_id = id;
      return 1;
    }
  }
  return 0;
}

static int emit_done_ok_locked(zi_file_aio_ctx *c, uint32_t rid, uint16_t orig_op, uint32_t result, const uint8_t *extra, uint32_t extra_len) {
  if (!c) return 0;
  uint32_t payload_len = 8u + extra_len;
  if (payload_len > (uint32_t)ZI_FILE_AIO_MAX_OUT) return 0;

  uint8_t *pl = (uint8_t *)malloc(payload_len);
  if (!pl) return 0;
  zi_zcl1_write_u16(pl + 0, orig_op);
  zi_zcl1_write_u16(pl + 2, 0);
  zi_zcl1_write_u32(pl + 4, result);
  if (extra_len && extra) memcpy(pl + 8, extra, extra_len);

  uint8_t fr[65536];
  int n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), (uint16_t)ZI_FILE_AIO_EV_DONE, rid, pl, payload_len);
  free(pl);
  if (n < 0) return 0;

  int was_empty = (c->out_len == 0);
  if (!append_out_or_wait_locked(c, fr, (uint32_t)n)) return 0;
  if (was_empty) signal_readable_locked(c);
  return 1;
}

static int emit_done_err_locked(zi_file_aio_ctx *c, uint32_t rid, const char *trace, const char *msg) {
  if (!c) return 0;
  uint8_t fr[65536];
  int n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), (uint16_t)ZI_FILE_AIO_EV_DONE, rid, trace, msg);
  if (n < 0) return 0;
  int was_empty = (c->out_len == 0);
  if (!append_out_or_wait_locked(c, fr, (uint32_t)n)) return 0;
  if (was_empty) signal_readable_locked(c);
  return 1;
}

static void process_pending_requests_locked(zi_file_aio_ctx *c) {
  if (!c) return;
  uint32_t off = 0;
  while (c->in_len - off >= 24u) {
    // Reserve headroom so we can always emit an immediate response frame.
    if (!ensure_out_headroom_locked(c, 4096u)) break;

    uint32_t payload_len = zi_zcl1_read_u32(c->inbuf + off + 20);
    uint32_t frame_len = 24u + payload_len;
    if (c->in_len - off < frame_len) break;

    zi_zcl1_frame z;
    if (!zi_zcl1_parse(c->inbuf + off, frame_len, &z)) {
      off += 1;
      continue;
    }

    (void)handle_req_locked(c, &z);
    off += frame_len;
  }

  if (off > 0) {
    uint32_t remain = c->in_len - off;
    if (remain) memmove(c->inbuf, c->inbuf + off, remain);
    c->in_len = remain;
  }
}

static void *worker_main(void *arg) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)arg;
  if (!c) return NULL;

  for (;;) {
    pthread_mutex_lock(&c->mu);
    while (!c->closed && c->job_count == 0) {
      pthread_cond_wait(&c->cv, &c->mu);
    }
    if (c->closed) {
      pthread_mutex_unlock(&c->mu);
      break;
    }
    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    int ok = dequeue_job_locked(c, &j);
    pthread_mutex_unlock(&c->mu);
    if (!ok) continue;

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_OPEN) {
      // open file
      int flags = 0;
      int want_r = (j.u.open.oflags & ZI_FILE_O_READ) != 0;
      int want_w = (j.u.open.oflags & ZI_FILE_O_WRITE) != 0;
      if (!want_r && !want_w) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "bad oflags");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }
      if (want_r && want_w) flags |= O_RDWR;
      else if (want_w) flags |= O_WRONLY;
      else flags |= O_RDONLY;

      if (j.u.open.oflags & ZI_FILE_O_CREATE) flags |= O_CREAT;
      if (j.u.open.oflags & ZI_FILE_O_TRUNC) flags |= O_TRUNC;
      if (j.u.open.oflags & ZI_FILE_O_APPEND) flags |= O_APPEND;

      if ((j.u.open.oflags & (ZI_FILE_O_TRUNC | ZI_FILE_O_APPEND)) && !want_w) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "TRUNC/APPEND requires write");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      mode_t mode = (mode_t)(j.u.open.create_mode ? j.u.open.create_mode : 0644);

      int fd = -1;
      if (c->root_enabled) {
        if (c->rootfd < 0) {
          pthread_mutex_lock(&c->mu);
          (void)emit_done_err_locked(c, j.rid, "file.aio", "sandbox root unavailable");
          pthread_mutex_unlock(&c->mu);
          job_free_payload(&j);
          continue;
        }
        int rr = open_under_root_fd(c->rootfd, j.u.open.path, j.u.open.path_len, flags, mode, &fd);
        if (rr != 1) {
          pthread_mutex_lock(&c->mu);
          if (rr == 0) (void)emit_done_err_locked(c, j.rid, "file.aio", "denied");
          else {
            char msg[64];
            snprintf(msg, sizeof(msg), "open failed: %d", rr);
            (void)emit_done_err_locked(c, j.rid, "file.aio", msg);
          }
          pthread_mutex_unlock(&c->mu);
          job_free_payload(&j);
          continue;
        }
      } else {
        if (j.u.open.path_len >= 4096u) {
          pthread_mutex_lock(&c->mu);
          (void)emit_done_err_locked(c, j.rid, "file.aio", "path too long");
          pthread_mutex_unlock(&c->mu);
          job_free_payload(&j);
          continue;
        }
        char host_path[4096];
        memcpy(host_path, j.u.open.path, j.u.open.path_len);
        host_path[j.u.open.path_len] = '\0';
        int open_flags = flags | O_CLOEXEC;
        if (open_flags & O_CREAT) fd = open(host_path, open_flags, mode);
        else fd = open(host_path, open_flags);
        if (fd < 0) {
          pthread_mutex_lock(&c->mu);
          (void)emit_done_err_locked(c, j.rid, "file.aio", "open failed");
          pthread_mutex_unlock(&c->mu);
          job_free_payload(&j);
          continue;
        }
      }

      uint64_t file_id = 0;
      pthread_mutex_lock(&c->mu);
      if (!file_alloc_locked(c, fd, &file_id)) {
        (void)close(fd);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "too many open files");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }
      uint8_t extra[8];
      zi_zcl1_write_u32(extra + 0, (uint32_t)(file_id & 0xFFFFFFFFu));
      zi_zcl1_write_u32(extra + 4, (uint32_t)((file_id >> 32) & 0xFFFFFFFFu));
      (void)emit_done_ok_locked(c, j.rid, j.op, 0u, extra, 8u);
      pthread_mutex_unlock(&c->mu);

      job_free_payload(&j);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_CLOSE) {
      pthread_mutex_lock(&c->mu);
      zi_aio_file *f = file_find_locked(c, j.u.close.file_id);
      if (!f) {
        (void)emit_done_err_locked(c, j.rid, "file.aio", "unknown file_id");
        pthread_mutex_unlock(&c->mu);
        continue;
      }
      int fd = f->fd;
      f->in_use = 0;
      f->id = 0;
      f->fd = -1;
      pthread_mutex_unlock(&c->mu);
      (void)close(fd);

      pthread_mutex_lock(&c->mu);
      (void)emit_done_ok_locked(c, j.rid, j.op, 0u, NULL, 0u);
      pthread_mutex_unlock(&c->mu);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_READ) {
      uint32_t want = j.u.read.max_len;
      if (want > ZI_FILE_AIO_MAX_INLINE) want = ZI_FILE_AIO_MAX_INLINE;

      pthread_mutex_lock(&c->mu);
      zi_aio_file *f = file_find_locked(c, j.u.read.file_id);
      int fd = f ? f->fd : -1;
      pthread_mutex_unlock(&c->mu);

      if (fd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "unknown file_id");
        pthread_mutex_unlock(&c->mu);
        continue;
      }

      uint8_t *buf = NULL;
      if (want) {
        buf = (uint8_t *)malloc(want);
        if (!buf) {
          pthread_mutex_lock(&c->mu);
          (void)emit_done_err_locked(c, j.rid, "file.aio", "oom");
          pthread_mutex_unlock(&c->mu);
          continue;
        }
      }

      ssize_t n = 0;
      if (want) {
        n = pread(fd, buf, (size_t)want, (off_t)j.u.read.offset);
      } else {
        n = 0;
      }
      if (n < 0) {
        free(buf);
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "read failed");
        pthread_mutex_unlock(&c->mu);
        continue;
      }

      pthread_mutex_lock(&c->mu);
      (void)emit_done_ok_locked(c, j.rid, j.op, (uint32_t)n, buf, (uint32_t)n);
      pthread_mutex_unlock(&c->mu);
      free(buf);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_WRITE) {
      pthread_mutex_lock(&c->mu);
      zi_aio_file *f = file_find_locked(c, j.u.write.file_id);
      int fd = f ? f->fd : -1;
      pthread_mutex_unlock(&c->mu);

      if (fd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "unknown file_id");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      ssize_t n = 0;
      if (j.u.write.len) {
        n = pwrite(fd, j.u.write.data, (size_t)j.u.write.len, (off_t)j.u.write.offset);
      } else {
        n = 0;
      }

      pthread_mutex_lock(&c->mu);
      if (n < 0) {
        (void)emit_done_err_locked(c, j.rid, "file.aio", "write failed");
      } else {
        (void)emit_done_ok_locked(c, j.rid, j.op, (uint32_t)n, NULL, 0u);
      }
      pthread_mutex_unlock(&c->mu);

      job_free_payload(&j);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_MKDIR) {
      if (c->root_enabled && c->rootfd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "sandbox root unavailable");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      int ok = 0;
      int32_t e = 0;
      if (c->root_enabled) {
        int dirfd = -1;
        int need_close = 0;
        char name[256];
        int rr = open_parent_under_root_fd(c->rootfd, j.u.mkdir.path, j.u.mkdir.path_len, &dirfd, &need_close, name, sizeof(name));
        if (rr != 1) {
          e = (rr == 0) ? ZI_E_DENIED : rr;
        } else {
          mode_t mode = (mode_t)(j.u.mkdir.mode ? j.u.mkdir.mode : 0755);
          if (mkdirat(dirfd, name, mode) == 0) ok = 1;
          else e = map_errno_to_zi(errno);
          if (need_close) (void)close(dirfd);
        }
      } else {
        if (j.u.mkdir.path_len >= 4096u) e = ZI_E_INVALID;
        else {
          char host_path[4096];
          memcpy(host_path, j.u.mkdir.path, j.u.mkdir.path_len);
          host_path[j.u.mkdir.path_len] = '\0';
          mode_t mode = (mode_t)(j.u.mkdir.mode ? j.u.mkdir.mode : 0755);
          if (mkdir(host_path, mode) == 0) ok = 1;
          else e = map_errno_to_zi(errno);
        }
      }

      pthread_mutex_lock(&c->mu);
      if (!ok) {
        char msg[64];
        snprintf(msg, sizeof(msg), "mkdir failed: %d", (int)e);
        (void)emit_done_err_locked(c, j.rid, "file.aio", msg);
      } else {
        (void)emit_done_ok_locked(c, j.rid, j.op, 0u, NULL, 0u);
      }
      pthread_mutex_unlock(&c->mu);

      job_free_payload(&j);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_UNLINK || j.op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) {
      uint8_t *path = (j.op == (uint16_t)ZI_FILE_AIO_OP_UNLINK) ? j.u.unlink.path : j.u.rmdir.path;
      uint32_t path_len = (j.op == (uint16_t)ZI_FILE_AIO_OP_UNLINK) ? j.u.unlink.path_len : j.u.rmdir.path_len;

      if (c->root_enabled && c->rootfd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "sandbox root unavailable");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      int ok = 0;
      int32_t e = 0;
      if (c->root_enabled) {
        int dirfd = -1;
        int need_close = 0;
        char name[256];
        int rr = open_parent_under_root_fd(c->rootfd, path, path_len, &dirfd, &need_close, name, sizeof(name));
        if (rr != 1) {
          e = (rr == 0) ? ZI_E_DENIED : rr;
        } else {
          struct stat st;
          if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
            e = ZI_E_DENIED;
          } else {
            int flags = (j.op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) ? AT_REMOVEDIR : 0;
            if (unlinkat(dirfd, name, flags) == 0) ok = 1;
            else e = map_errno_to_zi(errno);
          }
          if (need_close) (void)close(dirfd);
        }
      } else {
        if (path_len >= 4096u) e = ZI_E_INVALID;
        else {
          char host_path[4096];
          memcpy(host_path, path, path_len);
          host_path[path_len] = '\0';
          int r = 0;
          if (j.op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) r = rmdir(host_path);
          else r = unlink(host_path);
          if (r == 0) ok = 1;
          else e = map_errno_to_zi(errno);
        }
      }

      pthread_mutex_lock(&c->mu);
      if (!ok) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s failed: %d", (j.op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) ? "rmdir" : "unlink", (int)e);
        (void)emit_done_err_locked(c, j.rid, "file.aio", msg);
      } else {
        (void)emit_done_ok_locked(c, j.rid, j.op, 0u, NULL, 0u);
      }
      pthread_mutex_unlock(&c->mu);

      job_free_payload(&j);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_STAT) {
      if (c->root_enabled && c->rootfd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "sandbox root unavailable");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      int ok = 0;
      int32_t e = 0;
      struct stat st;
      memset(&st, 0, sizeof(st));

      if (c->root_enabled) {
        int dirfd = -1;
        int need_close = 0;
        char name[256];
        int rr = open_parent_under_root_fd(c->rootfd, j.u.stat.path, j.u.stat.path_len, &dirfd, &need_close, name, sizeof(name));
        if (rr != 1) {
          e = (rr == 0) ? ZI_E_DENIED : rr;
        } else {
          if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) ok = 1;
          else e = map_errno_to_zi(errno);
          if (need_close) (void)close(dirfd);
        }
      } else {
        if (j.u.stat.path_len >= 4096u) e = ZI_E_INVALID;
        else {
          char host_path[4096];
          memcpy(host_path, j.u.stat.path, j.u.stat.path_len);
          host_path[j.u.stat.path_len] = '\0';
          if (lstat(host_path, &st) == 0) ok = 1;
          else e = map_errno_to_zi(errno);
        }
      }

      pthread_mutex_lock(&c->mu);
      if (!ok) {
        char msg[64];
        snprintf(msg, sizeof(msg), "stat failed: %d", (int)e);
        (void)emit_done_err_locked(c, j.rid, "file.aio", msg);
      } else {
        uint64_t size = (uint64_t)st.st_size;
#if defined(__APPLE__)
        uint64_t mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#else
        uint64_t mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
        uint32_t mode = (uint32_t)st.st_mode;
        uint32_t uid = (uint32_t)st.st_uid;
        uint32_t gid = (uint32_t)st.st_gid;
        uint8_t extra[32];
        zi_zcl1_write_u32(extra + 0, (uint32_t)(size & 0xFFFFFFFFu));
        zi_zcl1_write_u32(extra + 4, (uint32_t)((size >> 32) & 0xFFFFFFFFu));
        zi_zcl1_write_u32(extra + 8, (uint32_t)(mtime_ns & 0xFFFFFFFFu));
        zi_zcl1_write_u32(extra + 12, (uint32_t)((mtime_ns >> 32) & 0xFFFFFFFFu));
        zi_zcl1_write_u32(extra + 16, mode);
        zi_zcl1_write_u32(extra + 20, uid);
        zi_zcl1_write_u32(extra + 24, gid);
        zi_zcl1_write_u32(extra + 28, 0u);
        (void)emit_done_ok_locked(c, j.rid, j.op, 0u, extra, (uint32_t)sizeof(extra));
      }
      pthread_mutex_unlock(&c->mu);

      job_free_payload(&j);
      continue;
    }

    if (j.op == (uint16_t)ZI_FILE_AIO_OP_READDIR) {
      if (c->root_enabled && c->rootfd < 0) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "sandbox root unavailable");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      uint32_t cap = j.u.readdir.max_bytes;
      if (cap == 0 || cap > 60000u) cap = 60000u;
      if (cap < 4u) cap = 4u;

      uint8_t *extra = (uint8_t *)malloc(cap);
      if (!extra) {
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "oom");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      uint32_t flags = 0u;
      uint32_t used = 4u;
      uint32_t count = 0u;

      DIR *dir = NULL;
      if (c->root_enabled) {
        int fd = -1;
        int rr = open_under_root_fd(c->rootfd, j.u.readdir.path, j.u.readdir.path_len, O_RDONLY | O_DIRECTORY, 0, &fd);
        if (rr == 1) {
          dir = fdopendir(fd);
          if (!dir) (void)close(fd);
        }
      } else {
        if (j.u.readdir.path_len < 4096u) {
          char host_path[4096];
          memcpy(host_path, j.u.readdir.path, j.u.readdir.path_len);
          host_path[j.u.readdir.path_len] = '\0';
          dir = opendir(host_path);
        }
      }

      if (!dir) {
        free(extra);
        pthread_mutex_lock(&c->mu);
        (void)emit_done_err_locked(c, j.rid, "file.aio", "readdir open failed");
        pthread_mutex_unlock(&c->mu);
        job_free_payload(&j);
        continue;
      }

      for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) break;

        const char *name = ent->d_name;
        if (!name) continue;
        if (name[0] == '.' && name[1] == '\0') continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == '\0') continue;

        uint32_t name_len = (uint32_t)strlen(name);
        uint32_t need = 8u + name_len;
        if (used + need > cap) {
          flags |= 0x1u;
          break;
        }
        zi_zcl1_write_u32(extra + used + 0, dtype_from_dirent(ent->d_type));
        zi_zcl1_write_u32(extra + used + 4, name_len);
        memcpy(extra + used + 8, name, name_len);
        used += need;
        count++;
      }

      (void)closedir(dir);

      zi_zcl1_write_u32(extra + 0, flags);
      pthread_mutex_lock(&c->mu);
      (void)emit_done_ok_locked(c, j.rid, j.op, count, extra, used);
      pthread_mutex_unlock(&c->mu);

      free(extra);
      job_free_payload(&j);
      continue;
    }

    // Unknown op
    pthread_mutex_lock(&c->mu);
    (void)emit_done_err_locked(c, j.rid, "file.aio", "unknown job op");
    pthread_mutex_unlock(&c->mu);
  }

  return NULL;
}

static int get_fd(void *ctx, int *out_fd) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c || c->closed) return 0;
  if (c->notify_r < 0) return 0;
  if (out_fd) *out_fd = c->notify_r;
  return 1;
}

static uint32_t get_ready(void *ctx) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c) return 0;
  uint32_t ev = 0;
  pthread_mutex_lock(&c->mu);
  if (c->out_len != 0) ev |= ZI_H_READABLE;
  if (!c->closed && c->job_count < ZI_FILE_AIO_MAX_JOBS) ev |= ZI_H_WRITABLE;
  pthread_mutex_unlock(&c->mu);
  return ev;
}

static void drain_wakeup(void *ctx) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c) return;
  pthread_mutex_lock(&c->mu);
  drain_wakeup_locked(c);
  pthread_mutex_unlock(&c->mu);
}

static const zi_handle_poll_ops_v1 POLL_OPS = {
    .get_fd = get_fd,
    .get_ready = get_ready,
    .drain_wakeup = drain_wakeup,
};

static int32_t aio_read(void *ctx, zi_ptr_t dst_ptr, zi_size32_t cap) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c || c->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_rw) return ZI_E_NOSYS;

  pthread_mutex_lock(&c->mu);
  if (c->out_off >= c->out_len) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_AGAIN;
  }

  uint8_t *dst = NULL;
  if (!mem->map_rw(mem->ctx, dst_ptr, cap, &dst) || !dst) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_BOUNDS;
  }

  uint32_t avail = c->out_len - c->out_off;
  uint32_t n = (avail < (uint32_t)cap) ? avail : (uint32_t)cap;
  memcpy(dst, c->outbuf + c->out_off, n);
  c->out_off += n;

  // Any progress draining output may free space for the worker to emit completions.
  pthread_cond_broadcast(&c->cv);

  if (c->out_off == c->out_len) {
    c->out_off = 0;
    c->out_len = 0;
    drain_notify_if_empty(c);
  }

  // If there are pending requests buffered from prior writes, process them now
  // (this makes forward progress even when the guest stops writing and waits on acks).
  process_pending_requests_locked(c);

  pthread_mutex_unlock(&c->mu);
  return (int32_t)n;
}

static int emit_error_locked(zi_file_aio_ctx *c, const zi_zcl1_frame *z, const char *trace, const char *msg) {
  uint8_t fr[4096];
  int n = zi_zcl1_write_error(fr, (uint32_t)sizeof(fr), z->op, z->rid, trace, msg);
  if (n < 0) return 0;
  int was_empty = (c->out_len == 0);
  if (!append_out_locked(c, fr, (uint32_t)n)) return 0;
  if (was_empty) signal_readable_locked(c);
  return 1;
}

static int emit_ok_empty_locked(zi_file_aio_ctx *c, const zi_zcl1_frame *z) {
  uint8_t fr[64];
  int n = zi_zcl1_write_ok(fr, (uint32_t)sizeof(fr), z->op, z->rid, NULL, 0);
  if (n < 0) return 0;
  int was_empty = (c->out_len == 0);
  if (!append_out_locked(c, fr, (uint32_t)n)) return 0;
  if (was_empty) signal_readable_locked(c);
  return 1;
}

static uint32_t u32le(const uint8_t *p) { return zi_zcl1_read_u32(p); }

static uint64_t u64le(const uint8_t *p) {
  uint64_t lo = (uint64_t)u32le(p);
  uint64_t hi = (uint64_t)u32le(p + 4);
  return lo | (hi << 32);
}

static int copy_guest_path_locked(zi_file_aio_ctx *c, const zi_zcl1_frame *z,
                                 uint64_t path_ptr, uint32_t path_len,
                                 uint8_t **out_path_copy) {
  if (!c || !z || !out_path_copy) return 0;
  *out_path_copy = NULL;
  if (path_len == 0) return emit_error_locked(c, z, "file.aio", "empty path");

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return emit_error_locked(c, z, "file.aio", "no memory");

  const uint8_t *path_bytes = NULL;
  if (!mem->map_ro(mem->ctx, (zi_ptr_t)path_ptr, (zi_size32_t)path_len, &path_bytes) || !path_bytes) {
    return emit_error_locked(c, z, "file.aio", "path out of bounds");
  }
  if (has_embedded_nul(path_bytes, path_len)) {
    return emit_error_locked(c, z, "file.aio", "path contains NUL");
  }

  uint8_t *path_copy = (uint8_t *)malloc(path_len);
  if (!path_copy) return emit_error_locked(c, z, "file.aio", "oom");
  memcpy(path_copy, path_bytes, path_len);
  *out_path_copy = path_copy;
  return 1;
}

static uint32_t dtype_from_dirent(unsigned char dt) {
  switch (dt) {
    case DT_REG:
      return (uint32_t)ZI_FILE_AIO_DTYPE_FILE;
    case DT_DIR:
      return (uint32_t)ZI_FILE_AIO_DTYPE_DIR;
    case DT_LNK:
      return (uint32_t)ZI_FILE_AIO_DTYPE_SYMLINK;
    case DT_UNKNOWN:
      return (uint32_t)ZI_FILE_AIO_DTYPE_UNKNOWN;
    default:
      return (uint32_t)ZI_FILE_AIO_DTYPE_OTHER;
  }
}

static int handle_req_locked(zi_file_aio_ctx *c, const zi_zcl1_frame *z) {
  if (!c || !z) return 0;

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_OPEN) {
    if (z->payload_len < 20u) return emit_error_locked(c, z, "file.aio", "bad OPEN payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t oflags = u32le(z->payload + 12);
    uint32_t create_mode = u32le(z->payload + 16);

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_OPEN;
    j.rid = z->rid;
    j.u.open.path = path_copy;
    j.u.open.path_len = path_len;
    j.u.open.oflags = oflags;
    j.u.open.create_mode = create_mode;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }

    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_CLOSE) {
    if (z->payload_len != 8u) return emit_error_locked(c, z, "file.aio", "bad CLOSE payload");
    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_CLOSE;
    j.rid = z->rid;
    j.u.close.file_id = u64le(z->payload);
    if (!enqueue_job_locked(c, &j)) return emit_error_locked(c, z, "file.aio", "queue full");
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_READ) {
    if (z->payload_len != 24u) return emit_error_locked(c, z, "file.aio", "bad READ payload");
    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_READ;
    j.rid = z->rid;
    j.u.read.file_id = u64le(z->payload + 0);
    j.u.read.offset = u64le(z->payload + 8);
    j.u.read.max_len = u32le(z->payload + 16);
    j.u.read.flags = u32le(z->payload + 20);
    if (j.u.read.flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");
    if (!enqueue_job_locked(c, &j)) return emit_error_locked(c, z, "file.aio", "queue full");
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_WRITE) {
    if (z->payload_len != 32u) return emit_error_locked(c, z, "file.aio", "bad WRITE payload");

    uint64_t file_id = u64le(z->payload + 0);
    uint64_t offset = u64le(z->payload + 8);
    uint64_t src_ptr = u64le(z->payload + 16);
    uint32_t src_len = u32le(z->payload + 24);
    uint32_t flags = u32le(z->payload + 28);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");
    if (src_len > ZI_FILE_AIO_MAX_WRITE) return emit_error_locked(c, z, "file.aio", "write too large");

    const zi_mem_v1 *mem = zi_runtime25_mem();
    if (!mem || !mem->map_ro) return emit_error_locked(c, z, "file.aio", "no memory");

    uint8_t *data = NULL;
    if (src_len) {
      const uint8_t *src = NULL;
      if (!mem->map_ro(mem->ctx, (zi_ptr_t)src_ptr, (zi_size32_t)src_len, &src) || !src) {
        return emit_error_locked(c, z, "file.aio", "src out of bounds");
      }
      data = (uint8_t *)malloc(src_len);
      if (!data) return emit_error_locked(c, z, "file.aio", "oom");
      memcpy(data, src, src_len);
    }

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_WRITE;
    j.rid = z->rid;
    j.u.write.file_id = file_id;
    j.u.write.offset = offset;
    j.u.write.data = data;
    j.u.write.len = src_len;
    j.u.write.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(data);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }

    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_MKDIR) {
    if (z->payload_len != 20u) return emit_error_locked(c, z, "file.aio", "bad MKDIR payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t mode = u32le(z->payload + 12);
    uint32_t flags = u32le(z->payload + 16);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_MKDIR;
    j.rid = z->rid;
    j.u.mkdir.path = path_copy;
    j.u.mkdir.path_len = path_len;
    j.u.mkdir.mode = mode;
    j.u.mkdir.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_RMDIR) {
    if (z->payload_len != 16u) return emit_error_locked(c, z, "file.aio", "bad RMDIR payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t flags = u32le(z->payload + 12);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_RMDIR;
    j.rid = z->rid;
    j.u.rmdir.path = path_copy;
    j.u.rmdir.path_len = path_len;
    j.u.rmdir.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_UNLINK) {
    if (z->payload_len != 16u) return emit_error_locked(c, z, "file.aio", "bad UNLINK payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t flags = u32le(z->payload + 12);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_UNLINK;
    j.rid = z->rid;
    j.u.unlink.path = path_copy;
    j.u.unlink.path_len = path_len;
    j.u.unlink.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_STAT) {
    if (z->payload_len != 16u) return emit_error_locked(c, z, "file.aio", "bad STAT payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t flags = u32le(z->payload + 12);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_STAT;
    j.rid = z->rid;
    j.u.stat.path = path_copy;
    j.u.stat.path_len = path_len;
    j.u.stat.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }
    return emit_ok_empty_locked(c, z);
  }

  if (z->op == (uint16_t)ZI_FILE_AIO_OP_READDIR) {
    if (z->payload_len != 20u) return emit_error_locked(c, z, "file.aio", "bad READDIR payload");
    uint64_t path_ptr = u64le(z->payload + 0);
    uint32_t path_len = u32le(z->payload + 8);
    uint32_t max_bytes = u32le(z->payload + 12);
    uint32_t flags = u32le(z->payload + 16);
    if (flags != 0) return emit_error_locked(c, z, "file.aio", "flags must be 0");

    uint8_t *path_copy = NULL;
    if (!copy_guest_path_locked(c, z, path_ptr, path_len, &path_copy) || !path_copy) return 1;

    zi_aio_job j;
    memset(&j, 0, sizeof(j));
    j.op = (uint16_t)ZI_FILE_AIO_OP_READDIR;
    j.rid = z->rid;
    j.u.readdir.path = path_copy;
    j.u.readdir.path_len = path_len;
    j.u.readdir.max_bytes = max_bytes;
    j.u.readdir.flags = flags;

    if (!enqueue_job_locked(c, &j)) {
      free(path_copy);
      return emit_error_locked(c, z, "file.aio", "queue full");
    }
    return emit_ok_empty_locked(c, z);
  }

  return emit_error_locked(c, z, "file.aio", "unknown op");
}

static int32_t aio_write(void *ctx, zi_ptr_t src_ptr, zi_size32_t len) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c || c->closed) return 0;

  const zi_mem_v1 *mem = zi_runtime25_mem();
  if (!mem || !mem->map_ro) return ZI_E_NOSYS;

  const uint8_t *src = NULL;
  if (!mem->map_ro(mem->ctx, src_ptr, len, &src) || !src) return ZI_E_BOUNDS;

  pthread_mutex_lock(&c->mu);
  if (len > (zi_size32_t)(sizeof(c->inbuf) - c->in_len)) {
    pthread_mutex_unlock(&c->mu);
    return ZI_E_BOUNDS;
  }
  memcpy(c->inbuf + c->in_len, src, (size_t)len);
  c->in_len += (uint32_t)len;

  process_pending_requests_locked(c);

  pthread_mutex_unlock(&c->mu);
  return (int32_t)len;
}

static int32_t aio_end(void *ctx) {
  zi_file_aio_ctx *c = (zi_file_aio_ctx *)ctx;
  if (!c) return 0;

  pthread_mutex_lock(&c->mu);
  c->closed = 1;
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);

  if (c->worker_started) {
    (void)pthread_join(c->worker, NULL);
  }

  // Cleanup fds and state.
  for (uint32_t i = 0; i < ZI_FILE_AIO_MAX_FILES; i++) {
    if (c->files[i].in_use && c->files[i].fd >= 0) {
      (void)close(c->files[i].fd);
    }
    c->files[i].in_use = 0;
    c->files[i].fd = -1;
    c->files[i].id = 0;
  }

  if (c->notify_r >= 0) (void)close(c->notify_r);
  if (c->notify_w >= 0) (void)close(c->notify_w);

  if (c->rootfd >= 0) (void)close(c->rootfd);
  c->rootfd = -1;

  free(c->outbuf);
  c->outbuf = NULL;
  c->out_cap = 0;

  // Free any queued jobs payloads.
  for (uint32_t k = 0; k < c->job_count; k++) {
    uint32_t idx = (c->job_head + k) % ZI_FILE_AIO_MAX_JOBS;
    job_free_payload(&c->jobs[idx]);
  }

  pthread_cond_destroy(&c->cv);
  pthread_mutex_destroy(&c->mu);

  memset(c, 0, sizeof(*c));
  free(c);
  return 0;
}

static const zi_handle_ops_v1 OPS = {
    .read = aio_read,
    .write = aio_write,
    .end = aio_end,
};

// ---- cap descriptor ----

static const uint8_t cap_meta[] =
    "{\"kind\":\"file\",\"name\":\"aio\",\"open\":{\"params\":\"(empty)\"},"
  "\"ops\":[\"OPEN\",\"CLOSE\",\"READ\",\"WRITE\",\"MKDIR\",\"RMDIR\",\"UNLINK\",\"STAT\",\"READDIR\"],\"ev\":[\"DONE\"]}";

static const zi_cap_v1 CAP = {
    .kind = ZI_CAP_KIND_FILE,
    .name = ZI_CAP_NAME_AIO,
    .version = 1,
    .cap_flags = ZI_CAP_CAN_OPEN,
    .meta = cap_meta,
    .meta_len = (uint32_t)(sizeof(cap_meta) - 1),
};

const zi_cap_v1 *zi_file_aio25_cap(void) { return &CAP; }

int zi_file_aio25_register(void) { return zi_cap_register(&CAP); }

zi_handle_t zi_file_aio25_open_from_params(zi_ptr_t params_ptr, zi_size32_t params_len) {
  (void)params_ptr;
  if (params_len != 0) return (zi_handle_t)ZI_E_INVALID;

  zi_file_aio_ctx *c = (zi_file_aio_ctx *)calloc(1, sizeof(*c));
  if (!c) return (zi_handle_t)ZI_E_OOM;

  c->notify_r = -1;
  c->notify_w = -1;
  c->next_file_id = 1;
  c->submit_full = 0;

  c->out_cap = 65536u;
  if (c->out_cap > (uint32_t)ZI_FILE_AIO_MAX_OUT) c->out_cap = (uint32_t)ZI_FILE_AIO_MAX_OUT;
  c->outbuf = (uint8_t *)malloc((size_t)c->out_cap);
  if (!c->outbuf) {
    free(c);
    return (zi_handle_t)ZI_E_OOM;
  }

  c->root_enabled = 0;
  c->rootfd = -1;
  c->root_open_errno = 0;

  const char *root = getenv("ZI_FS_ROOT");
  if (root && root[0] != '\0') {
    c->root_enabled = 1;
    c->rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (c->rootfd < 0) c->root_open_errno = errno;
  }

  if (pthread_mutex_init(&c->mu, NULL) != 0) {
    free(c->outbuf);
    free(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  if (pthread_cond_init(&c->cv, NULL) != 0) {
    pthread_mutex_destroy(&c->mu);
    free(c->outbuf);
    free(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }

  // Use a pipe as a wakeup notifier for sys/loop.
  // Readiness itself is provided via get_ready() (level-triggered).
  int fds[2];
  if (pipe(fds) != 0) {
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c->outbuf);
    free(c);
    return (zi_handle_t)ZI_E_IO;
  }
  c->notify_r = fds[0];
  c->notify_w = fds[1];
  set_nonblocking_best_effort(c->notify_r);
  set_nonblocking_best_effort(c->notify_w);

  if (pthread_create(&c->worker, NULL, worker_main, c) != 0) {
    (void)close(c->notify_r);
    (void)close(c->notify_w);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c->outbuf);
    free(c);
    return (zi_handle_t)ZI_E_INTERNAL;
  }
  c->worker_started = 1;

  uint32_t hflags = ZI_H_READABLE | ZI_H_WRITABLE | ZI_H_ENDABLE;
  zi_handle_t h = zi_handle25_alloc_with_poll(&OPS, &POLL_OPS, c, hflags);
  if (h == 0) {
    (void)aio_end(c);
    return (zi_handle_t)ZI_E_OOM;
  }
  return h;
}
