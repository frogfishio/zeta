#include "sircore_vm.h"

#include <string.h>

enum {
  ZI_E_INVALID = -1,
  ZI_E_BOUNDS = -2,
  ZI_E_NOSYS = -7,
  ZI_E_INTERNAL = -10,
};

bool sir_vm_init(sir_vm_t* vm, sir_vm_cfg_t cfg) {
  if (!vm) return false;
  memset(vm, 0, sizeof(*vm));

  if (!sem_guest_mem_init(&vm->mem, cfg.guest_mem_cap ? cfg.guest_mem_cap : (16u * 1024u * 1024u),
                          cfg.guest_mem_base ? cfg.guest_mem_base : 0x10000ull)) {
    return false;
  }
  vm->host = cfg.host;
  return true;
}

void sir_vm_dispose(sir_vm_t* vm) {
  if (!vm) return;
  sem_guest_mem_dispose(&vm->mem);
  memset(vm, 0, sizeof(*vm));
}

static int32_t sir_vm_write_bytes(sir_vm_t* vm, zi_handle_t h, const uint8_t* bytes, uint32_t len) {
  if (!vm || !bytes) return ZI_E_INTERNAL;
  if (!vm->host.v.zi_alloc || !vm->host.v.zi_write) return ZI_E_NOSYS;
  if (len == 0) return 0;

  const zi_ptr_t p = vm->host.v.zi_alloc(vm->host.user, len);
  if (!p) return ZI_E_BOUNDS;

  uint8_t* w = NULL;
  if (!sem_guest_mem_map_rw(&vm->mem, p, len, &w) || !w) return ZI_E_BOUNDS;
  memcpy(w, bytes, len);

  const int32_t n = vm->host.v.zi_write(vm->host.user, h, p, len);
  return (n < 0) ? n : 0;
}

int32_t sir_vm_run(sir_vm_t* vm, const sir_ins_t* ins, size_t ins_count) {
  if (!vm) return ZI_E_INTERNAL;
  if (ins_count && !ins) return ZI_E_INVALID;

  for (size_t ip = 0; ip < ins_count; ip++) {
    const sir_ins_t* i = &ins[ip];
    switch (i->k) {
      case SIR_INS_NOP:
        break;
      case SIR_INS_WRITE_BYTES: {
        const int32_t r = sir_vm_write_bytes(vm, i->u.write_bytes.h, i->u.write_bytes.bytes, i->u.write_bytes.len);
        if (r < 0) return r;
        break;
      }
      case SIR_INS_EXIT:
        return i->u.exit_.code;
      default:
        return ZI_E_INVALID;
    }
  }
  return 0;
}
