# zABI v2.5 — Runtime Semantics (WIP)

This document is the normative description of *host-side* zABI 2.5 runtime behavior.
It is intended to be implemented by `src/zingcore/2.5` and verified by conformance tests.

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described in RFC 2119.

## Scope

This document covers:

- Capability registry semantics
- Async selector registry semantics (registration, discovery, enumeration)

It does **not** yet specify the full set of `env.zi_*` imports.

## Design principles

- The ABI is small and sharp: fewer concepts, stronger rules.
- Behavior is deterministic and testable.
- Registration is explicit; no constructor or linker-section side effects.

## Capability identity and naming

A capability is identified by the tuple:

- `kind` (e.g. `"exec"`)
- `name` (e.g. `"run"`)
- `version` (an integer interface/selector version)

The canonical printed form of a capability identity is:

`kind/name` (for kind+name) and `kind/name@v<version>` (when including version).

Rules:

- `kind` and `name` MUST be non-empty strings.
- Callers MUST treat `kind` and `name` as case-sensitive.
- A runtime MUST NOT accept a cap registration with a NULL `kind` or NULL `name`.
- A runtime MUST reject duplicate capability registrations with the same (`kind`, `name`, `version`).

Note: the set of valid characters for `kind`/`name` is currently "stringly"; further tightening is allowed as long as it is deterministic and consistently enforced.

## Capability registry semantics

The runtime provides a process-global capability registry.

- The registry MUST be explicitly initialized before use.
- Initialization MUST be idempotent (safe to call multiple times).
- Registration order MUST be preserved for enumeration.
- Enumeration MUST be stable across calls within a process lifetime.

Introspection:

- The runtime MUST provide an API to enumerate all registered caps.
- Enumeration MUST NOT rely on probing hacks.

Errors:

- Registration MUST fail if the registry is not initialized.
- Registration MUST fail if capacity is exhausted.

## Async selector identity and naming

An async selector is identified by the tuple:

- `cap_kind`
- `cap_name`
- `selector` (string)

Selector naming rules (zABI 2.5 "by the book"):

- Selector names MUST be relative and versioned.
- Selector names MUST NOT be fully-qualified.

"Relative and versioned" means:

- The selector string MUST include a version suffix of the form `.v<digits>`.
	- Examples: `run.v1`, `ping.v1`, `read.v2`.
- The selector string MUST NOT begin with `<cap_kind>.`.
	- Example: for `cap_kind == "exec"`, `exec.run.v1` MUST be rejected.
- The selector string MUST NOT contain whitespace, ASCII control characters, `/`, or `\`.

Rationale:

- Relative naming avoids repeating identity already provided by (`cap_kind`, `cap_name`).
- Version suffixing makes selector evolution explicit.

## Async selector registry semantics

The runtime provides a process-global selector registry.

Initialization and enumeration:

- The selector registry MUST be explicitly initialized before use.
- Initialization MUST be idempotent.
- Registration order MUST be preserved for enumeration.
- The runtime MUST provide an API to enumerate all registered selectors.

Coupling to capabilities:

- A selector MUST NOT be registerable unless the corresponding capability (`cap_kind`, `cap_name`) has already been registered.
	- This prevents “dangling” implementations without an advertised capability.

Errors:

- Registration MUST fail if the selector is invalid (NULL fields, missing invoke callback, invalid selector name).
- Registration MUST fail on duplicate selector identity (`cap_kind`, `cap_name`, `selector`).

Discovery:

- The runtime SHOULD provide a lookup API by bytes (`kind`, `name`, `selector`) to support wire-encoded names.

## Explicit registration model

The embedding program (host runtime glue) is responsible for explicit registration.
The canonical startup sequence is:

1. Initialize zingcore (or registries): `zingcore25_init()` (or `zi_caps_init()` + `zi_async_init()`).
2. Register capabilities: `zi_cap_register(...)`.
3. Register selectors: `zi_async_register(...)`.

The runtime MUST NOT require constructor or linker-section based auto-registration for correctness.

## Errors and telemetry

zABI 2.5 standardizes a portable error payload shape inspired by RFC 7807 Problem Details.
This is used for host-side telemetry emission and for any boundary where errors need to be
serialized as bytes.

### Error identifiers

- Errors MUST have a stable, machine-readable identifier string (e.g. `not_found`).
- The set of identifiers is an append-only registry: new identifiers MAY be added, but existing identifiers MUST NOT be renamed.

### Trace (origin identifier)

`trace` is a human-entered unique origin identifier created at the site where the error is born.

- The runtime and host code SHOULD treat `trace` as an *origin id*, not a request correlation id.
- `trace` SHOULD be short and grep-friendly (often ~20 chars alphanumeric).
- When present, `trace` MUST be preserved through chaining/wrapping so that a single grep of the entire codebase locates the creation site.

Rationale: avoids “error helpers” and preserves debuggability across layers.

### Error chaining

Errors MAY be chained as they move through layers.

- A chain hop MUST include: `error`, `error_description`, `at`.
- A chain hop MAY include: `stage`.
- Chaining SHOULD preserve a single error identity across layers (mutate by appending a hop) rather than creating an unrelated new error object.

### Public JSON payload shape

When serialized, an error MUST follow this JSON object shape (field order is not significant,
but implementations SHOULD emit a stable order for diffability):

```json
{
	"type": "urn:zi-error:<id>",
	"title": "<short human title>",
	"status": <number>,
	"detail": "<human description>",
	"trace": "<origin id>",
	"chain": [
		{
			"error": "<id>",
			"error_description": "<human description>",
			"stage": "<optional stage>",
			"at": <epoch ms>
		}
	]
}
```

Rules:

- `chain` MUST be present and MUST be an array (it MAY be empty).
- `trace` SHOULD be omitted when not present.
- `status` uses HTTP status codes as a familiar classification, even when the transport is not HTTP.

### Telemetry emission

For telemetry events representing an error inside an event body (not in the topic), the body SHOULD include the serialized Problem Details JSON.
The telemetry topic SHOULD NOT be used to encode the structured error fields.

For development, hosts MAY implement telemetry by emitting JSONL/NDJSON lines to stderr.
This is intentionally "magic" and easy to pipe (`... 2>telemetry.ndjson`).
More advanced transports (firehose relays, collectors, sampling, routing) are out of scope for the core zingcore runtime and may be provided by host tooling.
