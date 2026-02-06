# INSTRUMENT — SIR instrumentation toolchain (analysis + transforms)

`instrument` is to `sem` what “advanced tooling mode” is to an emulator: it runs SIR via `sircore`, but focuses on **debug, instrumentation, profiling, fuzzing, and IR rewriting**.

This folder’s scope is intentionally ambitious; use `zem` as the reference UX for what a “serious emulator toolchain” can do.

Important boundary:

- `sircore` is the deterministic engine and event source (library).
- `instrument` is a consumer/producer of those events and provides higher-level tools.

Host calls are still only performed via the pure message ABI (`zi_ctl`); `instrument` may wrap/record/replay/perturb those host responses.


zem --help
zem — zasm IR v1.1 emulator (minimal)

Usage:
  zem [--help] [--version] [--caps] [--trace] [--trace-mem]
      [--trace-jsonl-out PATH]
      [--trace-mnemonic M] [--trace-pc N[..M]] [--trace-call-target T] [--trace-sample N]
      [--coverage] [--coverage-out PATH] [--coverage-merge PATH] [--coverage-blackholes N]
      [--pgo-len-out PATH]
      [--fuzz --fuzz-iters N [--fuzz-len N] [--fuzz-mutations N] [--fuzz-seed S]
            [--fuzz-out PATH] [--fuzz-crash-out PATH] [--fuzz-print-every N] [--fuzz-continue]]
      [--strip MODE --strip-profile PATH [--strip-out PATH]]
      [--rep-scan --rep-n N --rep-mode MODE --rep-out PATH [--rep-coverage-jsonl PATH] [--rep-diag]]
      [--stdin PATH] [--emit-cert DIR] [--cert-max-mem-events N]
      [--debug] [--debug-script PATH] [--debug-events] [--debug-events-only]
      [--source-name NAME]
      [--break-pc N] [--break-label L] [--break FILE:LINE] [<input.jsonl|->...]
      [--sniff] [--sniff-fatal]
      [--shake [--shake-iters N] [--shake-seed S] [--shake-start N]
              [--shake-heap-pad N] [--shake-heap-pad-max N] [--shake-poison-heap]
              [--shake-redzone N] [--shake-quarantine N] [--shake-poison-free]
              [--shake-io-chunking] [--shake-io-chunk-max N]]
      [--inherit-env] [--clear-env] [--env KEY=VAL]... [--params <guest-arg>...]

Info:
  --help            Print this help and exit
  --version         Print version and exit
  --caps            Print loaded host capabilities and registered selectors
  --strip MODE       Rewrite IR JSONL using a coverage profile (no execution)
                    MODE: uncovered-ret | uncovered-delete
  --strip-profile PATH Coverage JSONL produced by --coverage-out
  --strip-out PATH   Write stripped IR JSONL to PATH (default: stdout)
  --strip-stats-out PATH Write strip stats JSONL to PATH (or '-' for stderr)
  --rep-scan         Analyze IR JSONL for repetition (no execution)
  --rep-n N          N-gram length (e.g. 8)
  --rep-mode MODE    MODE: exact | shape
  --rep-out PATH     Write repetition report JSONL to PATH (or '-' for stdout)
  --rep-max-report N Emit up to N zem_rep_ngram records (top repeated n-grams)
  --rep-coverage-jsonl PATH Optional coverage JSONL to enrich bloat score
  --rep-diag         Print one-line bloat_diag summary to stdout
  --params          Stop option parsing; remaining args become guest argv
  --                Alias for --params
  --inherit-env      Snapshot host environment for zi_env_get_*
  --clear-env        Clear the env snapshot (default: empty)
  --env KEY=VAL      Add/override an env entry in the snapshot (repeatable)
  --cert-max-mem-events N  Cap cert mem events per step (default: built-in)

Stream mode:
  If no input files are provided, zem reads the program IR JSONL from stdin
  (equivalent to specifying a single '-' input).

