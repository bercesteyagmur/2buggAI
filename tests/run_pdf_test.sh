#!/usr/bin/env bash
# Integration test of the automatic PDF export: runs the real binary on a small C project
# with all LLM calls answered by tests/mock_llm_server.py (offline, deterministic) and
# checks the PDFs that are written next to the JSON report.
#
# Usage (from the repository root, after `make`):  tests/run_pdf_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0

[ -x "$BUGGY" ] || { echo "buggy is not built - run 'make' first"; exit 1; }

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$PORT_FILE")"

FAKE_GEMINI="fake-gemini-key-AAAA"
FAKE_CLAUDE="sk-fake-claude-123456"
FAKE_OPENAI="sk-fake-openai-987654"

# C program with a missing semicolon (same as tests/run_mock_tests.sh)
make_project() {
    mkdir -p "$1/proj"
    cp "$ROOT/errorchecklist.txt" "$1/"
    cat > "$1/proj/main.c" <<'SRC'
#include <stdio.h>

int main(void) {
    int x = 5;
    printf("%d\n", x);
    return 0
}
SRC
}

# run_case <name> <buggy arguments...>  -> sets DIR, OUT, RC
run_case() {
    local name="$1"; shift
    DIR="$WORK/$name"
    make_project "$DIR"
    [ -n "${PROJECT_CONFIG:-}" ] && printf '%s\n' "$PROJECT_CONFIG" > "$DIR/proj/.buggai.json"
    OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" \
        BUGGAI_DEBUGGER=gemini BUGGAI_REVIEWER=claude BUGGAI_VERIFIER=openai \
        GEMINI_API_KEY=$FAKE_GEMINI GEMINI_MODEL=mock-gemini \
        CLAUDE_API_KEY=$FAKE_CLAUDE CLAUDE_MODEL=mock-claude \
        OPENAI_API_KEY=$FAKE_OPENAI OPENAI_MODEL=mock-openai \
        BUGGAI_GEMINI_ENDPOINT=$BASE/normal/gemini/v1beta/openai/chat/completions \
        BUGGAI_CLAUDE_ENDPOINT=$BASE/normal/claude/v1/chat/completions \
        BUGGAI_OPENAI_ENDPOINT=$BASE/normal/openai/v1/chat/completions \
        OPENAI_BASE_URL=$BASE/normal/legacy \
        timeout 300 "$BUGGY" --path "$DIR/proj" --fix "missing semicolon" "$@" 2>&1)"
    RC=$?
    printf '%s\n' "$OUT" > "$WORK/$name.log"
}

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
has()      { grep -qF -- "$1" <<<"$OUT"; }
hasnt()    { ! grep -qF -- "$1" <<<"$OUT"; }
fixed()    { grep -qF 'return 0;' "$DIR/proj/main.c"; }
is_pdf()   { [ -s "$1" ] && [ "$(head -c 5 "$1")" = "%PDF-" ] && tail -c 6 "$1" | grep -q '%%EOF'; }
pdf_has()  { grep -aqF -- "$2" "$1"; }       # the content streams are not compressed
no_pdfs()  { [ -z "$(find "$DIR" -name '*.pdf' -print -quit)" ]; }
# every *.json in a folder has a PDF with the same name
all_json_have_pdf() { local j; for j in "$1"/*.json; do is_pdf "${j%.json}.pdf" || return 1; done; }
valid_json() { python3 -I -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if 'ai_analysis' in d else 1)" "$1"; }

echo "== default: --json-out writes the JSON and, automatically, the PDFs"
mkdir -p "$WORK/default/out"          # the tool does not create the folder of --json-out
PROJECT_CONFIG="" run_case default --json-out "$WORK/default/out/report.json"
check "run succeeds"                         test "$RC" -eq 0
check "bug fixed (fix loop unchanged)"       fixed
check "JSON report written and complete"     valid_json "$DIR/out/report.json"
check "PDF for every JSON file"              all_json_have_pdf "$DIR/out"
check "report.pdf is a PDF"                  is_pdf "$DIR/out/report.pdf"
check "combined report exists"               is_pdf "$DIR/out/report.combined.pdf"
check "export is logged"                     has "PDF Report gespeichert: $DIR/out/report.pdf"
check "header: file name and run id"         pdf_has "$DIR/out/report.pdf" "(report.json) Tj"
check "footer: page numbers"                 pdf_has "$DIR/out/report.pdf" "(Page 1 of "
check "fix report layout"                    pdf_has "$DIR/out/report.pdf" "(2buggAI Fehlerbericht) Tj"
check "summary of the run"                   pdf_has "$DIR/out/report.pdf" "(  Gefunden: 1   ) Tj"
check "section of the fixed error"           pdf_has "$DIR/out/report.pdf" "(1. syntax_error in main.c) Tj"
check "faulty line with its number, marked"  pdf_has "$DIR/out/report.pdf" '(    6 > ) Tj'
check "final analysis in the PDF"            pdf_has "$DIR/out/report.pdf" "(Gesamtanalyse des Modells) Tj"
check "no raw JSON by default"               bash -c "! grep -aqF '(\"detected_errors\")' '$DIR/out/report.pdf'"
check "combined report lists the document"   pdf_has "$DIR/out/report.combined.pdf" "(Documents \(1\), each starting on a new page:)"
check "no per-iteration PDF by default"      test ! -e "$DIR/out/report.iter01.pdf"
check "no leftover temp files"               bash -c "! ls '$DIR/out' | grep -q '\.tmp$'"
check "no API keys in the PDFs"              bash -c "! grep -aqE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$DIR'/out/*.pdf"

echo "== report.include_raw_json_appendix: the JSON as before, after the fix sections"
mkdir -p "$WORK/appendix/out"
PROJECT_CONFIG='{"report": {"include_raw_json_appendix": true}}' run_case appendix --json-out "$WORK/appendix/out/report.json"
check "run succeeds"                         test "$RC" -eq 0
check "appendix title"                       pdf_has "$DIR/out/report.pdf" "(Anhang: roher JSON-Report) Tj"
check "JSON keys in the PDF"                 pdf_has "$DIR/out/report.pdf" '("detected_errors")'
check "final analysis key in the JSON"       pdf_has "$DIR/out/report.pdf" '("ai_analysis")'
check "source code as a block"               pdf_has "$DIR/out/report.pdf" '$.source_code.content'
check "block has numbered lines"             pdf_has "$DIR/out/report.pdf" '(    1 | )'
check "no API keys in the PDFs"              bash -c "! grep -aqE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$DIR'/out/*.pdf"

echo "== per iteration: --pdf-per-iteration --pdf-dir"
PROJECT_CONFIG="" run_case iteration --json-out "$WORK/iteration/report.json" --pdf-per-iteration --pdf-dir "$WORK/iteration/reports"
check "run succeeds"                         test "$RC" -eq 0
check "PDF of the fix iteration"             is_pdf "$DIR/reports/report.iter01.pdf"
check "iteration PDF names the error"        pdf_has "$DIR/reports/report.iter01.pdf" '("syntax_error")'
check "iteration PDF has the fixed code"     pdf_has "$DIR/reports/report.iter01.pdf" '$.fixed_code'
check "final PDF in the output folder"       is_pdf "$DIR/reports/report.pdf"
check "nothing next to the JSON"             test ! -e "$DIR/report.pdf"
check "combined report has both documents"   pdf_has "$DIR/reports/report.combined.pdf" "(Documents \(2\), each starting on a new page:)"
check "JSON still next to --json-out"        valid_json "$DIR/report.json"

echo "== --no-pdf: output exactly as before"
PROJECT_CONFIG="" run_case nopdf --json-out "$WORK/nopdf/report.json" --no-pdf
check "run succeeds"                         test "$RC" -eq 0
check "JSON report written"                  valid_json "$DIR/report.json"
check "no PDF anywhere"                      no_pdfs
check "no PDF log line"                      hasnt "PDF Report gespeichert"

echo "== without --json-out nothing changes"
PROJECT_CONFIG="" run_case nojson
check "run succeeds"                         test "$RC" -eq 0
check "bug fixed"                            fixed
check "no PDF anywhere"                      no_pdfs

echo "== without --json-out but with --pdf-dir: PDF of the report"
PROJECT_CONFIG="" run_case pdfdir --pdf-dir "$WORK/pdfdir/reports"
check "run succeeds"                         test "$RC" -eq 0
check "PDF named after the project"          is_pdf "$DIR/reports/proj_report.pdf"
check "contains the fix section"             pdf_has "$DIR/reports/proj_report.pdf" "(1. syntax_error in main.c) Tj"
check "contains the final analysis"          pdf_has "$DIR/reports/proj_report.pdf" '$.ai_analysis'
check "no JSON file was created"             bash -c "[ -z \"\$(find '$DIR' -name '*report*.json' -print -quit)\" ]"

echo "== project config: <project>/.buggai.json pdf_export"
PROJECT_CONFIG='{"pdf_export": {"combined": false, "per_iteration": true, "output_dir": "pdf-out"}}' \
    run_case config --json-out "$WORK/config/report.json"
check "run succeeds"                         test "$RC" -eq 0
check "output_dir relative to the project"   is_pdf "$DIR/proj/pdf-out/report.pdf"
check "per_iteration from the config"        is_pdf "$DIR/proj/pdf-out/report.iter01.pdf"
check "combined: false is respected"         test ! -e "$DIR/proj/pdf-out/report.combined.pdf"

PROJECT_CONFIG='{"pdf_export": {"enabled": false}}' run_case configoff --json-out "$WORK/configoff/report.json"
check "enabled: false -> no PDF"             no_pdfs
check "JSON report still written"            valid_json "$DIR/report.json"

echo "== a failing PDF export does not abort the run"
PROJECT_CONFIG="" run_case failing --json-out "$WORK/failing/report.json" --pdf-per-iteration --pdf-dir /dev/null/reports
check "exit code 0"                          test "$RC" -eq 0
check "failure is logged"                    has "[PDF] Export fehlgeschlagen"
check "bug still fixed"                      fixed
check "fix loop finished"                    has "-> fixed"
check "JSON report complete"                 valid_json "$DIR/report.json"
check "final analysis still printed"         has "===== DEBUG REPORT (OpenAI) ====="
check "no PDF was written"                   no_pdfs

echo
if [ "$FAILS" -eq 0 ]; then echo "PDF export test passed."; else echo "$FAILS check(s) failed. Logs: $WORK"; trap 'kill $SERVER_PID 2>/dev/null' EXIT; fi
exit $((FAILS > 0))
