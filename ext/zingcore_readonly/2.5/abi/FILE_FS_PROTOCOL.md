# file/fs v1 Protocol Specification

**Version:** 1  
**Capability Name:** `file/fs`  
**Status:** Golden (Stable)

## Overview

The `file/fs` capability provides sandboxed filesystem access to guest programs. It implements the standard handle stream protocol (`zi_read`, `zi_write`, `zi_end`) over files opened via ZCL1 control operations.

**Key Design Principles:**
- **Sandbox First:** All filesystem access is restricted by the `ZI_FS_ROOT` environment variable
- **Stream Semantics:** Files are accessed via standard handle operations (read/write/end)
- **ZCL1 Control Plane:** File open/stat operations use ZCL1 for structured requests

## Sandbox Model

### ZI_FS_ROOT Environment Variable

The runtime **MUST** enforce filesystem sandboxing via the `ZI_FS_ROOT` environment variable:

```bash
ZI_FS_ROOT=/path/to/sandbox zrun program.wat
```

**Semantics:**
- All guest-visible paths are relative to `ZI_FS_ROOT`
- Guest path `/foo/bar.txt` resolves to `${ZI_FS_ROOT}/foo/bar.txt` on the host
- Paths outside `ZI_FS_ROOT` (via `..` or symlinks) **MUST** result in `EACCES` (permission denied)
- If `ZI_FS_ROOT` is unset or empty, the capability **MUST NOT** be registered

### Path Normalization

Implementations **MUST**:
1. Normalize all guest paths (collapse `..`, deduplicate `/`)
2. Resolve symlinks within the sandbox
3. Reject access to any path that escapes the sandbox root
4. Use canonical path comparison to prevent TOCTOU attacks

## ZCL1 Operations

All operations use the standard ZCL1 frame format (see `ZCL1_PROTOCOL.md`).

### 1. OPEN (op=1)

Opens a file and returns a handle for streaming I/O.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | flags       | Open flags (bitfield, see below)
4      | 4    | mode        | File mode (POSIX permissions, for create)
8      | N    | path        | UTF-8 path string (NOT null-terminated)
```

**Open Flags (bitfield):**
```
0x0001  O_READ      - Open for reading
0x0002  O_WRITE     - Open for writing
0x0004  O_APPEND    - Append mode (writes go to end)
0x0008  O_CREATE    - Create if does not exist
0x0010  O_EXCL      - Fail if exists (with O_CREATE)
0x0020  O_TRUNC     - Truncate to zero length
0x0040  O_DIRECTORY - Fail if not a directory
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | handle      | New handle ID (>=3)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `2` (ENOENT) - File does not exist
- `13` (EACCES) - Permission denied (sandbox escape, mode mismatch)
- `17` (EEXIST) - File exists (O_EXCL)
- `20` (ENOTDIR) - Path component is not a directory
- `21` (EISDIR) - Cannot open directory for writing
- `28` (ENOSPC) - No space left on device

**Behavioral Requirements:**
- Opened handles support `zi_read`, `zi_write`, `zi_end`
- `zi_end` on a file handle **MUST** close the file and release the handle
- Multiple handles to the same file are independent streams
- Handle flags (`ZI_H_READABLE`, `ZI_H_WRITABLE`) reflect open mode

### 2. STAT (op=2)

Retrieves file metadata without opening the file.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | N    | path        | UTF-8 path string (NOT null-terminated)
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 8    | size        | File size in bytes
8      | 8    | mtime       | Modification time (Unix timestamp, seconds)
16     | 4    | mode        | File mode (POSIX permissions)
20     | 4    | kind        | File kind (0=file, 1=dir, 2=symlink, 3=other)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `2` (ENOENT) - File does not exist
- `13` (EACCES) - Permission denied

### 3. UNLINK (op=3)

Removes a file or empty directory.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | N    | path        | UTF-8 path string (NOT null-terminated)
```

**Response Payload (success, status=0):**
Empty (zero-length payload).

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `2` (ENOENT) - File does not exist
- `13` (EACCES) - Permission denied
- `39` (ENOTEMPTY) - Directory not empty

### 4. MKDIR (op=4)

Creates a new directory.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | mode        | Directory mode (POSIX permissions)
4      | N    | path        | UTF-8 path string (NOT null-terminated)
```

