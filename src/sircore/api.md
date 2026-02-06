# sircore API (conceptual, v0)

This document sketches the public API boundary for `sircore` as a library.

`sircore` executes an in-memory SIR module and interacts with the host solely via `zi_ctl` messages.

## 1. Core types

### 1.1 Module and VM

- `SirModule` (immutable after finalize)
- `SirVm` (execution state)

### 1.2 Host interface

`sircore` does not link to a runtime ABI directly. Instead, the embedding tool provides:

```
typedef int64_t (*zi_ctl_fn)(
  void* user,
  const uint8_t* req, size_t req_len,
  uint8_t* resp, size_t resp_cap
);
```

`sircore` holds `{zi_ctl_fn fn; void* user;}`.

## 2. Module construction

`sircore` should support a two-phase model:

1) **build**: caller adds records (types/syms/nodes/optional src)
2) **finalize**: `sircore` resolves refs, builds tables, validates invariants, then freezes

Conceptual API:

- `SirModuleBuilder* sir_module_builder_new(SirArena* a);`
- `bool sir_module_add_type(builder, SirTypeRec);`
- `bool sir_module_add_sym(builder, SirSymRec);`
- `bool sir_module_add_node(builder, SirNodeRec);`
- `bool sir_module_add_src(builder, SirSrcRec);` (optional)
- `SirModule* sir_module_finalize(builder, SirDiagSink* diags);`

Notes:

- IDs may be strings or ints in the frontend, but `sircore` should work on dense ids (u32/u64 indices).
- The finalize step is where “defs before uses” vs forward refs are resolved.

## 3. Execution

### 3.1 Run modes

- `run`: execute until exit/trap
- `step`: execute one node/term at a time (for tooling)

Conceptual API:

- `SirVm* sir_vm_new(SirModule* m, SirHost host, SirVmOptions opt);`
- `SirRunResult sir_vm_run(SirVm* vm);`
- `SirStepResult sir_vm_step(SirVm* vm);`

### 3.2 Determinism and traps

`sircore` should define deterministic traps for:

- bounds errors (e.g. `ptr.offset` out of range if specified)
- misalignment (if SIR uses deterministic trap semantics)
- invalid host responses (malformed `zi_ctl` framing, span out-of-range, etc.)

## 4. Observability (events)

The embedding tool may register an event sink:

- `on_step` (node id, tag, block/pc if applicable)
- `on_mem` (read/write, addr, size)
- `on_hostcall` (selector, sizes, rc)
- `on_trap` (code, about)

The key idea: instrumentation lives outside the VM, built on stable events.

## 5. Minimum supported SIR subset (MVP)

Start by matching the “integrator stage” node-frontend subset used by `sircc`:

- consts, arithmetic, loads/stores, blocks/terms, calls as needed for “hello world”
- no GC/eh/coro initially

The exact supported set should be discoverable and versioned similarly to `sircc --print-support`.

