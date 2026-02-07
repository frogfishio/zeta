// SPDX-FileCopyrightText: 2026 Frogfish
// SPDX-License-Identifier: GPL-3.0-or-later

#include "support.h"

#include "json.h"

#include "sircc_support_table.generated.h"

#include <string.h>

static void html_escape(FILE* out, const char* s) {
  if (!out || !s) return;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    switch (*p) {
      case '&':
        fputs("&amp;", out);
        break;
      case '<':
        fputs("&lt;", out);
        break;
      case '>':
        fputs("&gt;", out);
        break;
      case '"':
        fputs("&quot;", out);
        break;
      case '\'':
        fputs("&#39;", out);
        break;
      default:
        fputc((int)*p, out);
        break;
    }
  }
}

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

  if (fmt == SIRCC_SUPPORT_HTML) {
    fprintf(out, "<!doctype html>\n");
    fprintf(out, "<html lang=\"en\">\n");
    fprintf(out, "<meta charset=\"utf-8\">\n");
    fprintf(out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    fprintf(out, "<title>sircc support</title>\n");
    fprintf(out, "<style>\n");
    fprintf(out, ":root{--fg:#111;--muted:#666;--bg:#fff;--line:#e5e5e5;--ok:#0a7;--bad:#c33;--code:#f7f7f7;}\n");
    fprintf(out, "body{font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;line-height:1.35;color:var(--fg);background:var(--bg);margin:24px;max-width:1100px;}\n");
    fprintf(out, "h1{font-size:20px;margin:0 0 6px 0;}\n");
    fprintf(out, "p{margin:0 0 14px 0;color:var(--muted);}\n");
    fprintf(out, "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;}\n");
    fprintf(out, "table{border-collapse:collapse;width:100%%;margin:12px 0 18px 0;}\n");
    fprintf(out, "th,td{border:1px solid var(--line);padding:8px 10px;vertical-align:top;}\n");
    fprintf(out, "th{background:#fafafa;text-align:left;font-weight:600;}\n");
    fprintf(out, ".kpi{display:flex;gap:16px;flex-wrap:wrap;margin:10px 0 6px 0;}\n");
    fprintf(out, ".k{padding:8px 10px;border:1px solid var(--line);border-radius:8px;background:#fafafa;}\n");
    fprintf(out, ".k b{display:block;font-size:12px;color:var(--muted);font-weight:600;}\n");
    fprintf(out, ".k span{display:block;font-size:16px;}\n");
    fprintf(out, ".ok{color:var(--ok);font-weight:700;}\n");
    fprintf(out, ".bad{color:var(--bad);font-weight:700;}\n");
    fprintf(out, ".mn{white-space:nowrap;}\n");
    fprintf(out, "ul{margin:6px 0 0 18px;padding:0;}\n");
    fprintf(out, "li{margin:2px 0;}\n");
    fprintf(out, "</style>\n");
    fprintf(out, "<h1>sircc support</h1>\n");
    fprintf(out, "<p>Generated by <code>sircc --print-support</code> (do not edit). Spec source: <code>");
    html_escape(out, sircc_support_spec_source ? sircc_support_spec_source : "(unknown)");
    fprintf(out, "</code>.</p>\n");

    fprintf(out, "<div class=\"kpi\">\n");
    fprintf(out, "  <div class=\"k\"><b>IR</b><span>");
    html_escape(out, sircc_support_ir ? sircc_support_ir : "(unknown)");
    fprintf(out, "</span></div>\n");
    fprintf(out, "  <div class=\"k\"><b>Spec mnemonics</b><span>%zu</span></div>\n", sircc_support_spec_all_count);
    fprintf(out, "  <div class=\"k\"><b>Spec core mnemonics</b><span>%zu</span></div>\n", sircc_support_spec_core_count);
    fprintf(out, "  <div class=\"k\"><b>Implemented (in spec)</b><span>%zu</span></div>\n", sircc_support_impl_in_spec_count);
    fprintf(out, "  <div class=\"k\"><b>Missing</b><span class=\"%s\">%zu</span></div>\n",
            missing_total == 0 ? "ok" : "bad", missing_total);
    fprintf(out, "  <div class=\"k\"><b>Missing core</b><span class=\"%s\">%zu</span></div>\n",
            missing_core == 0 ? "ok" : "bad", missing_core);
    fprintf(out, "  <div class=\"k\"><b>Milestone 3</b><span class=\"%s\">%s</span></div>\n",
            sircc_support_m3_missing_count == 0 ? "ok" : "bad", sircc_support_m3_missing_count == 0 ? "OK" : "MISSING");
    fprintf(out, "</div>\n");

    if (sircc_support_m3_missing_count > 0) {
      fprintf(out, "<h2>Milestone 3 missing (%zu)</h2>\n", sircc_support_m3_missing_count);
      fprintf(out, "<table><thead><tr><th>Mnemonic</th></tr></thead><tbody>\n");
      for (size_t i = 0; i < sircc_support_m3_missing_count; i++) {
        fprintf(out, "<tr><td class=\"mn\"><code>");
        html_escape(out, sircc_support_m3_missing[i]);
        fprintf(out, "</code></td></tr>\n");
      }
      fprintf(out, "</tbody></table>\n");
    }

    fprintf(out, "<h2>Missing by pack</h2>\n");
    fprintf(out, "<table>\n");
    fprintf(out, "<thead><tr><th>Pack</th><th>Missing</th><th>Mnemonics</th></tr></thead>\n");
    fprintf(out, "<tbody>\n");
    for (size_t i = 0; i < sircc_support_missing_by_pack_count; i++) {
      const SirccSupportList* l = &sircc_support_missing_by_pack[i];
      const char* pack = l->pack ? l->pack : "core";
      fprintf(out, "<tr><td><code>");
      html_escape(out, pack);
      fprintf(out, "</code></td><td>%zu</td><td>", l->count);
      if (l->count == 0) {
        fprintf(out, "<span class=\"ok\">OK</span>");
      } else {
        size_t limit = full ? l->count : (l->count < 25 ? l->count : 25);
        fprintf(out, "<ul>");
        for (size_t j = 0; j < limit; j++) {
          fprintf(out, "<li><code>");
          html_escape(out, l->items[j]);
          fprintf(out, "</code></li>");
        }
        if (!full && l->count > limit) {
          fprintf(out, "<li>… (%zu more; pass <code>--full</code>)</li>", (size_t)(l->count - limit));
        }
        fprintf(out, "</ul>");
      }
      fprintf(out, "</td></tr>\n");
    }
    fprintf(out, "</tbody></table>\n");

    // Also embed the JSON summary for tooling (HTML is forgiving of escaped JSON).
    fprintf(out, "<h2>Raw JSON summary</h2>\n");
    fprintf(out, "<pre>");
    // Re-run the JSON path inline (small and stable).
    // NOTE: Keep this in sync with the JSON branch above.
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
    fprintf(out, "}");
    fprintf(out, "</pre>\n");

    fprintf(out, "</html>\n");
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
