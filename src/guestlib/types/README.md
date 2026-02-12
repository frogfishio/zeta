# guestlib/types

Small data structures intended for sem-subset guest code.

- `vector.sir`: a growable contiguous array (`vector_new`, `vector_push_copy`, `vector_get_ptr`, ...)
- `hashmap.sir`: an open-addressing hash map from byte-string keys to `ptr` values (`hashmap_put`, `hashmap_get`, `hashmap_iter_next`, ...)

These are meant as building blocks for higher-level libraries like JSON parsing/serialization.
