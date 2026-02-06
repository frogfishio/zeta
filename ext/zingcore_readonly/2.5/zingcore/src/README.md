# zingcore2.5 implementation modules (WIP)

This directory will host the zABI 2.5 runtime implementation.

Target approach:

- Split by concern: `caps`, `async`, `stream`, `mem`, `env`, `argv`, `telemetry`, `exec`.
- Keep each module unit-testable.
- Prefer explicit initialization over constructor-based auto-registration.

Status: placeholder.
