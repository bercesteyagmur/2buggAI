#!/usr/bin/env bash
# Integration test of the error detection (real binary, LLM answered by tests/mock_llm_server.py):
#   exit: a Java program exits with code 3 and no known message -> unrecognized_runtime_failure is
#         worked on and fixed
#   gdb:  a C program segfaults under --gdb; a fix that only adds a comment must NOT count as
#         fixed (the program has to run under GDB again), the second fix removes the crash
#   env:  a Java program cannot reach a service -> environment problem, no fix request at all
#
# Usage (from the repository root, after `make`): tests/run_detection_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUGGY="$ROOT/buggy"
WORK="$(mktemp -d)"
PORT_FILE="$WORK/port"
REQ_LOG="$WORK/requests.jsonl"
FAILS=0

[ -x "$BUGGY" ] || { echo "buggy is not built - run 'make' first"; exit 1; }
for tool in javac gcc gdb python3; do
    command -v "$tool" >/dev/null || { echo "$tool is needed for this test"; exit 1; }
done

python3 -I "$ROOT/tests/mock_llm_server.py" "$PORT_FILE" "$REQ_LOG" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do [ -s "$PORT_FILE" ] && break; sleep 0.1; done
BASE="http://127.0.0.1:$(cat "$PORT_FILE")"

# run_case <name> <scenario> <target (relative to the case folder)> [buggy options...]
run_case() {
    local name="$1" scenario="$2" target="$3"; shift 3
    DIR="$WORK/$name"
    OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" \
        OPENAI_API_KEY=sk-fake-openai-987654 OPENAI_MODEL=mock-legacy OPENAI_BASE_URL=$BASE/$scenario/legacy \
        timeout 600 "$BUGGY" --path "$DIR/$target" --fix "fix all errors" --run-timeout 60 \
            --json-out "$DIR/out/report.json" "$@" 2>&1)"
    RC=$?
    printf '%s\n' "$OUT" > "$WORK/$name.log"
}
new_case() { mkdir -p "$WORK/$1/proj" "$WORK/$1/out"; cp "$ROOT/errorchecklist.txt" "$WORK/$1/"; }

