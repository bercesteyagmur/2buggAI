#!/usr/bin/env bash
# Integration test for fullstack projects: runs the real binary against a copy of
# tests/fixtures/fullstack_sample with all LLM calls answered by tests/mock_llm_server.py
# (scenario "fullstack"), so no API key and no LLM network access is needed.
#
# What needs which tool:
#   db/        python3 only (always tested)
#   backend/   python3 + venv; flask and pytest come from PyPI on the first run
#              (skipped with a note when they cannot be installed)
#   frontend/  node + npm for the real tsc/vitest run. Without node, tsc is replaced by
#              tests/helpers/tsc_stand_in.py through a .buggai.json command override, so
#              the parser, the checklist and the cross-component context are still tested;
#              the frontend test run (vitest) is skipped.
#
# Optional live pass against a real provider (uses your LLM environment variables):
#   FULLSTACK_LIVE_TEST=1 tests/run_fullstack_test.sh
#
# Usage (from the repository root, after `make`):  tests/run_fullstack_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
FIXTURE="$ROOT/tests/fixtures/fullstack_sample"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0
SKIPS=0

[ -x "$BUGGY" ] || { echo "buggy is not built - run 'make' first"; exit 1; }

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$PORT_FILE")"

FAKE_GEMINI="fake-gemini-key-AAAA"
FAKE_CLAUDE="sk-fake-claude-123456"
FAKE_OPENAI="sk-fake-openai-987654"
S=fullstack

DIR="$WORK/case"
PROJ="$DIR/proj"
mkdir -p "$DIR"
cp -r "$FIXTURE" "$PROJ"
cp "$ROOT/errorchecklist.txt" "$DIR/"     # the tool reads and extends the checklist in its cwd

