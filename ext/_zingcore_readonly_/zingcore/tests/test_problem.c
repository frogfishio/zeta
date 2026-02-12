#include "zi_problem.h"

#include <stdio.h>
#include <string.h>

static int assert_contains(const char *haystack, const char *needle, const char *msg) {
  if (!strstr(haystack, needle)) {
    fprintf(stderr, "assert_contains failed: %s\nneedle: %s\nhaystack: %s\n", msg, needle,
            haystack);
    return 0;
  }
  return 1;
}

int main(void) {
  zi_problem_details p;
  zi_problem_init(&p, ZI_ERR_NOT_FOUND, "missing", "A1B2C3D4E5F6G7H8I9J0");
  if (p.status != 404) {
    fprintf(stderr, "expected status 404, got %u\n", p.status);
    return 1;
  }

  if (!zi_problem_chain_push(&p, ZI_ERR_INVALID_REQUEST, "bad input", "parse", 123)) {
    fprintf(stderr, "chain push failed\n");
    return 1;
  }

  char json[512];
  size_t n = zi_problem_to_json(&p, json, sizeof(json));
  if (!n) {
    fprintf(stderr, "zi_problem_to_json failed\n");
    return 1;
  }

  if (!assert_contains(json, "\"type\":\"urn:zi-error:not_found\"",
                       "type/id"))
    return 1;
  if (!assert_contains(json, "\"status\":404", "status")) return 1;
  if (!assert_contains(json, "\"detail\":\"missing\"", "detail")) return 1;
  if (!assert_contains(json, "\"trace\":\"A1B2C3D4E5F6G7H8I9J0\"", "trace"))
    return 1;
  if (!assert_contains(json, "\"chain\":[{", "chain exists")) return 1;
  if (!assert_contains(json, "\"error\":\"invalid_request\"", "chain error"))
    return 1;
  if (!assert_contains(json, "\"stage\":\"parse\"", "chain stage")) return 1;
  if (!assert_contains(json, "\"at\":123", "chain at")) return 1;

  // Ensure optional trace is omitted when NULL.
  zi_problem_init(&p, ZI_ERR_SERVICE_ERROR, "boom", NULL);
  zi_problem_chain_push(&p, ZI_ERR_SYSTEM_ERROR, "inner", NULL, 1);
  n = zi_problem_to_json(&p, json, sizeof(json));
  if (!n) {
    fprintf(stderr, "zi_problem_to_json failed (no trace)\n");
    return 1;
  }
  if (strstr(json, "\"trace\":")) {
    fprintf(stderr, "trace key should be omitted when NULL\n");
    return 1;
  }

  return 0;
}
