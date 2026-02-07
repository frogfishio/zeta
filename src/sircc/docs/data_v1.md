# `data:v1` — baseline data story (verifier-enforced)

`data:v1` is a **convention pack**: it does not add new mnemonics, but it gives
producers a single, portable “data story” for the most common higher-level
datatypes (bytes, UTF-8 strings, and C strings).

Unlike pure documentation, `data:v1` is **enforced by `sircc --verify-only`**:
if a stream enables `data:v1` via `meta.ext.features`, the canonical types MUST
be present and have the exact shapes described below.

## Canonical types

Enable the pack:

```json
{"ir":"sir-v1.0","k":"meta","ext":{"features":["data:v1"]}}
```

Then define these types (ids may be integers or strings):

### `bytes`

An owned or borrowed byte slice.

- `bytes = struct { data: ptr(i8), len: i64 }`
- `len` is a byte count.
- This is **not** NUL-terminated; embedded `0` bytes are allowed.

### `string.utf8`

A UTF-8 string slice.

- `string.utf8 = struct { data: ptr(i8), len: i64 }`
- Bytes are UTF-8 by convention.
- Not NUL-terminated; embedded `0` bytes are allowed.

### `cstr`

A C string pointer (NUL-terminated bytes).

- `cstr = ptr(i8)` (a ptr type whose `of` is `i8`)
- The trailing `\0` is a **producer/host convention**, not validated by SIR.

## Interop guidance (portable, explicit)

- Prefer passing raw `(ptr(i8), len)` to “write-like” ABIs (`zi_write`, etc.).
- Only use `cstr` when calling ABIs that require NUL-terminated strings.
- Conversions between `string.utf8` and `cstr` should be explicit calls to a
  runtime helper (zABI / host-provided capability), not implicit compiler magic.

## Examples

- `src/sircc/examples/data_v1_ok.sir.jsonl` (positive)
- `src/sircc/examples/bad_data_v1_string_wrong_fields.sir.jsonl` (negative)
