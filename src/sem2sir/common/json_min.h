#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GritJsonCursor {
    const char *p;
    const char *end;
} GritJsonCursor;

// Initializes a cursor over a JSON buffer.
static inline GritJsonCursor grit_json_cursor(const char *buf, size_t len) {
    GritJsonCursor c;
    c.p = buf;
    c.end = buf + len;
    return c;
}

// Skips whitespace.
bool grit_json_skip_ws(GritJsonCursor *c);

// Parses a JSON string into a newly-allocated C string (NUL-terminated).
// Returns false on parse error.
bool grit_json_parse_string_alloc(GritJsonCursor *c, char **out);

// Skips a JSON value (object/array/string/number/true/false/null).
// Returns false on parse error.
bool grit_json_skip_value(GritJsonCursor *c);

// Consumes an expected single character after skipping whitespace.
// Returns false if the character does not match.
bool grit_json_consume_char(GritJsonCursor *c, char expected);

// Parses a JSON integer number into an int64.
// Returns false on parse error.
bool grit_json_parse_int64(GritJsonCursor *c, int64_t *out);

// Parses a JSON boolean (true/false).
// Returns false on parse error.
bool grit_json_parse_bool(GritJsonCursor *c, bool *out);

// Parses a root object and returns the value of a named string field.
// Example: { "start": "Program" }.
// Returns false if the field is missing or is not a string.
bool grit_json_get_root_string_field_alloc(const char *buf, size_t len, const char *field, char **out);
