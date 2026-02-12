# proc/hopper v1 Protocol Specification

**Version:** 1  
**Capability Name:** `proc/hopper`  
**Status:** Golden (Stable)

## Overview

The `proc/hopper` capability provides runtime introspection and dynamic linking for zasm programs. It exposes a **catalog** of available linkable functions that can be invoked via the standard handle protocol.

**Key Design Principles:**
- **Catalog-Driven:** Discovery via ZCL1 CATALOG operation
- **Stream Semantics:** Function invocation via `zi_write` (args) / `zi_read` (results)
- **Optional ABI:** Programs can use proc/hopper *or* direct imports (zi_hopabi25)

## Hopper vs. zi_hopabi25

There are **two ways** to access runtime functionality:

1. **proc/hopper capability** (this spec):
   - Dynamic discovery via CATALOG operation
   - Invocation via handle stream protocol
   - Portable across runtimes (uses ZCL1)

2. **zi_hopabi25 direct imports** (see `ZI_HOPABI25.md`):
   - Static function imports in WASM module
   - Direct function calls (no handle protocol)
   - Faster, but less portable

Programs can use **either** or **both** mechanisms depending on requirements.

## Catalog Model

The proc/hopper capability exposes a **catalog** of available functions. Each function has:
- **Name:** UTF-8 identifier (e.g., `"itoa"`, `"memcpy"`)
- **Signature:** Input/output types (encoded as byte descriptor)
- **Description:** Human-readable help text

Programs query the catalog via ZCL1, then invoke functions via handle operations.

## ZCL1 Operations

### 1. CATALOG (op=1)

Retrieves the catalog of available functions.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | flags       | Reserved (must be 0)
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | count       | Number of function entries
4      | ...  | entries     | Variable-length function list
```

**Function Entry Format (repeated `count` times):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | name_len    | Length of function name
4      | N    | name        | UTF-8 function name (NOT null-terminated)
N+4    | 4    | sig_len     | Length of signature descriptor
N+8    | M    | signature   | Signature descriptor (see below)
N+M+8  | 4    | desc_len    | Length of description
N+M+12 | K    | description | UTF-8 description (NOT null-terminated)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | Error code
4      | N    | message     | UTF-8 error description
```

**Signature Descriptor Format:**

The signature descriptor is a compact binary representation of function type:

```
Byte 0:      Encoding version (0x01)
Byte 1:      Input count (number of input parameters)
Byte 2:      Output count (number of output values)
Byte 3:      Reserved (0)
Bytes 4+:    Type descriptors (one per input, then one per output)
```

**Type Descriptors:**
```
0x01  - i32 (32-bit integer)
0x02  - i64 (64-bit integer)
0x03  - f32 (32-bit float)
0x04  - f64 (64-bit float)
0x10  - ptr (32-bit guest pointer)
0x11  - buffer (ptr + length pair)
```

**Example Signature:**
```c
// Function: i32 itoa(i32 value, ptr buf, i32 cap)
// Returns: i32 (length written)

Encoding: [0x01, 0x03, 0x01, 0x00, 0x01, 0x10, 0x01, 0x01]
          [ver,  in=3, out=1, res,  i32,  ptr,  i32,  i32]
```

### 2. INVOKE (op=2)

Invokes a function and returns a handle for streaming arguments/results.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | name_len    | Length of function name
4      | N    | name        | UTF-8 function name (NOT null-terminated)
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | handle      | New invocation handle (>=3)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | Error code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `2` (ENOENT) - Function not found in catalog
- `12` (ENOMEM) - Out of memory for invocation context

## Stream Operations

Invocation handles support a **request-response** protocol via standard stream operations:

### zi_write(handle, src_ptr, len)

**Semantics:**
- Writes function input arguments to the invocation context
- Arguments are encoded in **little-endian** binary format, matching the signature
- **Must** write the exact number of bytes expected for the input signature
- Returns `len` on success, negative errno on error
- `EINVAL` if argument encoding is malformed

**Argument Encoding:**
- `i32`: 4 bytes, little-endian
- `i64`: 8 bytes, little-endian
- `f32`: 4 bytes, IEEE 754 binary32
- `f64`: 8 bytes, IEEE 754 binary64
- `ptr`: 4 bytes, little-endian guest pointer
- `buffer`: 8 bytes (4-byte ptr + 4-byte length)

### zi_read(handle, dst_ptr, cap)

**Semantics:**
- Reads function output results from the invocation context
- **First** invokes the function (if not already executed)
- Results are encoded in **little-endian** binary format, matching the signature
- Returns number of bytes read (0-cap), or negative errno on error
- Returns `0` after all results consumed
- `EFAULT` if function execution failed

