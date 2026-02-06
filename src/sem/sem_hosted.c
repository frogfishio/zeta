#include "sem_hosted.h"

static uint32_t hz_abi_version(void* u) { return sir_zi_abi_version((sir_hosted_zabi_t*)u); }
static int32_t hz_ctl(void* u, zi_ptr_t a, zi_size32_t b, zi_ptr_t c, zi_size32_t d) { return sir_zi_ctl((sir_hosted_zabi_t*)u, a, b, c, d); }
static int32_t hz_read(void* u, zi_handle_t h, zi_ptr_t p, zi_size32_t n) { return sir_zi_read((sir_hosted_zabi_t*)u, h, p, n); }
static int32_t hz_write(void* u, zi_handle_t h, zi_ptr_t p, zi_size32_t n) { return sir_zi_write((sir_hosted_zabi_t*)u, h, p, n); }
static int32_t hz_end(void* u, zi_handle_t h) { return sir_zi_end((sir_hosted_zabi_t*)u, h); }
static zi_ptr_t hz_alloc(void* u, zi_size32_t n) { return sir_zi_alloc((sir_hosted_zabi_t*)u, n); }
static int32_t hz_free(void* u, zi_ptr_t p) { return sir_zi_free((sir_hosted_zabi_t*)u, p); }
static int32_t hz_telemetry(void* u, zi_ptr_t a, zi_size32_t b, zi_ptr_t c, zi_size32_t d) { return sir_zi_telemetry((sir_hosted_zabi_t*)u, a, b, c, d); }
static int32_t hz_cap_count(void* u) { return sir_zi_cap_count((sir_hosted_zabi_t*)u); }
static int32_t hz_cap_get_size(void* u, int32_t i) { return sir_zi_cap_get_size((sir_hosted_zabi_t*)u, i); }
static int32_t hz_cap_get(void* u, int32_t i, zi_ptr_t p, zi_size32_t n) { return sir_zi_cap_get((sir_hosted_zabi_t*)u, i, p, n); }
static zi_handle_t hz_cap_open(void* u, zi_ptr_t p) { return sir_zi_cap_open((sir_hosted_zabi_t*)u, p); }
static uint32_t hz_handle_hflags(void* u, zi_handle_t h) { return sir_zi_handle_hflags((sir_hosted_zabi_t*)u, h); }

sir_host_t sem_hosted_make_host(sir_hosted_zabi_t* hz) {
  sir_host_t host = {0};
  host.user = hz;
  host.v = (sir_host_vtable_t){
      .zi_abi_version = hz_abi_version,
      .zi_ctl = hz_ctl,
      .zi_read = hz_read,
      .zi_write = hz_write,
      .zi_end = hz_end,
      .zi_alloc = hz_alloc,
      .zi_free = hz_free,
      .zi_telemetry = hz_telemetry,
      .zi_cap_count = hz_cap_count,
      .zi_cap_get_size = hz_cap_get_size,
      .zi_cap_get = hz_cap_get,
      .zi_cap_open = hz_cap_open,
      .zi_handle_hflags = hz_handle_hflags,
  };
  return host;
}

