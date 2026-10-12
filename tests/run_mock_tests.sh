#!/usr/bin/env bash
# Testet die Multi-Agent-Pipeline gegen einen lokalen Mock-Server.
# Es werden nur Fake-Keys verwendet; die Umgebung wird mit env -i geleert,
# damit keine echten Keys aus ~/.bashrc in die Tests gelangen.
#
# Aufruf (aus dem Repo-Root, nach `make`):  tests/run_mock_tests.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0

[ -x "$BUGGY" ] || { echo "buggy nicht gebaut – erst 'make' ausführen"; exit 1; }

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
PORT="$(cat "$PORT_FILE")"
BASE="http://127.0.0.1:$PORT"

FAKE_GEMINI="fake-gemini-key-AAAA"
FAKE_CLAUDE="sk-fake-claude-123456"
FAKE_OPENAI="sk-fake-openai-987654"

# Kleines C-Programm mit fehlendem Semikolon
make_project() {
    local dir="$1"
    mkdir -p "$dir/proj"
    cp "$ROOT/errorchecklist.txt" "$dir/"
    cat > "$dir/proj/main.c" <<'EOF'
#include <stdio.h>

int main(void) {
    int x = 5;
    printf("%d\n", x);
    return 0
}
EOF
}

pipeline_env() {
    local s="$1"
    echo BUGGAI_DEBUGGER=gemini BUGGAI_REVIEWER=claude BUGGAI_VERIFIER=openai \
         GEMINI_API_KEY=$FAKE_GEMINI GEMINI_MODEL=mock-gemini \
         CLAUDE_API_KEY=$FAKE_CLAUDE CLAUDE_MODEL=mock-claude \
         OPENAI_API_KEY=$FAKE_OPENAI OPENAI_MODEL=mock-openai \
         BUGGAI_GEMINI_ENDPOINT=$BASE/$s/gemini/v1beta/openai/chat/completions \
         BUGGAI_CLAUDE_ENDPOINT=$BASE/$s/claude/v1/chat/completions \
         BUGGAI_OPENAI_ENDPOINT=$BASE/$s/openai/v1/chat/completions \
         OPENAI_BASE_URL=$BASE/$s/legacy
}

# run_case <name> <env...>  -> setzt OUT, RC, DIR
run_case() {
    local name="$1"; shift
    DIR="$WORK/$name"
    make_project "$DIR"
    OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" "$@" \
           timeout 300 "$BUGGY" --path "$DIR/proj" --fix "missing semicolon" 2>&1)"
    RC=$?
    printf '%s\n' "$OUT" > "$WORK/$name.log"
}

