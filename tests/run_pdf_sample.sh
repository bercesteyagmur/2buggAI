#!/usr/bin/env bash
# Generates sample PDFs with the 2buggAI PDF exporter and KEEPS them, so you can open
# them. No API key, no LLM and no network needed.
#
#   make pdf-sample          (or: tests/run_pdf_sample.sh after `make json2pdf`)
#
# Output: build/pdf_sample/report.pdf, broken.pdf, fix_report.pdf, report.combined.pdf
#   fix_report.pdf shows the report with fix sections (docs/PLAN_REPORT_REDESIGN.md),
#   the other two the plain JSON layout for JSON files without "fix_report".
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOL="$ROOT/build/json2pdf"
SAMPLES="$ROOT/tests/fixtures/pdf_sample"
OUT="$ROOT/build/pdf_sample"
FAILS=0

[ -x "$TOOL" ] || { echo "build/json2pdf is not built - run 'make json2pdf' first"; exit 1; }

rm -rf "$OUT"
"$TOOL" --out "$OUT" "$SAMPLES/report.json" "$SAMPLES/broken.json" "$SAMPLES/fix_report.json" > "$OUT.log" 2>&1
RC=$?

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
is_pdf()  { [ -s "$1" ] && [ "$(head -c 5 "$1")" = "%PDF-" ] && tail -c 6 "$1" | grep -q '%%EOF'; }
pdf_has() { grep -aqF -- "$2" "$1"; }

check "generator exits with 0"            test "$RC" -eq 0
check "report.pdf generated"              is_pdf "$OUT/report.pdf"
check "broken.pdf generated"              is_pdf "$OUT/broken.pdf"
check "report.combined.pdf generated"     is_pdf "$OUT/report.combined.pdf"
check "header with the file name"         pdf_has "$OUT/report.pdf" "(report.json) Tj"
check "page numbers"                      pdf_has "$OUT/report.pdf" "(Page 1 of "
check "source code as a numbered block"   pdf_has "$OUT/report.pdf" '$.source_code.content'
check "agent diff as a block"             pdf_has "$OUT/report.pdf" '$.agents.debugger.diff'
check "fix recommendations as own field"  pdf_has "$OUT/report.pdf" "(Fix recommendations) Tj"
check "recommendations before the JSON"   bash -c "[ \$(grep -abo '(Fix recommendations) Tj' '$OUT/report.pdf' | head -1 | cut -d: -f1) -lt \$(grep -abo '(JSON report) Tj' '$OUT/report.pdf' | head -1 | cut -d: -f1) ]"
check "api_key is masked"                 bash -c "! grep -aq 'this-must-not-appear' '$OUT'/*.pdf"
check "invalid JSON is reported"          pdf_has "$OUT/broken.pdf" "(Invalid JSON: this file could not be parsed.)"
check "combined report lists all files"   pdf_has "$OUT/report.combined.pdf" "(Documents \(3\), each starting on a new page:)"

FIX="$OUT/fix_report.pdf"
check "fix_report.pdf generated"          is_pdf "$FIX"
check "fix report: title"                 pdf_has "$FIX" "(2buggAI Fehlerbericht) Tj"
check "fix report: summary"               pdf_has "$FIX" "(Nicht behoben: 1) Tj"
check "fix report: faulty code"           pdf_has "$FIX" "(FEHLERHAFTER CODE) Tj"
check "fix report: root cause"            pdf_has "$FIX" "(WARUM DAS EIN FEHLER IST) Tj"
check "fix report: fixed code"            pdf_has "$FIX" "(GEFIXTER CODE) Tj"
check "fix report: not fixed, marked"     pdf_has "$FIX" "(  NICHT BEHOBEN - Iterationen: 5) Tj"
check "fix report: links in the table"    pdf_has "$FIX" "/Subtype /Link"
check "fix report: no raw JSON"           bash -c "! grep -aqF '(\"fix_report\")' '$FIX'"

echo
if [ "$FAILS" -eq 0 ]; then
    echo "PDF sample test passed. Open the files:"
    ls -1 "$OUT"/*.pdf | sed 's/^/  /'
else
    echo "$FAILS check(s) failed. Log: $OUT.log"
fi
exit $((FAILS > 0))
