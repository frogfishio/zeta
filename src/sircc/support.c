// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "support.h"

#include "json.h"

#include "sircc_support_table.generated.h"

#include <string.h>

static void print_text_list(FILE* out, const char* title, const char* const* items, size_t count, bool full) {
  fprintf(out, "%s (%zu)\n", title, count);
  size_t limit = full ? count : (count < 25 ? count : 25);
  for (size_t i = 0; i < limit; i++) {
    fprintf(out, "  - %s\n", items[i]);
  }
  if (!full && count > limit) {
    fprintf(out, "  ... (%zu more; pass --full)\n", (size_t)(count - limit));
  }
}

bool sircc_print_support(FILE* out, SirccSupportFormat fmt, bool full) {
  if (!out) return false;

  // Summary counts.
  size_t missing_total = 0;
  size_t missing_core = 0;
  for (size_t i = 0; i < sircc_support_missing_by_pack_count; i++) {
    const SirccSupportList* l = &sircc_support_missing_by_pack[i];
    missing_total += l->count;
    if (l->pack && strcmp(l->pack, "core") == 0) missing_core = l->count;
  }

  if (fmt == SIRCC_SUPPORT_JSON) {
    fprintf(out, "{");
    fprintf(out, "\"tool\":\"sircc\"");
    fprintf(out, ",\"ir\":");
    json_write_escaped(out, sircc_support_ir);

    fprintf(out, ",\"spec\":{");
    fprintf(out, "\"source\":");
    json_write_escaped(out, sircc_support_spec_source);
    fprintf(out, ",\"mnemonics\":%zu", sircc_support_spec_all_count);
    fprintf(out, ",\"core_mnemonics\":%zu", sircc_support_spec_core_count);
    fprintf(out, "}");

    fprintf(out, ",\"implemented\":{");
    fprintf(out, "\"mnemonics\":%zu", sircc_support_impl_in_spec_count);
    fprintf(out, "}");

    fprintf(out, ",\"missing\":{");
    fprintf(out, "\"mnemonics\":%zu", missing_total);
    fprintf(out, ",\"core_mnemonics\":%zu", missing_core);
    fprintf(out, ",\"by_pack\":[");
    for (size_t i = 0; i < sircc_support_missing_by_pack_count; i++) {
      if (i) fprintf(out, ",");
      const SirccSupportList* l = &sircc_support_missing_by_pack[i];
      fprintf(out, "{");
      fprintf(out, "\"pack\":");
      json_write_escaped(out, l->pack ? l->pack : "core");
      fprintf(out, ",\"count\":%zu", l->count);
      if (full) {
        fprintf(out, ",\"mnemonics\":[");
        for (size_t j = 0; j < l->count; j++) {
          if (j) fprintf(out, ",");
          json_write_escaped(out, l->items[j]);
        }
        fprintf(out, "]");
      }
      fprintf(out, "}");
    }
    fprintf(out, "]}");

    fprintf(out, ",\"milestone3\":{");
    fprintf(out, "\"candidates\":%zu", sircc_support_m3_candidates_count);
    fprintf(out, ",\"missing\":%zu", sircc_support_m3_missing_count);
    if (full) {
      fprintf(out, ",\"missing_mnemonics\":[");
      for (size_t i = 0; i < sircc_support_m3_missing_count; i++) {
        if (i) fprintf(out, ",");
        json_write_escaped(out, sircc_support_m3_missing[i]);
      }
      fprintf(out, "]");
    }
    fprintf(out, "}");

    fprintf(out, "}\n");
    return true;
  }

  fprintf(out, "sircc support (%s)\n", sircc_support_ir ? sircc_support_ir : "unknown");
  fprintf(out, "  spec: %zu mnemonics (core: %zu)\n", sircc_support_spec_all_count, sircc_support_spec_core_count);
  fprintf(out, "  implemented: %zu\n", sircc_support_impl_in_spec_count);
  fprintf(out, "  missing: %zu (core: %zu)\n", missing_total, missing_core);
  fprintf(out, "  milestone3: %s (%zu candidates)\n", sircc_support_m3_missing_count == 0 ? "OK" : "MISSING", sircc_support_m3_candidates_count);
  fprintf(out, "\n");

  if (sircc_support_m3_missing_count > 0) {
    print_text_list(out, "MILESTONE 3 MISSING", sircc_support_m3_missing, sircc_support_m3_missing_count, true);
    fprintf(out, "\n");
  }

  for (size_t i = 0; i < sircc_support_missing_by_pack_count; i++) {
    const SirccSupportList* l = &sircc_support_missing_by_pack[i];
    if (!l->pack) continue;
    char title[128];
    snprintf(title, sizeof(title), "Missing (%s)", l->pack);
    print_text_list(out, title, l->items, l->count, full);
    fprintf(out, "\n");
  }

  return true;
}