check() {  # check <description> <condition...>
    local desc="$1"; shift
    if "$@"; then echo "  ok   $desc"; else echo "  FAIL $desc"; FAILS=$((FAILS + 1)); fi
}
has()     { grep -qF -- "$1" <<<"$OUT"; }
hasnt()   { ! grep -qF -- "$1" <<<"$OUT"; }
# debugger_requests <scenario>
debugger_requests() { python3 -I -c "import json
print(sum(1 for l in open('$REQ_LOG') if (r:=json.loads(l))['scenario']=='$1' and r['role']=='debugger'))"; }
report()  { python3 -I -c "import json,sys
r=json.load(open('$DIR/out/report.json'))
fixes=r['fix_report']['fixes']
byname={x['error_name']: x for x in fixes}
sys.exit(0 if ($1) else 1)"; }

echo "== unknown crash (exit code 3, no known message)"
new_case exit
cat > "$WORK/exit/proj/Main.java" <<'SRC'
public class Main {
    public static void main(String[] args) {
        System.out.println("widget exploded");
        System.exit(3);
    }
}
SRC
run_case exit detect_exit proj
check "run succeeds"                               test "$RC" -eq 0
check "fallback category detected"                 has "- unrecognized_runtime_failure"
check "worked on and fixed"                        has "  -> fixed."
check "exit removed from the code"                 bash -c "! grep -qF 'System.exit(3)' '$DIR/proj/Main.java'"
check "report: fixed"                              report "byname['unrecognized_runtime_failure']['status']=='fixed'"

echo "== --gdb: a fix without effect is not 'fixed'"
new_case gdb
cat > "$WORK/gdb/proj/crash.c" <<'SRC'
#include <stdio.h>
#include <stddef.h>

int main(void) {
    int *p = NULL;
    *p = 42;
    printf("%d\n", *p);
    return 0;
}
SRC
run_case gdb detect_gdb proj/crash.c --gdb
check "run succeeds"                               test "$RC" -eq 0
check "segfault detected"                          has "- segmentation_fault"
check "two fix requests"                           test "$(debugger_requests detect_gdb)" -eq 2
check "comment-only fix: still present"            report "byname['segmentation_fault']['attempts'][0]['outcome']=='still_present'"
check "real fix: fixed"                            report "byname['segmentation_fault']['attempts'][1]['outcome']=='fixed'"
check "program ran under GDB after the fixes"      bash -c "grep -c 'DEBUGGER IS RUNNING\|Program received signal\|exited normally' '$WORK/gdb.log' >/dev/null && grep -qF 'exited normally' '$WORK/gdb.log'"

echo "== service not reachable: environment, not code"
new_case env
cat > "$WORK/env/proj/Main.java" <<'SRC'
import java.net.Socket;

public class Main {
    public static void main(String[] args) throws Exception {
        try (Socket s = new Socket("127.0.0.1", 1)) {
            System.out.println("connected");
        }
    }
}
SRC
cp "$WORK/env/proj/Main.java" "$WORK/Main.java.env"
run_case env detect_env proj
check "run succeeds"                               test "$RC" -eq 0
check "environment problem reported"               has "[UMGEBUNGSPROBLEM] env_service_unreachable"
check "skipped, no fix attempt"                    has "Skipping uncaught_exception_main: caused by the environment"
check "no fix request sent"                        test "$(debugger_requests detect_env)" -eq 0
check "code unchanged"                             cmp -s "$DIR/proj/Main.java" "$WORK/Main.java.env"
check "report: reason names the environment"       report "'Umgebungsproblem (env_service_unreachable)' in byname['uncaught_exception_main'].get('reason','') + json.dumps(byname['uncaught_exception_main'])"
check "JSON: environment_problems"                 report "r['environment_problems'][0]['name']=='env_service_unreachable'"
check "final analysis was told"                    python3 -I -c "import json,sys
rows=[json.loads(l) for l in open('$REQ_LOG')]
sys.exit(0 if any(r['scenario']=='detect_env' and r['role']=='report' for r in rows) else 1)"

echo "== Python: every kind of attempt (not writable, no effect, syntax error, progress, fixed)"
new_case loop
cat > "$WORK/loop/proj/calc.py" <<'SRC'
def double(x):
    return x + x + 1


def square(x):
    return x * x + 1
SRC
cat > "$WORK/loop/proj/main.py" <<'SRC'
from calc import double, square
from readonly import VALUE

assert double(2) == 4, f"double: got {double(2)}"
assert square(3) == 9, f"square: got {square(3)}"
print("all checks passed", VALUE)
SRC
printf 'VALUE = 1\n' > "$WORK/loop/proj/readonly.py"
chmod 444 "$WORK/loop/proj/readonly.py"
: > "$WORK/loop/proj/requirements.txt"   # no dependencies, no network
run_case loop loop proj
chmod 644 "$DIR/proj/readonly.py"
outcomes() { report "[a['outcome'] for a in byname['assertion_error']['attempts']]==$1"; }
reason()   { report "'$2' in byname['assertion_error']['attempts'][$1]['reason']"; }
loopreq()  { python3 -I -c "import json,sys
rows=[json.loads(l) for l in open('$REQ_LOG')]
dbg=[r for r in rows if r['scenario']=='loop' and r['role']=='debugger']
sys.exit(0 if ($1) else 1)"; }
check "run succeeds (write error does not end it)" test "$RC" -eq 0
check "venv created in the project"                test -x "$DIR/proj/.venv/bin/python"
check "write error reported"                       has "fix could not be written"
check "no-effect fix reverted"                     has "The fix had no effect, the error output is unchanged. Reverting this change."
check "syntax error counts as a broken build"      has "The fix broke the build"
check "progress kept"                              has "progress: the error output changed, keeping this change"
check "program passes at the end"                  has "all checks passed 1"
check "calc.py has both real fixes only"           bash -c "grep -qx '    return x + x' '$DIR/proj/calc.py' && grep -qx '    return x \* x' '$DIR/proj/calc.py' && ! grep -q 'helper functions' '$DIR/proj/calc.py' && grep -qx 'def double(x):' '$DIR/proj/calc.py'"
check "read-only file unchanged"                   grep -qx 'VALUE = 1' "$DIR/proj/readonly.py"
check "report: outcome of every attempt"           outcomes "['apply_failed','still_present','still_present','still_present','fixed']"
check "report: no effect"                          reason 1 "keine Wirkung"
check "report: broken build"                       reason 2 "Build gebrochen"
check "report: progress"                           reason 3 "Fortschritt"
check "history tells the model what was reverted"  loopreq "dbg[4]['prompt'].count('this change was REVERTED')==2 and 'this change was KEPT' in dbg[4]['prompt'] and 'could not be written' in dbg[4]['prompt']"

echo "== relative project path (the fix loop used to look for proj/proj/main.cpp)"
new_case relative
printf '#include <iostream>\nint main() {\n    std::cout << "hi";\n    return 0\n}\n' > "$WORK/relative/proj/main.cpp"
DIR="$WORK/relative"
OUT="$(cd "$DIR" && env -i PATH="$PATH" HOME="$HOME" \
    OPENAI_API_KEY=sk-fake-openai-987654 OPENAI_MODEL=mock-legacy OPENAI_BASE_URL=$BASE/relative/legacy \
    timeout 600 "$BUGGY" proj "fix all errors" --no-pdf 2>&1)"
RC=$?
printf '%s\n' "$OUT" > "$WORK/relative.log"
check "run succeeds"                               test "$RC" -eq 0
check "fix applied"                                grep -qF 'return 0;' "$DIR/proj/main.cpp"
check "backups in the project folder"              test -f "$DIR/proj/backups/main.cpp.orig"
check "all errors fixed"                           has "All errors fixed"

echo
if [ "$FAILS" -eq 0 ]; then echo "Detection test passed."; else echo "$FAILS check(s) failed. Logs: $WORK"; trap 'kill $SERVER_PID 2>/dev/null' EXIT; fi
exit $((FAILS > 0))
