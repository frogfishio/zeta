# ztest - Zeta Testing Library

A small, predictable unit-testing library for Zeta programs.

Design goals:

- **Tiny surface area**: tests are normal functions; assertions return integers.
- **Composability**: a test returns `0` for pass, non-zero for fail; runners can aggregate however they want.
- **Useful output**: failures go to **stderr**, results/summary go to **stdout**.
- **No magic**: no global registries required; you can still build suites on top.

---

## Quick start

```sir
@include "ztest/zetest.sir"

;; Example: run a few assertions inside a test.
;; Convention: test returns 0 on pass, non-zero on fail.
fn test_math(state:ptr) -> i32
  let r1: i32 = zetest_expect_eq_i32(state, 1+1, 2, "1+1 should be 2\n", 15)
  let r2: i32 = zetest_expect_true(state, true, "sanity check\n", 13)

  ;; Aggregate assertion results for the test's return code.
  ;; Any non-zero means failure.
  return i32.or(r1, r2)
end

fn main() -> i32 public
  ;; Caller owns the state buffer (two i32s: passed, failed)
  let buf: array(i32, 2)
  let state: ptr = &buf

  let _: i32 = zetest_init(state)

  ;; Run tests (runner prints PASS/FAIL)
  let _: i32 = zetest_run_test(state, "test_math", 9, test_math(state))

  ;; Prints summary and returns failure count as exit code
  return zetest_finalize(state)
end
```

**Note:** for failure messages, include a trailing newline (e.g. `"...\n"`) so multiple failures do not run together.

---

## What you get

- **Assertions**
  - `zetest_expect_eq_i32(state, actual, expected, msg, msg_len)` - equality
  - `zetest_expect_ne_i32(state, actual, expected, msg, msg_len)` - not equal
  - `zetest_expect_lt_i32(state, actual, expected, msg, msg_len)` - less than
  - `zetest_expect_le_i32(state, actual, expected, msg, msg_len)` - less than or equal
  - `zetest_expect_gt_i32(state, actual, expected, msg, msg_len)` - greater than
  - `zetest_expect_ge_i32(state, actual, expected, msg, msg_len)` - greater than or equal
  - `zetest_expect_true(state, condition, msg, msg_len)` - boolean true
  - `zetest_expect_false(state, condition, msg, msg_len)` - boolean false
  - `zetest_expect_eq_str(state, actual, expected, msg, msg_len)` - string equality
  - `zetest_expect_eq_bytes(state, actual, expected, len, msg, msg_len)` - byte buffer equality

- **Runner helpers**
  - `zetest_run_test(state, name, name_len, test_result)` prints `PASS name` / `FAIL name`
  - `zetest_finalize(state)` prints a summary with numeric counts and returns the **failed count** (use as process exit code)

- **Suite helpers**
  - `zetest_suite_begin(suite_name, suite_name_len)` - begin a test suite
  - `zetest_suite_end(state)` - end a test suite with summary

- **Output conventions**
  - Assertion failures: **stderr** (with automatic newline safety)
  - Test results and summary: **stdout** (with numeric counts)

---

## Recommended conventions

These conventions keep the library simple while still producing good diagnostics:

1) **One shared state per process**
   - Initialize once in `main`, pass the `state` pointer into tests.

2) **Tests return a single status code**
   - Combine multiple assertion results using `i32.or(...)`.

3) **Messages should identify the check**
   - Include the condition and (if relevant) values. Example:
     - `"expected foo() == 42\n"`

4) **Keep tests independent**
   - Avoid ordering dependencies between tests.

---

## Example: multiple tests

```sir
@include "ztest/zetest.sir"

fn test_truthy(state:ptr) -> i32
  return zetest_expect_true(state, true, "true should be true\n", 20)
end

fn test_eq(state:ptr) -> i32
  let a: i32 = 123
  let b: i32 = 123
  return zetest_expect_eq_i32(state, a, b, "a should equal b\n", 16)
end

fn main() -> i32 public
  let buf: array(i32, 2)
  let state: ptr = &buf
  let _: i32 = zetest_init(state)

  let _: i32 = zetest_run_test(state, "test_truthy", 11, test_truthy(state))
  let _: i32 = zetest_run_test(state, "test_eq", 7, test_eq(state))

  return zetest_finalize(state)
end
```

---

## Library shape (current)

### State

The test state is an array of two `i32`s:

- `state[0]` = passed
- `state[1]` = failed

You provide the storage (stack or static), and the library writes counts into it.

### Return codes