**Response Payload (success, status=0):**
Empty (zero-length payload).

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `13` (EACCES) - Permission denied
- `17` (EEXIST) - Directory exists
- `20` (ENOTDIR) - Path component is not a directory

### 5. READDIR (op=5)

Lists directory entries.

**Request Payload:**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | N    | path        | UTF-8 path string (NOT null-terminated)
```

**Response Payload (success, status=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | count       | Number of entries
4      | ...  | entries     | Variable-length entry list
```

**Entry Format (repeated `count` times):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | kind        | File kind (0=file, 1=dir, 2=symlink, 3=other)
4      | 4    | name_len    | Length of name string
8      | N    | name        | UTF-8 name string (NOT null-terminated)
```

**Response Payload (error, status!=0):**
```
Offset | Size | Field       | Description
-------|------|-------------|------------------------------------------
0      | 4    | errno       | POSIX errno code
4      | N    | message     | UTF-8 error description
```

**Error Codes:**
- `2` (ENOENT) - Directory does not exist
- `13` (EACCES) - Permission denied
- `20` (ENOTDIR) - Path is not a directory

## Stream Operations

Opened file handles support the standard handle protocol:

### zi_read(handle, dst_ptr, cap)

**Semantics:**
- Reads up to `cap` bytes from the current file position into guest memory at `dst_ptr`
- Returns number of bytes read (0-cap), or negative errno on error
- Returns `0` at EOF
- Advances file position by number of bytes read
- `EBADF` if handle was not opened with `O_READ`

### zi_write(handle, src_ptr, len)

**Semantics:**
- Writes `len` bytes from guest memory at `src_ptr` to the current file position
- Returns number of bytes written (0-len), or negative errno on error
- Advances file position by number of bytes written
- `EBADF` if handle was not opened with `O_WRITE`
- `ENOSPC` if disk full

### zi_end(handle)

**Semantics:**
- Closes the file and releases the handle
- Returns `0` on success, negative errno on error
- Handle becomes invalid after this call
- Idempotent (calling twice is safe)

## Protocol Example

**Opening a file for reading:**

```
// Guest calls zi_ctl with ZCL1 frame:
Magic:       "ZCL1"
Version:     1
Op:          1 (OPEN)
RID:         1001
Status:      0
Reserved:    0
PayloadLen:  13
Payload:     [flags=0x0001, mode=0, path="hello.txt"]

// Runtime responds:
Magic:       "ZCL1"
Version:     1
Op:          1
RID:         1001
Status:      0 (success)
Reserved:    0
PayloadLen:  4
Payload:     [handle=3]

// Guest can now use zi_read(3, ...) to stream file contents
```

**Opening a file for writing (with create):**

```
// Guest calls zi_ctl:
Op:          1 (OPEN)
Payload:     [flags=0x000A (O_WRITE|O_CREATE), mode=0644, path="out.txt"]

// Runtime responds:
Status:      0
Payload:     [handle=4]

// Guest uses zi_write(4, ...) to write data, zi_end(4) to close
```

## Conformance Requirements

Implementations **MUST**:
1. Enforce `ZI_FS_ROOT` sandbox strictly (no path escapes)
2. Support all 5 ZCL1 operations (OPEN, STAT, UNLINK, MKDIR, READDIR)
3. Implement standard stream semantics (read/write/end)
4. Return correct POSIX errno codes for all error conditions
5. Handle partial reads/writes correctly (short counts)
6. Support concurrent open handles to different files
7. Support UTF-8 paths (no null-termination in ZCL1 payloads)

Implementations **SHOULD**:
- Use buffered I/O for performance
- Support large files (>4GB, use 64-bit sizes)
- Provide clear error messages for sandbox violations

Implementations **MAY**:
- Impose limits on open handle count
- Impose limits on path length (but >=1024 bytes recommended)
- Reject non-UTF-8 paths with EILSEQ

## Security Considerations

- **Path Traversal:** Implementations MUST validate all paths against the sandbox root
- **Symlink Attacks:** Follow symlinks only within the sandbox
- **TOCTOU:** Use atomic operations where possible (e.g., O_EXCL)
- **Resource Limits:** Enforce handle count and file size limits to prevent DoS
- **Error Disclosure:** Error messages MUST NOT leak host filesystem structure

## Version History

- **v1 (2.5):** Initial stable specification (golden capability)
