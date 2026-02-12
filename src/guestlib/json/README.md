# guestlib/json

Minimal JSON parse/serialize for sem-subset guest code.

- Parser produces a small owned tree of values (null/bool/num/str/arr/obj).
- Numbers are preserved as a *lexeme* (bytes + len), not converted.
- Strings are unescaped (supports `\\uXXXX` for BMP code points; rejects surrogates).

Main entry points:

- `json_parse(src, len, out_val_ptr)`
- `json_serialize_alloc(val, out_buf_ptr, out_len_ptr)`
- `json_free(val)`