**Result Encoding:**
Same as argument encoding.

### zi_end(handle)

**Semantics:**
- Releases the invocation context and handle
- Returns `0` on success
- Idempotent (safe to call multiple times)

## Protocol Example

**Querying the catalog:**

```
// Guest calls zi_ctl with ZCL1 frame:
Magic:       "ZCL1"
Version:     1
Op:          1 (CATALOG)
RID:         2001
Status:      0
Reserved:    0
PayloadLen:  4
Payload:     [flags=0]

// Runtime responds:
Status:      0
PayloadLen:  ... (catalog entries)
Payload:     [count=2, 
              entry1={name="itoa", sig=[...], desc="Integer to ASCII"},
              entry2={name="memcpy", sig=[...], desc="Memory copy"}]
```

**Invoking a function:**

```
// Guest calls zi_ctl to invoke "itoa":
Op:          2 (INVOKE)
Payload:     [name_len=4, name="itoa"]

// Runtime responds:
Status:      0
Payload:     [handle=5]

// Guest writes input arguments via zi_write(5, ...):
// Arguments: i32 value=1234, ptr buf=0x1000, i32 cap=64
// Encoded: [0xD2, 0x04, 0x00, 0x00,  // value=1234
//           0x00, 0x10, 0x00, 0x00,  // buf=0x1000
//           0x40, 0x00, 0x00, 0x00]  // cap=64
zi_write(5, arg_ptr, 12);

// Guest reads result via zi_read(5, ...):
// Result: i32 length=4
// Encoded: [0x04, 0x00, 0x00, 0x00]
zi_read(5, result_ptr, 4);

// Guest closes invocation:
zi_end(5);
```

## Standard Catalog Functions

Implementations **SHOULD** provide these common functions in the catalog:

### itoa
```
Signature: i32 itoa(i32 value, ptr buf, i32 cap)
Description: Convert integer to ASCII decimal string
Returns: Length written to buf (excluding null terminator)
```

### memcpy
```
Signature: void memcpy(ptr dst, ptr src, i32 len)
Description: Copy len bytes from src to dst
Returns: (none)
```

### strlen
```
Signature: i32 strlen(ptr str)
Description: Compute length of null-terminated string
Returns: Length in bytes (excluding null terminator)
```

### strcmp
```
Signature: i32 strcmp(ptr s1, ptr s2)
Description: Compare null-terminated strings
Returns: <0 if s1<s2, 0 if equal, >0 if s1>s2
```

Implementations **MAY** provide additional functions based on runtime capabilities.

## zi_hopabi25 Integration

If a runtime supports **both** proc/hopper and zi_hopabi25:

1. The catalog **MUST** list all functions available via zi_hopabi25
2. Function signatures **MUST** match between both interfaces
3. Implementations **SHOULD** use the same underlying functions for both paths

This allows programs to:
- Use proc/hopper for dynamic discovery
- Use zi_hopabi25 for performance-critical paths
- Fall back gracefully if zi_hopabi25 is unavailable

## Conformance Requirements

Implementations **MUST**:
1. Support CATALOG operation returning valid function list
2. Support INVOKE operation for all catalog functions
3. Implement request-response protocol via stream operations
4. Use little-endian encoding for all numeric types
5. Validate argument counts and types against signatures
6. Return correct errno codes for all error conditions

Implementations **SHOULD**:
- Provide standard catalog functions (itoa, memcpy, strlen, strcmp)
- Support concurrent invocation handles
- Validate guest pointers in function arguments
- Provide meaningful error messages for type mismatches

Implementations **MAY**:
- Impose limits on invocation handle count
- Extend the catalog with runtime-specific functions
- Cache invocation contexts for repeated calls

## Security Considerations

- **Memory Safety:** Validate all guest pointers before dereferencing
- **Type Safety:** Enforce signature matching strictly
- **Resource Limits:** Impose limits on concurrent invocations to prevent DoS
- **Side Effects:** Catalog functions SHOULD be pure or have well-defined side effects
- **Error Disclosure:** Error messages MUST NOT leak host memory layout

## Performance Notes

- **Overhead:** Stream-based invocation has higher overhead than direct imports
- **Batching:** Implementations MAY batch multiple invocations for efficiency
- **Caching:** Catalog lookups SHOULD be cached by guest programs
- **Fast Path:** Use zi_hopabi25 for hot loops, proc/hopper for rare operations

## Version History

- **v1 (2.5):** Initial stable specification (golden capability)
