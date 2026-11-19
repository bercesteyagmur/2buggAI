#!/usr/bin/env bash
# Integration test of the fix loop against fixes that only rename a crash.
#
# Runs the real binary on a small Java project (javac, no Maven); every LLM call is answered by
# tests/mock_llm_server.py (scenario "history", single-model mode, offline and deterministic):
#   Parser.parse("c") throws ArrayIndexOutOfBoundsException, called from Main.java:6
#   attempt 1: workaround in Parser.java -> NumberFormatException at the same place
#              => must NOT count as fixed, must be reverted, must be sent with the next request
#   attempt 2: root cause fixed in Main.java -> the program prints sum=9
# Afterwards the final analysis must see the state after the fixes (code and program output).
#
# Usage (from the repository root, after `make`): tests/run_fix_history_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0

[ -x "$BUGGY" ] || { echo "buggy is not built - run 'make' first"; exit 1; }
command -v javac >/dev/null || { echo "javac is needed for this test"; exit 1; }

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$PORT_FILE")"
FAKE_OPENAI="sk-fake-openai-987654"

DIR="$WORK/case"
mkdir -p "$DIR/proj" "$DIR/out"
cp "$ROOT/errorchecklist.txt" "$DIR/"
cat > "$DIR/proj/Main.java" <<'SRC'
public class Main {
    public static void main(String[] args) {
        String[] values = {"a=6", "b=3", "c"};
        int sum = 0;
        for (String v : values) {
            sum += Parser.parse(v);
        }
        System.out.println("sum=" + sum);
    }
}
SRC
cat > "$DIR/proj/Parser.java" <<'SRC'
public class Parser {
    public static int parse(String s) {
        String[] parts = s.split("=");
        return Integer.parseInt(parts[1]);
    }
}
SRC
cp "$DIR/proj/Parser.java" "$WORK/Parser.java.orig"

OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" \
    OPENAI_API_KEY=$FAKE_OPENAI OPENAI_MODEL=mock-legacy OPENAI_BASE_URL=$BASE/history/legacy \
    timeout 600 "$BUGGY" --path "$DIR/proj" --fix "fix all errors" --run-timeout 60 \
        --json-out "$DIR/out/report.json" 2>&1)"
RC=$?
printf '%s\n' "$OUT" > "$WORK/run.log"

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
has()     { grep -qF -- "$1" <<<"$OUT"; }
# reqs <python expression over rows = mock requests (debugger / report)>
reqs()    { python3 -I -c "import json,sys
rows=[json.loads(l) for l in open('$REQ_LOG')]
dbg=[r for r in rows if r['role']=='debugger']
rep=[r for r in rows if r['role']=='report']
sys.exit(0 if ($1) else 1)"; }
# report <python expression>; worked = the section the loop worked on (the one with attempts)
report()  { python3 -I -c "import json,sys
r=json.load(open('$DIR/out/report.json'))
fixes=r['fix_report']['fixes']
worked=[x for x in fixes if x.get('attempts')][0]
sys.exit(0 if ($1) else 1)"; }

echo "== workaround that only renames the crash"
check "run succeeds"                                   test "$RC" -eq 0
check "crash detected"                                 has "- array_index_out_of_bounds"
check "transformation recognised"                      has "The error only changed its form (number_format_exception), the program still crashes at the same place (Main.java:6). Reverting this change."
check "workaround reverted in Parser.java"             cmp -s "$DIR/proj/Parser.java" "$WORK/Parser.java.orig"
check "root cause fixed in Main.java"                  grep -qF 'if (!v.contains("=")) continue;' "$DIR/proj/Main.java"
check "all errors fixed at the end"                    has "All errors fixed"
check "two debugger requests"                          reqs "len(dbg)==2"

echo "-- attempt history sent with the next request"
check "first request has no history"                   reqs "not dbg[0]['history']"
check "second request has the history"                 reqs "dbg[1]['history'] and dbg[1]['reverted']"
check "history names file, change and new error"       reqs "all(s in dbg[1]['prompt'] for s in ['- Attempt 1: changed', 'Parser.java', \"Default to an empty value\", 'number_format_exception', 'NumberFormatException'])"

echo "-- report"
check "one section with attempts"                      report "len([x for x in fixes if x.get('attempts')])==1"
check "attempt 1 not counted as fixed"                 report "worked['attempts'][0]['outcome']=='still_present'"
check "reason says reverted"                           report "'zurückgenommen' in worked['attempts'][0]['reason']"
check "attempt 2 fixed the error"                      report "worked['status']=='fixed' and worked['attempts'][1]['outcome']=='fixed'"
check "other categories credited to attempt 2 only"    report "all(x['status']=='fixed' and x.get('resolved_by')==worked['id'] for x in fixes if x is not worked)"

echo "-- final analysis sees the state after the fixes"
check "final run output in the report request"         reqs "len(rep)==1 and 'sum=9' in rep[0]['prompt']"
check "fixed code in the report request"               reqs "'if (!v.contains(\\\\\"=\\\\\")) continue;' in rep[0]['prompt']"
check "old crash not in the report request"            reqs "'ArrayIndexOutOfBoundsException' not in rep[0]['prompt']"
check "final run output in the JSON report"            report "'sum=9' in json.dumps(r)"

echo
if [ "$FAILS" -eq 0 ]; then echo "Fix history test passed."; else echo "$FAILS check(s) failed. Logs: $WORK"; trap 'kill $SERVER_PID 2>/dev/null' EXIT; fi
exit $((FAILS > 0))