Supported (subset):
  - Directives: DB, DW, RESB, STR
  - Instructions: LD, ADD, SUB, AND, OR, XOR, INC, DEC, CP, JR,
                  CALL, RET, shifts/rotates, mul/div/rem, LD*/ST*
  - Primitives (Zingcore ABI v2, preferred):
               CALL zi_abi_version   (HL=0x00020005)
               CALL zi_ctl           (HL=req_ptr64, DE=req_len, BC=resp_ptr64, IX=resp_cap, HL=n_or_err)
               CALL zi_alloc         (HL=size, HL=ptr_or_err)
               CALL zi_free          (HL=ptr, HL=rc)
               CALL zi_enum_alloc    (HL=key_lo, DE=key_hi, BC=slot_size, DE:HL=ptr_or_0)
               CALL zi_read          (HL=h, DE=dst_ptr64, BC=cap, HL=n_or_err)
               CALL zi_write         (HL=h, DE=src_ptr64, BC=len, HL=n_or_err)
               CALL zi_end           (HL=h, HL=rc)
               CALL zi_telemetry     (HL=topic_ptr64, DE=topic_len, BC=msg_ptr64, IX=msg_len, HL=rc)
               CALL zi_cap_count     (HL=n_or_err)
               CALL zi_cap_get_size  (HL=i, HL=need_or_err)
               CALL zi_cap_get       (HL=i, DE=out_ptr64, BC=cap, HL=written_or_err)
               CALL zi_cap_open      (HL=req_ptr64, HL=h_or_err)
               CALL zi_handle_hflags (HL=h, HL=hflags_or_0)
               CALL zi_argc          (HL=argc)
               CALL zi_argv_len      (HL=i, HL=len_or_err)
               CALL zi_argv_copy     (HL=i, DE=out_ptr64, BC=cap, HL=written_or_err)
               CALL zi_env_get_len   (HL=key_ptr64, DE=key_len, HL=len_or_err)
               CALL zi_env_get_copy  (HL=key_ptr64, DE=key_len, BC=out_ptr64, IX=cap, HL=written_or_err)
               CALL zi_hop_alloc     (HL=scope, DE=size, BC=align, DE:HL=ptr_or_0)
               CALL zi_hop_alloc_buf (HL=scope, DE=cap, DE:HL=buf_or_0)
               CALL zi_hop_mark      (HL=scope, HL=mark)
               CALL zi_hop_release   (HL=scope, DE=mark, BC=wipe, HL=rc)
               CALL zi_hop_reset     (HL=scope, DE=wipe, HL=rc)
               CALL zi_hop_used      (HL=scope, HL=used)
               CALL zi_hop_cap       (HL=scope, HL=cap)

  - Legacy underscore primitives are not supported.

  --coverage-out PATH Write coverage JSONL to PATH
  --pgo-len-out PATH Write PGO JSONL: observed BC lengths for FILL/LDIR
  --coverage-merge PATH Merge existing coverage JSONL from PATH into this run
  --coverage-blackholes N Print top-N labels with uncovered instructions
  --trace-mnemonic M  Only emit step events whose mnemonic == M (repeatable)
  --trace-pc N[..M]   Only emit step events with pc in [N, M] (inclusive)
  --trace-call-target T Only emit step events for CALLs with target == T (repeatable)
  --trace-sample N    Emit 1 out of every N step events (deterministic)
  --trace-mem         Emit memory read/write JSONL events to stderr
  --stdin PATH        Use PATH as guest stdin (captured for replay/certs)
  --fuzz              Run a simple in-process coverage-guided stdin fuzzer
  --fuzz-iters N      Number of mutated runs (default: 1000)
  --fuzz-len N        Fixed stdin length in bytes (default: stdin file size, else 64)
  --fuzz-mutations N  Byte flips per iteration (default: 4)
  --fuzz-unlock       Enable a concolic-lite branch unlocker for stdin
  --fuzz-unlock-tries N Max unlock attempts per iteration (default: 4)
  --fuzz-unlock-trace Emit one-line predicate traces for unlock suggestions
  --fuzz-seed S       RNG seed (default: 1; accepts 0x.. too)
  --fuzz-out PATH     Write best-found stdin input to PATH
  --fuzz-crash-out PATH Write first crashing stdin input to PATH
  --fuzz-print-every N Print progress every N iterations (default: 0)
  --fuzz-continue     Keep fuzzing after a failing run
  --emit-cert DIR     Emit a trace-validity certificate (SMT-LIB) into DIR
  --sniff             Proactively warn about suspicious runtime patterns
  --sniff-fatal       Like --sniff but stop execution on detection
  --shake             Run the program multiple times with deterministic perturbations
  --shake-iters N      Number of runs (default: 100)
  --shake-seed S       Seed for shake RNG (default: derived from time/pid)
  --shake-start N      Starting run index (default: 0)
  --shake-heap-pad N   Fixed heap-base padding per run (bytes; useful for replay)
  --shake-heap-pad-max N  Random heap-base padding per run: [0..N] bytes
  --shake-poison-heap  Poison newly-allocated heap bytes (surfaces uninit reads)
  --shake-redzone N    Add N-byte redzones around zi_alloc/_alloc allocations
  --shake-quarantine N Track up to N freed spans; fault on access (UAF surfacing)
  --shake-poison-free  Poison freed regions (best-effort; pairs well with quarantine)
  --shake-io-chunking  Force short reads for zi_read/req_read/_in
  --shake-io-chunk-max N Max short-read chunk size (default: 64)
  --break-pc N        Break when pc (record index) == N
  --break-label L     Break at label L (first instruction after label record)
  --break FILE:LINE   Break at first instruction mapped to FILE:LINE via v1.1 src/src_ref
  --debug             Interactive CLI debugger (break/step/regs/bt)
  --debug-script PATH Run debugger commands from PATH (no prompt; exit on EOF).
                    Note: --debug-script - reads debugger commands from stdin;
                    this cannot be combined with reading program IR JSONL from stdin
                    (either via '-' or via stream mode with no inputs).
  --debug-events      Emit JSONL dbg_stop events to stderr on each stop
  --debug-events-only Like --debug-events but suppress debugger text output
                    and suppresses zem lifecycle telemetry.
  --source-name NAME  Source name to report when reading program JSONL from stdin ('-')

