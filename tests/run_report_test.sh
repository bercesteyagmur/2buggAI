#!/usr/bin/env bash
# Integration test of the report with fix sections (docs/PLAN_REPORT_REDESIGN.md).
#
# Runs the real binary on a small C++ project with three compiler errors; every LLM call is
# answered by tests/mock_llm_server.py (scenario "report": offline and deterministic):
#   - missing semicolon             -> fixed
#   - undeclared variable           -> fixed by inserting a line (shifts the line numbers);
#                                      the mock leaves out fix_explanation on purpose
#   - std::string assigned to int   -> the Verifier rejects every proposal: NOT fixed
# g++ reports the first two under two checklist categories each, so the report has five
# sections: 2 fixed directly, 2 fixed together with them, 1 not fixed.
#
# The PDF text is read with pdftotext when it is installed, otherwise with
# tests/helpers/pdf_text.py (the exporter writes uncompressed streams).
#
# Usage (from the repository root, after `make`):
#   tests/run_report_test.sh
#   REPORT_SAMPLE_OUT=docs/examples/sample_report.pdf tests/run_report_test.sh   # keep the report
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0

[ -x "$BUGGY" ] || { echo "buggy is not built - run 'make' first"; exit 1; }
command -v g++ >/dev/null || { echo "g++ is needed for this test"; exit 1; }

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$PORT_FILE")"

FAKE_GEMINI="fake-gemini-key-AAAA"
FAKE_CLAUDE="sk-fake-claude-123456"
FAKE_OPENAI="sk-fake-openai-987654"
SECRET_IN_CODE="sk-live-0123456789abcdef"
S=report

make_project() {
    mkdir -p "$1/proj" "$1/out"
    cp "$ROOT/errorchecklist.txt" "$1/"
    cat > "$1/proj/main.cpp" <<SRC
#include <iostream>
#include <string>

// Konfiguration (Testwert, darf nie im Report erscheinen)
const char* api_key = "$SECRET_IN_CODE";

int compute(int a) {
    int z = y + a;
    return z * 2;
}

int main() {
    int count = std::string("42");
    std::cout << compute(count) << std::endl;
    return 0
}
SRC
}