# Same rule as the tool: a Windows node under /mnt/ (WSL) does not count.
NODE="$(command -v node 2>/dev/null || true)"
case "$NODE" in ""|/mnt/*) TSC_MODE="stand-in" ;; *) TSC_MODE="real" ;; esac
if [ "$TSC_MODE" = "stand-in" ]; then
    printf '{ "commands": { "frontend": { "tsc": "python3 -I %s/tests/helpers/tsc_stand_in.py" } } }\n' "$ROOT" \
        > "$PROJ/.buggai.json"
fi

OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" \
    BUGGAI_DEBUGGER=gemini BUGGAI_REVIEWER=claude BUGGAI_VERIFIER=openai \
    GEMINI_API_KEY=$FAKE_GEMINI GEMINI_MODEL=mock-gemini \
    CLAUDE_API_KEY=$FAKE_CLAUDE CLAUDE_MODEL=mock-claude \
    OPENAI_API_KEY=$FAKE_OPENAI OPENAI_MODEL=mock-openai \
    BUGGAI_GEMINI_ENDPOINT=$BASE/$S/gemini/v1beta/openai/chat/completions \
    BUGGAI_CLAUDE_ENDPOINT=$BASE/$S/claude/v1/chat/completions \
    BUGGAI_OPENAI_ENDPOINT=$BASE/$S/openai/v1/chat/completions \
    OPENAI_BASE_URL=$BASE/$S/legacy \
    timeout 1800 "$BUGGY" --path "$PROJ" --fix "fix all errors" --json-out "$DIR/report.json" 2>&1)"
RC=$?
printf '%s\n' "$OUT" > "$WORK/run.log"

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
skip()    { echo "  skip $1"; SKIPS=$((SKIPS + 1)); }
has()     { grep -qF -- "$1" <<<"$OUT"; }
hasnt()   { ! grep -qF -- "$1" <<<"$OUT"; }
infile()  { grep -qF -- "$2" "$PROJ/$1"; }
# report <python expression over r (report.json)>
report()  { python3 -I -c "import json,sys
r=json.load(open('$DIR/report.json'))
steps={s['name']: s['exit_code'] for s in r['steps']}
sys.exit(0 if ($1) else 1)"; }
# reqs <python expression over rows (requests of this scenario)>
reqs()    { python3 -I -c "import json,sys
rows=[json.loads(l) for l in open('$REQ_LOG')]
rows=[r for r in rows if r['scenario']=='$S']
sys.exit(0 if ($1) else 1)"; }
ran()     { report "'$1' in steps"; }   # step was executed (not skipped)

echo "== detection"
check "routed to the stack pipeline"          has "[STACK] fullstack project detected"
check "backend component (python)"            has "- backend  [python / python]  root: backend  service: backend  depends on: db"
check "frontend component (typescript)"       has "- frontend  [node / typescript]  root: frontend  service: frontend  depends on: backend"
check "db component (sql)"                    has "- db  [sql / sql]  root: db"
check "compose file with two services"        has "Compose file: docker-compose.yml (2 services)"
check "stack in the JSON report"              report "[c['name'] for c in r['stack']['components']] == ['backend', 'frontend', 'db']"

echo "== db: SQL migration error (python3 only)"
check "dry run executed"                      ran "db: sql"
check "classified: sql_migration_failed"      has "- sql_migration_failed"
check "located: db/002_orders.sql:2"          has "db/002_orders.sql:2  [db / sql]"
check "Debugger received the migration"       reqs "any(r['role']=='debugger' and 'db/002_orders.sql' in r['files'] for r in rows)"
check "fix applied"                           infile db/002_orders.sql "CREATE TABLE orders"
check "dry run passes after the fix"          report "steps.get('db: sql') == 0"
check "original kept (backups/*.orig)"        grep -qF "CREATE TABEL" "$PROJ/backups/db/002_orders.sql.orig"
check "diff kept (backups/*.1.diff)"          grep -qF -- "-CREATE TABEL orders (" "$PROJ/backups/db/002_orders.sql.1.diff"

echo "== backend: import error + failing test (needs pytest)"
if ran "backend: pytest"; then
    check "classified: import_error"          has "- import_error"
    check "classified: assertion_error"       has "- assertion_error"
    check "located: backend/app/main.py:3"    has "backend/app/main.py:3  [backend / pytest]"
    check "import fixed"                      infile backend/app/main.py "from app.models import USERS"
    check "slugify fixed"                     infile backend/app/utils.py '.replace(" ", "-")'
    check "pytest passes after the fixes"     report "steps.get('backend: pytest') == 0"
    check "Verifier re-ran dependents"        has "[VERIFY] backend/app/main.py changed -> re-running: backend frontend"
else
    skip "backend tests: pytest not available (see 'Skipped steps' in $WORK/run.log)"
fi

echo "== frontend: type error from the API field rename (tsc: $TSC_MODE)"
if ran "frontend: tsc"; then
    check "classified: ts_property_missing"   has "- ts_property_missing"
    check "located: frontend/src/format.ts:4" has "frontend/src/format.ts:4  [frontend / tsc]"
    check "type error fixed"                  infile frontend/src/format.ts '${user.full_name}'
    check "tsc passes after the fix"          report "steps.get('frontend: tsc') == 0"
    check "Debugger got the backend model"    reqs "any(r['role']=='debugger' and r['cross'] and 'frontend/src/format.ts' in r['files'] and 'backend/app/models.py' in r['files'] for r in rows)"
    check "Reviewer got the cross context"    reqs "any(r['role']=='reviewer' and r['cross'] for r in rows)"
    check "Verifier got the cross context"    reqs "any(r['role']=='verifier' and r['cross'] for r in rows)"
    if ran "frontend: vitest"; then
        check "frontend test passes"          report "steps.get('frontend: vitest') == 0"
    else
        check "skipped vitest is reported"    report "any(s.startswith('frontend: vitest') for s in r['stack']['skipped_steps'])"
        skip "frontend test run (vitest): node is not installed"
    fi
else
    check "frontend: tsc step ran"            false
fi

echo "== agent pipeline"
check "Debugger called"                       has "[Debugger gemini/mock-gemini] Fix für"
check "Reviewer approved"                     has "[Reviewer claude/mock-claude] APPROVE"
check "Verifier confirmed"                    has "[Verifier openai/mock-openai] bestätigt"
check "all three roles in the request log"    reqs "{'debugger','reviewer','verifier'} <= {r['role'] for r in rows}"
check "no request contains an .env file"      reqs "not any('.env' in f for r in rows for f in r['files'])"
check "request size stays bounded"            reqs "all(r['prompt_chars'] < 200000 for r in rows)"
check "resolved errors in the report"         report "len(r['fixed_errors']) >= 1"
if ran "backend: pytest" && ran "frontend: tsc"; then
    check "everything fixed"                  has "All errors fixed."
    check "no error left in the report"       report "r['detected_errors'] == []"
fi
check "fixture itself untouched"              grep -qF "CREATE TABEL" "$FIXTURE/db/002_orders.sql"
check "exit code 0"                           test "$RC" -eq 0
check "no keys in the output"                 bash -c "! grep -qE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$WORK/run.log' '$DIR/report.json'"

echo "== report with fix sections (locations from the tool parsers)"
# fixes = {error name: section} of the fix report in the JSON
fixrep()  { python3 -I -c "import json,sys
r=json.load(open('$DIR/report.json'))
f=r.get('fix_report', {}); fixes={x['error_name']: x for x in f.get('fixes', [])}
try: ok = bool($1)
except (KeyError, IndexError, TypeError): ok = False
sys.exit(0 if ok else 1)"; }
check "fix_report in the JSON"                fixrep "f['schema'] == 1 and len(f['fixes']) >= 1"
check "summary matches the sections"          fixrep "f['summary']['found'] == len(f['fixes']) and f['summary']['fixed'] + f['summary']['not_fixed'] == f['summary']['found']"
check "migration: file, line and tool"        fixrep "fixes['sql_migration_failed']['file'] == 'db/002_orders.sql' and 2 in fixes['sql_migration_failed']['lines'] and fixes['sql_migration_failed']['tool'].startswith('sql')"
# the section that holds the fix of the migration (g++-style: two categories can match the
# same message; the one picked first gets the attempt, the other one is "fixed with it")
MIG="[x for x in f['fixes'] if x['file'] == 'db/002_orders.sql' and x['attempts']][0]"
check "migration: fixed in one iteration"     fixrep "$MIG['status'] == 'fixed' and $MIG['iterations'] == 1"
check "migration: line 2 marked before/after" fixrep "any(l['line'] == 2 and l['mark'] and 'TABEL' in l['text'] for h in $MIG['attempts'][0]['snippet']['hunks'] for l in h['old']) and any(l['line'] == 2 and l['mark'] and 'CREATE TABLE' in l['text'] for h in $MIG['attempts'][0]['snippet']['hunks'] for l in h['new'])"
check "re-run: db check passes after the fix" fixrep "'db: sql' not in $MIG['attempts'][0]['failed_checks']"
check "other category fixed with it"          fixrep "all(x['status'] == 'fixed' and x['resolved_by'] == $MIG['id'] for x in f['fixes'] if x['file'] == 'db/002_orders.sql' and not x['attempts'])"
if ran "frontend: tsc"; then
    check "type error located in format.ts"   fixrep "fixes['ts_property_missing']['file'] == 'frontend/src/format.ts' and 4 in fixes['ts_property_missing']['lines']"
fi
check "report PDF has the fix sections"       grep -aqF "(2buggAI Fehlerbericht) Tj" "$DIR/report.pdf"
check "no keys in the PDFs"                   bash -c "! grep -aqE '$FAKE_GEMINI|$FAKE_CLAUDE|$FAKE_OPENAI' '$DIR'/*.pdf"

echo "== optional live pass"
if [ "${FULLSTACK_LIVE_TEST:-}" = "1" ] && [ -n "${OPENAI_API_KEY:-}${LLM_API_KEY:-}" ]; then
    LIVE="$WORK/live"
    mkdir -p "$LIVE"
    cp -r "$FIXTURE" "$LIVE/proj"
    cp "$ROOT/errorchecklist.txt" "$LIVE/"
    (cd "$LIVE" && timeout 3600 "$BUGGY" --path "$LIVE/proj" --fix "fix all errors" --json-out "$LIVE/report.json" > "$LIVE/run.log" 2>&1)
    LIVE_RC=$?
    check "live run exits with 0"             test "$LIVE_RC" -eq 0
    check "live run fixed the migration"      grep -qiE "CREATE TABLE( IF NOT EXISTS)? orders" "$LIVE/proj/db/002_orders.sql"
    [ "$LIVE_RC" -eq 0 ] || cp "$LIVE/run.log" "$ROOT/fullstack_live_test.log"
else
    skip "live test (set FULLSTACK_LIVE_TEST=1 and OPENAI_API_KEY or LLM_API_KEY to run it)"
fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "Fullstack test passed ($SKIPS part(s) skipped)."
else
    echo "$FAILS check(s) failed. Log: $WORK/run.log"
    trap 'kill $SERVER_PID 2>/dev/null' EXIT
fi
exit $((FAILS > 0))