check() {  # check <name> <beschreibung> <bedingung...>
    local name="$1" desc="$2"; shift 2
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
has()     { grep -qF -- "$1" <<<"$OUT"; }
hasnt()   { ! grep -qF -- "$1" <<<"$OUT"; }
fixed()   { grep -qF 'return 0;' "$DIR/proj/main.c"; }
unfixed() { grep -qxF '    return 0' "$DIR/proj/main.c"; }
reqs()    { python3 -I -c "import json,sys
rows=[json.loads(l) for l in open('$REQ_LOG')]
rows=[r for r in rows if r['scenario']=='$1']
sys.exit(0 if ($2) else 1)"; }

echo "== normal: Debugger -> Reviewer APPROVE -> Verifier bestätigt"
run_case normal $(pipeline_env normal)
check normal "Debugger-Log"                has "[Debugger gemini/mock-gemini] Fix für $DIR/proj/main.c"
check normal "Reviewer APPROVE"            has "[Reviewer claude/mock-claude] APPROVE"
check normal "Verifier bestätigt"          has "[Verifier openai/mock-openai] bestätigt"
check normal "Fehler behoben (redetect)"   has "-> fixed"
check normal "Datei korrigiert"            fixed
check normal "Pipeline-Konfig beim Start"  has "[PIPELINE] Debugger=gemini/mock-gemini Reviewer=claude/mock-claude Verifier=openai/mock-openai"
check normal "Nutzungsstatistik"           has "Debugger gemini/mock-gemini: 1 Aufruf(e), Tokens: 150"
check normal "kein temperature/top_p"      reqs normal "all('temperature' not in r['keys'] and 'top_p' not in r['keys'] for r in rows)"
check normal "OpenAI: max_completion_tokens" reqs normal "all(r['max_completion_tokens']==8000 and r['max_tokens'] is None for r in rows if r['provider']=='openai')"
check normal "Gemini: max_tokens 16000"    reqs normal "all(r['max_tokens']==16000 for r in rows if r['provider']=='gemini')"
check normal "Claude: max_tokens 8000"     reqs normal "all(r['max_tokens']==8000 for r in rows if r['provider']=='claude')"
check normal "Bearer-Key je Anbieter"      reqs normal "all(r['auth']=='Bearer '+{'gemini':'$FAKE_GEMINI','claude':'$FAKE_CLAUDE','openai':'$FAKE_OPENAI','legacy':'$FAKE_OPENAI'}[r['provider']] for r in rows)"
check normal "Report über Legacy-Endpoint" reqs normal "any(r['role']=='report' and r['path']=='/normal/legacy/v1/chat/completions' for r in rows)"

echo "== changes: Reviewer fordert Änderungen, Debugger überarbeitet einmal"
run_case changes $(pipeline_env changes)
check changes "CHANGES_REQUESTED (2 Findings)" has "[Reviewer claude/mock-claude] CHANGES_REQUESTED (2 Findings)"
check changes "Findings gelistet"          has "[medium] main.c: Fix touches unrelated code"
check changes "Überarbeitung"              has "[Debugger] Überarbeitung"
check changes "zweites Review APPROVE"     has "[Reviewer] APPROVE"
check changes "Verifier bestätigt"         has "bestätigt"
check changes "Datei korrigiert"           fixed
check changes "Revision enthält Findings"  reqs changes "sum(r['revision'] for r in rows if r['role']=='debugger')==1"
check changes "genau 2 Debugger-Aufrufe"   reqs changes "sum(r['role']=='debugger' for r in rows)==2"

echo "== reject: Verifier lehnt ab"
run_case reject $(pipeline_env reject)
check reject "Verifier abgelehnt"          has "[Verifier openai/mock-openai] abgelehnt: Diff does not address the root cause"
check reject "Remaining issues gelistet"   has "- semicolon still missing elsewhere"
check reject "Fix-Loop gibt auf"           has "Could not fix error"
check reject "Datei unverändert"           unfixed
check reject "kein Absturz"                test "$RC" -eq 0

echo "== badjson: ungültiges JSON vom Debugger, nächster Versuch klappt"
run_case badjson $(pipeline_env badjson)
check badjson "Ungültiges JSON geloggt"    has "[Debugger gemini/mock-gemini] Ungültiges JSON in der Antwort"
check badjson "Attempt 2 läuft"            has "Attempt 2/5"
check badjson "Datei korrigiert"           fixed

echo "== length: finish_reason=length"
run_case length $(pipeline_env length)
check length "Abschneiden geloggt"         has "Antwort abgeschnitten – Token-Limit (max_tokens=16000)"
check length "Datei korrigiert"            fixed

echo "== retry429: HTTP 429 mit Retry"
run_case retry429 $(pipeline_env retry429)
check retry429 "429 geloggt"               has "HTTP 429: Rate limit exceeded – neuer Versuch in 1s (2/4)"
check retry429 "im selben Attempt behoben" hasnt "Attempt 2/5"
check retry429 "Datei korrigiert"          fixed

echo "== badpath: Debugger liefert unbekannten Pfad"
run_case badpath $(pipeline_env badpath)
check badpath "Pfad verworfen"             has "Verworfen: Pfad '../../etc/passwd' war nicht Teil des Requests"
check badpath "kein Reviewer-Aufruf"       reqs badpath "not any(r['role']=='reviewer' for r in rows)"
check badpath "Datei unverändert"          unfixed
check badpath "kein Absturz"               test "$RC" -eq 0

echo "== credit: HTTP 400 vom Reviewer (Claude-Fehlertext)"
run_case credit $(pipeline_env credit)
check credit "Fehlertext sichtbar"         has "HTTP 400: Your credit balance is too low"
check credit "Key im Fehlertext maskiert"  has "(key ***)"
check credit "kein Absturz"                test "$RC" -eq 0

echo "== legacy: keine BUGGAI_*-Variable -> Einzelmodell wie bisher"
run_case legacy OPENAI_API_KEY=$FAKE_OPENAI OPENAI_MODEL=mock-legacy OPENAI_BASE_URL=$BASE/legacy/legacy
check legacy "keine Pipeline"              hasnt "[PIPELINE]"
check legacy "kein Reviewer/Verifier"      reqs legacy "all(r['role'] in ('debugger','report') for r in rows)"
check legacy "Chat-Completions-Pfad"       reqs legacy "all(r['path']=='/legacy/legacy/v1/chat/completions' for r in rows)"
check legacy "Datei korrigiert"            fixed

echo "== off: BUGGAI_PIPELINE=off -> Einzelmodell mit LLM_*"
run_case off BUGGAI_PIPELINE=off LLM_API_KEY=$FAKE_OPENAI LLM_MODEL=mock-llm LLM_ENDPOINT=$BASE/off/llm/v1beta/openai
check off "keine Pipeline"                 hasnt "[PIPELINE]"
check off "LLM_ENDPOINT + /chat/completions" reqs off "rows and all(r['path']=='/off/llm/v1beta/openai/chat/completions' and r['model']=='mock-llm' for r in rows)"
check off "Gemini-Stil: max_tokens"        reqs off "all(r['max_tokens']==16000 for r in rows)"
check off "Datei korrigiert"               fixed

echo "== missing: fehlende Variable beim Start"
run_case missing BUGGAI_DEBUGGER=gemini GEMINI_API_KEY=$FAKE_GEMINI CLAUDE_API_KEY=$FAKE_CLAUDE OPENAI_API_KEY=$FAKE_OPENAI
check missing "GEMINI_MODEL genannt"       has "GEMINI_MODEL ist nicht gesetzt (benötigt für Debugger = gemini)"
check missing "Abbruch vor dem Build"      hasnt "ANALYZING FOLDER"
check missing "Exit-Code 2"                test "$RC" -eq 2

echo "== Keys in keinem Log"
check all "keine Fake-Keys in Ausgaben" bash -c "! grep -rqE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$WORK'/*.log"

echo
if [ "$FAILS" -eq 0 ]; then echo "Alle Tests bestanden."; else echo "$FAILS Test(s) fehlgeschlagen. Logs: $WORK"; trap 'kill $SERVER_PID 2>/dev/null' EXIT; fi
exit $((FAILS > 0))
