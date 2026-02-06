#include "zi_telemetry.h"

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
  char out[256];
  zi_telemetry_clock clk = {.ts_ms = 42};

  // Raw JSON object body should embed without quotes.
  const char *topic = "api";
  const char *body_obj = "{\"ok\":true}";
  size_t n = zi_telemetry_format_jsonl(&clk, (const uint8_t *)topic, 3,
                                       (const uint8_t *)body_obj,
                                       (uint32_t)strlen(body_obj), out,
                                       sizeof(out));
  if (!n) {
    fprintf(stderr, "format failed\n");
    return 1;
  }
  if (!assert_contains(out, "\"ts\":42", "ts")) return 1;
  if (!assert_contains(out, "\"topic\":\"api\"", "topic")) return 1;
  if (!assert_contains(out, "\"body\":{\"ok\":true}", "raw body")) return 1;

  // Non-JSON body should be emitted as a JSON string.
  const char *body_txt = "hello";
  n = zi_telemetry_format_jsonl(&clk, (const uint8_t *)topic, 3,
                               (const uint8_t *)body_txt,
                               (uint32_t)strlen(body_txt), out, sizeof(out));
  if (!n) {
    fprintf(stderr, "format failed (txt)\n");
    return 1;
  }
  if (!assert_contains(out, "\"body\":\"hello\"", "string body")) return 1;

  // Ensure escaping works.
  const char *body_esc = "a\n\"b\"";
  n = zi_telemetry_format_jsonl(&clk, (const uint8_t *)topic, 3,
                               (const uint8_t *)body_esc,
                               (uint32_t)strlen(body_esc), out, sizeof(out));
  if (!n) {
    fprintf(stderr, "format failed (esc)\n");
    return 1;
  }
  if (!assert_contains(out, "a\\n\\\"b\\\"", "escapes")) return 1;

  return 0;
}