Tools:
  --irdiff ...       Diff IR JSONL
  --min-ir ...       Delta-minimize IR JSONL
  --triage ...       Run + group failures over corpora
  --duel ...         Differential runner (A/B)


---


## Conceptual spec (what lives here)

### Event-driven tooling (no VM forks)

All tooling should be built on a single execution engine (`sircore`) that emits structured events:

- step events (node id/tag, basic-block/pc where applicable)
- memory events (loads/stores, sizes, logical addresses)
- host call events (`zi_ctl` selector + sizes + rc)
- trap/diag events (code + about)

Tooling features are implemented as consumers/producers of event streams, not as VM modifications.

### High-ROI tools (initial target set)

Mirror the `zem` set, adjusted for SIR:

- **Trace**: text + JSONL; filters by mnemonic/tag, id ranges, src_ref, call target; sampling.
- **Coverage**: node/term coverage, optionally per-src_ref; merge profiles; blackholes (“never executed” hotspots).
- **Profiles**: histograms (branch outcomes, bulk-mem lengths, call targets, etc.).
- **Strip**: rewrite SIR streams using a coverage profile (delete uncovered, or replace with deterministic return).
- **Minimize**: delta-minimize failing inputs (keep failure signature stable).
- **Triage**: run corpora, cluster failures by signature.
- **Fuzz**: in-process fuzzing with coverage guidance; optional concolic-lite unlocker for stdin-driven programs.
- **Shake**: deterministic perturbation runs (heap redzones/poison, short reads, env snapshots, etc.).
- **Cert** (later): emit trace validity certificates for selected properties (SMT-LIB or similar).

### Output contracts

Tool outputs should be machine-readable and composable:

- diagnostics as JSONL (stderr)
- traces as JSONL (file/pipe)
- coverage/profiles as JSONL sidecars
- rewrite outputs as SIR JSONL (or later CBOR) emitted by the frontend

## Relationship to KLEE-like workflows

Once `sircore` provides a stepper + event stream + deterministic host ABI, `instrument` can grow symbolic/concolic tooling:

- treat `zi_ctl` reads (stdin, env, file) as symbolic sources
- explore path conditions using a solver
- emit minimized reproducer inputs and path traces

This is deliberately staged: first build robust concrete execution and tooling (like `zem`), then grow symbolic execution.
