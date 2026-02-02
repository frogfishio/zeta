# zingcore 25-family public headers

This folder contains the public headers for the zingcore **25-family** implementation.

## Naming policy

- **Wire/system ABI:** the stable syscall surface is `zi_*` as declared by the family’s sysabi header
	(for 2.5 that is `zi_sysabi25.h`). These names are *not* family-suffixed.
- **Wiring/implementation:** process-global wiring and convenience APIs are family-namespaced
	(e.g. `zingcore25_*`, `zi_runtime25_*`).

## What “25” means

“25” is a **family namespace**, not “ABI version digits in a function name”.

The intent is that 2.5 / 2.6 / 2.7 / … stay within the same wiring family without renaming exported
wiring symbols on every minor bump. We only introduce a new family number when we make a truly
incompatible wiring break.

## General rules

- Keep includes minimal and C11-friendly.
- Prefer fixed-width integer types at ABI boundaries.
- Avoid leaking internal structs; prefer opaque handles where practical.
