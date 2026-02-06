#pragma once

#include "hosted_zabi.h"
#include "sircore_vm.h"

// Build a sir_host_t vtable that forwards to a hosted zABI runtime.
sir_host_t sem_hosted_make_host(sir_hosted_zabi_t* hz);