- Assertions return `0` on success, `1` on failure.
- A test should return `0` if all assertions pass; otherwise return non-zero.
- `zetest_finalize()` returns the failure count (good as a process exit code).

---

## Gaps (next improvements)

If you want this to feel like a “decent” test library fast, these are the highest leverage additions:

- **Fix accounting so failures are always counted**
  - Ensure `failed` increments whenever a test returns non-zero, even if the test didn’t call `zetest_fail`.

- **Better failure diagnostics**
  - Add specialized helpers like `expect_eq_i32` that also print actual/expected values.
  - Include the test name in failure output (or pass it into assertions).

- **Summary with numbers**
  - Add a tiny `i32 -> decimal string` helper so Passed/Failed/Total print real counts.

- **More assertion types**
  - `expect_ne_i32`, `expect_lt_i32`, `expect_gt_i32`
  - `expect_eq_str` (pointer + length)
  - `expect_eq_bytes` (buffers)

- **Suites**
  - Optional `zetest_run_suite(state, suite_name, tests...)` that prints a grouped header.

---

## File layout

- **zetest.sir**: core assertions + runner helpers

---

## API reference

### Core Functions
- `zetest_init(state:ptr) -> i32` - Initialize test state
- `zetest_fail(state:ptr, msg:ptr, msg_len:i32) -> i32` - Record failure with message
- `zetest_run_test(state:ptr, test_name:ptr, test_name_len:i32, test_result:i32) -> i32` - Run test and print result
- `zetest_finalize(state:ptr) -> i32` - Print summary and return failure count

### Assertions
- `zetest_expect_eq_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_ne_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_lt_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_le_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_gt_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_ge_i32(state:ptr, actual:i32, expected:i32, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_true(state:ptr, condition:bool, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_false(state:ptr, condition:bool, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_eq_str(state:ptr, actual:ptr, expected:ptr, msg:ptr, msg_len:i32) -> i32`
- `zetest_expect_eq_bytes(state:ptr, actual:ptr, expected:ptr, len:i32, msg:ptr, msg_len:i32) -> i32`

### Suite Helpers
- `zetest_suite_begin(suite_name:ptr, suite_name_len:i32) -> i32`
- `zetest_suite_end(state:ptr) -> i32`

---

## TODO

- [x] **Fix pass/fail accounting**: count failures whenever a test returns non-zero (not only when an assertion calls `zetest_fail`).
- [x] **Fix compilation issues**: replace `sem.if` with `select()` for compatibility.
- [x] **Summary with numbers**: Added `i32_to_string` helper for numeric output.
- [x] **Newline safety**: `zetest_fail` automatically adds newlines to messages.
- [x] **More assertion types**: Added comparison operators, string/byte assertions.
- [x] **Suites**: Added `zetest_suite_begin/end` helpers.
- [ ] **Better failure diagnostics**: Enhanced `expect_eq_i32` with actual/expected values.
- [ ] **Fix compilation issues**: Resolve parser compatibility (functions with parameters).
- [ ] **Print numeric summary**: implement a tiny `i32 -> decimal string` helper and print real `Passed`, `Failed`, and `Total` counts.
- [ ] **Newline-safe failure output**: ensure failure messages always end with `\n` (append one in `zetest_fail` if missing).
- [ ] **Better `eq_i32` diagnostics**: when `expect_eq_i32` fails, print `expected=` and `actual=` values (and the user message).
- [ ] **Add more assertions**: `expect_ne_i32`, `expect_lt_i32`, `expect_le_i32`, `expect_gt_i32`, `expect_ge_i32`.
- [ ] **String equality assertion**: `expect_eq_str(ptr,len, ptr,len)` with a short diff hint (first mismatch index).
- [ ] **Byte buffer assertion**: `expect_eq_bytes(ptr,len, ptr,len)` with mismatch index and hex byte values.
- [ ] **Suite helper**: `zetest_run_suite_header(state, name, len)` to print a grouped header, and optional per-suite subtotal.
- [ ] **Setup/teardown helpers**: conventions or hooks for suite-level setup and per-test setup/teardown.
- [ ] **Output configurability**: allow configuring stdout/stderr fds in `zetest_init` (or store `out_fd`/`err_fd` in state).
- [ ] **Consistent return codes**: define constants (e.g. `ZETEST_OK=0`, `ZETEST_FAIL=1`) and document them.
- [ ] **Docs polish**: add a “Best practices” section (test independence, naming, avoiding shared state) and a CI example showing exit codes.