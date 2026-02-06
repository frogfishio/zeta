# instrument (conceptual spec, v0)

`instrument` is the “analysis and rewriting” toolchain around `sircore` execution.

It reuses the same engine and host ABI as `sem` (pure `zi_ctl`), but adds:

- richer tracing and event outputs
- coverage and profiling sidecars
- delta-minimization and triage across corpora
- fuzzing and deterministic perturbation (“shake”)

The most important architectural constraint:

> All tools are built on `sircore`’s structured event stream. No VM forks.

## 1. Execution-backed tools

- `trace`: record step/mem/hostcall events
- `coverage`: mark executed nodes/terms; merge; blackholes
- `profile`: histograms and counts derived from traces
- `fuzz`: coverage-guided stdin fuzzing (in-process)
- `shake`: repeat runs with deterministic perturbations (short reads, poison, redzones)

## 2. Execution-free tools (rewrites / analysis)

These can operate on structured IR (frontends decide format):

- `strip`: remove or rewrite uncovered regions using a coverage profile
- `rep-scan`: repeated n-gram detection (exact/shape)
- `irdiff`: diff IR streams
- `min-ir`: delta-minimize a failing IR stream (black-box reducer)

## 3. Record/replay

A high-ROI capability is the ability to record `zi_ctl` responses and replay them:

- capture all host interactions deterministically
- rerun without host dependencies
- enables regression tests and certificates

