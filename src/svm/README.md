# SVM — SIR Virtual Machine (sircore + runtime + JIT)

`svm` is the “full VM” tier: think “V8 for SIR”.

Where `sircore` is a strict interpreter, `svm` adds:

- JIT and/or AOT compilation for hot paths
- managed memory / GC (eventually aligned with `gc:v1`)
- runtime services (frames, safepoints, metadata)
- performance instrumentation and adaptive optimization

`svm` is expected to be usable both:

- as a **library** embedding target (language runtimes can link to it)
- and via an integrated toolchain CLI (likely as a mode/subcommand of `sem`)

## Compatibility contract

The north star is that `svm` remains compatible with `sircore`:

- `sircore` defines the reference semantics (deterministic traps, edge cases, etc.)
- `svm` must match those semantics, or explicitly declare where it differs (and why)

## Relationship to `instrument`

`instrument` focuses on analysis and tooling around execution.

`svm` focuses on execution performance and runtime services.

In practice, `svm` should reuse the same event/trace schema as `sircore`
so existing instrumentation tools keep working.