# run_case <name> [buggy arguments...]  -> sets DIR, OUT, RC, TXT (PDF text of out/report.pdf)
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
        BUGGAI_GEMINI_ENDPOINT=$BASE/$S/gemini/v1beta/openai/chat/completions \
        BUGGAI_CLAUDE_ENDPOINT=$BASE/$S/claude/v1/chat/completions \
        BUGGAI_OPENAI_ENDPOINT=$BASE/$S/openai/v1/chat/completions \
        OPENAI_BASE_URL=$BASE/$S/legacy \
        timeout 600 "$BUGGY" --path "$DIR/proj" --fix "fix all errors" --json-out "$DIR/out/report.json" "$@" 2>&1)"
    RC=$?
    printf '%s\n' "$OUT" > "$WORK/$name.log"
    TXT="$WORK/$name.txt"
    if [ -s "$DIR/out/report.pdf" ]; then python3 -I "$ROOT/tests/helpers/pdf_text.py" "$DIR/out/report.pdf" > "$TXT"; else : > "$TXT"; fi
}

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
txt()     { grep -qF -- "$1" "$TXT"; }              # text in the PDF
notxt()   { ! grep -qF -- "$1" "$TXT"; }
txtre()   { grep -qE -- "$1" "$TXT"; }
count()   { grep -cF -- "$1" "$TXT"; }
# order <a> <b>: the first occurrence of a comes before the first occurrence of b
order()   { local a b; a=$(grep -nF -- "$1" "$TXT" | head -1 | cut -d: -f1); b=$(grep -nF -- "$2" "$TXT" | head -1 | cut -d: -f1);
            [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; }
# section <n>: text of fix section n (from its title to the next one)
section() { awk -v n="$1" 'BEGIN{p=0} $0 ~ "^"n"\\. " {p=1} p && $0 ~ "^"(n+1)"\\. " {p=0} p {print}' "$TXT"; }
in_section() { section "$1" | grep -qF -- "$2"; }
report()  { python3 -I -c "import json,sys
r=json.load(open('$DIR/out/report.json'))
f=r['fix_report']; fixes={x['error_name']: x for x in f['fixes']}
sys.exit(0 if ($1) else 1)"; }
# reqs <python expression over rows (mock requests of this scenario; dir = case folder)>
reqs()    { python3 -I -c "import json,sys,re
rows=[json.loads(l) for l in open('$REQ_LOG')]
rows=[r for r in rows if r['scenario']=='$S']
for r in rows: r['dir']=(re.search(r'$WORK/([^/]+)/', r.get('first_file') or '') or [None,''])[1]
sys.exit(0 if ($1) else 1)"; }
is_pdf()  { [ -s "$1" ] && [ "$(head -c 5 "$1")" = "%PDF-" ] && tail -c 6 "$1" | grep -q '%%EOF'; }

echo "== default configuration"
PROJECT_CONFIG="" run_case default
check "run succeeds"                               test "$RC" -eq 0
check "semicolon fixed in the project"             grep -qF 'return 0;' "$DIR/proj/main.cpp"
check "type error left as it was"                  grep -qF 'int count = std::string("42");' "$DIR/proj/main.cpp"
check "report.pdf written"                         is_pdf "$DIR/out/report.pdf"
check "combined report written"                    is_pdf "$DIR/out/report.combined.pdf"

echo "-- header and summary"
check "title"                                      txt "2buggAI Fehlerbericht"
check "project"                                    txtre "^Projekt: +proj$"
check "run id in the header block"                 txtre "^Run-ID: +[0-9]{8}-[0-9]{6}-[0-9]+$"
check "date"                                       txtre "^Datum: +20[0-9]{2}-"
check "mode and models (no keys)"                  txtre "^Debugger: +gemini/mock-gemini$"
check "reviewer and verifier models"               bash -c "grep -qE '^Reviewer: +claude/mock-claude$' '$TXT' && grep -qE '^Verifier: +openai/mock-openai$' '$TXT'"
check "summary numbers: 5 / 4 / 1"                 txt "Gefunden: 5   Behoben: 4   Nicht behoben: 1"
check "summary numbers match the JSON"             report "f['summary']=={'found':5,'fixed':4,'not_fixed':1}"
check "table has a row per error"                  test "$(grep -cE '^ [1-5] +\| main\.cpp:[0-9]+' "$TXT")" -eq 5
check "table: not fixed row"                       txtre "\| wrong_data_type +\| NICHT BEHOBEN +\| S\. +[0-9]+$"
check "table: fixed together with another fix"     txtre "\| behoben \(mit Nr\. [12]\) +\| S\. +[0-9]+$"
check "table rows link to the sections"            test "$(grep -ac '/Subtype /Link' "$DIR/out/report.pdf")" -eq 5
check "no raw JSON by default"                     notxt '"detected_errors"'
check "no appendix by default"                     notxt "Anhang: roher JSON-Report"

echo "-- every fix section"
check "five sections"                              test "$(grep -cE '^[1-5]\. [a-z_]+ in main\.cpp$' "$TXT")" -eq 5
for heading in "FEHLERMELDUNG" "FEHLERHAFTER CODE" "WARUM DAS EIN FEHLER IST" "GEFIXTER CODE" "DIFF" \
               "ERKLÄRUNG DER BEHEBUNG" "ERGEBNIS"; do
    check "heading: $heading"                      txt "$heading"
done
check "metadata: file, lines, language, tool"      bash -c "grep -qE '^Datei: +main.cpp$' '$TXT' && grep -qE '^Zeile\(n\): +[0-9]+' '$TXT' && grep -qE '^Sprache: +cpp$' '$TXT' && grep -qE '^Tool/Quelle: +Compiler \(g\+\+\)$' '$TXT'"
check "metadata: severity and category"            bash -c "grep -qE '^Schweregrad: +error \(Schwierigkeit: ' '$TXT' && grep -qE '^Kategorie: +syntax_error \(Checkliste\)' '$TXT'"
check "error message shortened, short paths"       bash -c "grep -qE \"^  main.cpp:[0-9]+:[0-9]+: error: expected ';' before '}' token\" '$TXT' && ! grep -qF '$DIR/proj/main.cpp:' '$TXT'"

echo "-- semicolon: faulty line red, fixed line green"
SEMI=$(python3 -I -c "import json; f=json.load(open('$DIR/out/report.json'))['fix_report']['fixes']; print([x['id'] for x in f if x['error_name']=='syntax_error'][0])")
check "faulty line marked (>)"                     in_section "$SEMI" ">     return 0"
check "fixed line marked (+)"                      in_section "$SEMI" "+     return 0;"
check "root cause explanation"                     in_section "$SEMI" "Nach der return-Anweisung fehlt das Semikolon."
check "fix explanation"                            in_section "$SEMI" "Das fehlende Semikolon wurde hinter 'return 0' ergänzt."
check "side effects"                               in_section "$SEMI" "Mögliche Nebenwirkungen: Keine."
check "reviewer approved with comment"             in_section "$SEMI" "angenommen - Minimaler Fix an der eigentlichen Ursache."
check "verifier confirmed"                         in_section "$SEMI" "bestätigt - Diff behebt die Ursache."
check "re-run result"                              in_section "$SEMI" "Fehler behoben"
check "unified diff: - line"                       in_section "$SEMI" "-    return 0"
check "unified diff: + line"                       in_section "$SEMI" "+    return 0;"
check "red / green backgrounds in the PDF"         bash -c "grep -aqF '1.00 0.87 0.87 rg' '$DIR/out/report.pdf' && grep -aqF '0.86 0.96 0.86 rg' '$DIR/out/report.pdf'"

echo "-- undeclared variable: inserted line shifts the numbers"
UNDECL=$(python3 -I -c "import json; f=json.load(open('$DIR/out/report.json'))['fix_report']['fixes']; print([x['id'] for x in f if x['error_name']=='undeclared_variable'][0])")
check "old: line 8 is the faulty line"             in_section "$UNDECL" "    8 >     int z = y + a;"
check "new: line 8 is the inserted line"           in_section "$UNDECL" "    8 +     int y = 1;"
check "new: old line 8 is now line 9"              in_section "$UNDECL" "    9 |     int z = y + a;"
check "context lines before (3)"                   in_section "$UNDECL" "    5 | const char* api_key"
check "missing fix_explanation is reported"        in_section "$UNDECL" "Keine Erklärung vom Modell geliefert."
check "run went on after the missing field"        report "fixes['undeclared_variable']['status']=='fixed'"

echo "-- not fixed: marked, last proposal and reason"
check "status bar NICHT BEHOBEN"                   txt "NICHT BEHOBEN - Iterationen: 5"
check "reason: Verifier rejected"                  txt "Letzter Grund: Verifier abgelehnt: Ein Cast von std::string"
check "last proposal shown, not applied"           txt "LETZTER VORSCHLAG (nicht übernommen)"
check "proposal line marked"                       txt '+     int count = (int)std::string("42");'
check "reviewer rejected after revision"           txt "abgelehnt (Änderungen gefordert) nach einer Überarbeitung"
check "verifier remaining issue"                   txt "- std::stoi statt Cast verwenden"
check "re-run not executed"                        txt "nicht ausgeführt (Fix nicht angewandt)"
check "attempt list with 5 attempts"               test "$(grep -cE '^  #[1-5]  abgelehnt, nicht angewandt' "$TXT")" -eq 5
check "affected_lines given as string parsed"      report "fixes['wrong_data_type']['attempts'][0]['explanation']['affected_lines']==[13]"
check "side_effects given as list joined"          report "fixes['wrong_data_type']['attempts'][0]['explanation']['side_effects']=='Der Cast verdeckt den eigentlichen Typfehler.'"

echo "-- fixed together with another fix"
check "reason names the section"                   txtre "Mit dem Fix in Abschnitt [12] behoben"
check "resolved_by in the JSON"                    report "fixes['missing_semicolon_cpp']['resolved_by']==fixes['syntax_error']['id'] and fixes['missing_include_cpp']['resolved_by']==fixes['undeclared_variable']['id']"

echo "-- final analysis, order, secrets"
check "final analysis after the sections"          order "1. " "Gesamtanalyse des Modells"
check "summary before the sections"                order "Zusammenfassung" "1. "
check "secret from the code masked in the PDF"     bash -c "! grep -aqF '$SECRET_IN_CODE' '$DIR/out/report.pdf' '$DIR/out/report.combined.pdf'"
check "masked value visible as ***"                txt 'const char* api_key = "***";'
check "secret masked in fix_report (JSON)"         report "'$SECRET_IN_CODE' not in json.dumps(f)"
check "no API keys in PDFs or JSON"                bash -c "! grep -aqE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$DIR'/out/*.pdf '$DIR/out/report.json'"
check "page numbers"                               txtre "^Page 1 of [0-9]+$"

if [ -n "${REPORT_SAMPLE_OUT:-}" ] && [ "$FAILS" -eq 0 ]; then
    mkdir -p "$(dirname "$REPORT_SAMPLE_OUT")"
    cp "$DIR/out/report.pdf" "$REPORT_SAMPLE_OUT" && echo "  sample report: $REPORT_SAMPLE_OUT"
fi

echo "== report config: appendix, no diff, context 1, English"
PROJECT_CONFIG='{"report": {"include_raw_json_appendix": true, "show_diff": false, "context_lines": 1, "language": "en"}}' run_case config
check "run succeeds"                               test "$RC" -eq 0
check "English labels"                             bash -c "grep -qF '2buggAI error report' '$TXT' && grep -qF 'FAULTY CODE' '$TXT' && grep -qF 'WHY THIS IS AN ERROR' '$TXT'"
check "English summary"                            txt "Found: 5   Fixed: 4   Not fixed: 1"
check "explanation language sent to the Debugger"  reqs "any(r['role']=='debugger' and r.get('explanation_language')=='English' for r in rows if r['dir']=='config')"
check "default run asked for German"               reqs "all(r.get('explanation_language')=='German' for r in rows if r['role']=='debugger' and r['dir']=='default')"
check "appendix with the raw JSON"                 txt "Appendix: raw JSON report"
check "appendix has the JSON keys"                 txt '"detected_errors"'
check "appendix after the sections"                order "1. " "Appendix: raw JSON report"
check "no diff section"                            bash -c "! grep -qxF 'DIFF' '$TXT'"
# (only the part before the appendix: its numbered JSON blocks have line numbers of their own)
check "context_lines=1: one line around"           bash -c "sed '/^Appendix: raw JSON report$/,\$d' '$TXT' > '$TXT.main' && grep -qF '    7 | int compute(int a) {' '$TXT.main' && ! grep -qF '    6 | ' '$TXT.main'"
check "secret also masked in the appendix"         bash -c "! grep -aqF '$SECRET_IN_CODE' '$DIR/out/report.pdf'"

echo "== --no-pdf still writes the fix sections into the JSON"
PROJECT_CONFIG="" run_case nopdf --no-pdf
check "run succeeds"                               test "$RC" -eq 0
check "no PDF"                                     test ! -e "$DIR/out/report.pdf"
check "fix_report in the JSON"                     report "f['summary']['found']==5"

echo
if [ "$FAILS" -eq 0 ]; then echo "Report test passed."; else echo "$FAILS check(s) failed. Logs: $WORK"; trap 'kill $SERVER_PID 2>/dev/null' EXIT; fi
exit $((FAILS > 0))
