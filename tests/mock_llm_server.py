#!/usr/bin/env python3
"""Mock für OpenAI-kompatible /chat/completions Endpoints (nur Standardbibliothek).

URL-Schema:  http://127.0.0.1:<port>/<szenario>/<anbieter>/.../chat/completions
Die Rolle (Debugger/Reviewer/Verifier/Report) wird am System-Prompt erkannt.

Aufruf: mock_llm_server.py <port-datei> <request-log.jsonl>
Der Server bindet einen freien Port und schreibt ihn in <port-datei>.
"""
import json
import re
import sys
import threading
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT_FILE, LOG_FILE = sys.argv[1], sys.argv[2]
counters = defaultdict(int)
lock = threading.Lock()


def role_of(system_prompt):
    if "Senior Code Reviewer" in system_prompt:
        return "reviewer"
    if "final Verifier" in system_prompt:
        return "verifier"
    if "Analyze the JSON" in system_prompt:
        return "report"
    return "debugger"


def chat_response(content, finish_reason="stop"):
    return {
        "id": "mock",
        "object": "chat.completion",
        "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                     "finish_reason": finish_reason}],
        "usage": {"prompt_tokens": 100, "completion_tokens": 50, "total_tokens": 150},
    }


def debugger_fix(user_prompt, path_override=None):
    m = re.search(r"^===== FILE: (.+) =====$", user_prompt, re.M)
    path = m.group(1)
    start = m.end() + 1
    nxt = user_prompt.find("\n\nChecklist:\n", start)
    code = user_prompt[start:nxt].rstrip("\n") + "\n"
    fixed = code.replace("return 0\n", "return 0;\n")
    body = {"file_path": path_override or path, "fixed_code": fixed,
            "new_errors": [], "success": True, "is_confident": True}
    # in ```json eingepackt, um das robuste Parsing zu testen
    return "Here is the fix:\n```json\n" + json.dumps(body) + "\n```"


# ---------- Szenario "fullstack": tests/fixtures/fullstack_sample ----------
# Bekannte Fehler der Fixture und ihre Korrektur. Der Mock "repariert" die erste Datei
# aus dem Request, die einen dieser Fehler enthält.
FULLSTACK_RULES = [
    ("from app.modles import", "from app.models import"),
    ('.replace(" ", "_")', '.replace(" ", "-")'),
    ("${user.name}", "${user.full_name}"),
    ("CREATE TABEL", "CREATE TABLE"),
]


def request_files(user_prompt):
    """[(pfad, inhalt)] aller '===== FILE: <pfad> =====' Blöcke vor der Checkliste."""
    source = user_prompt[:user_prompt.find("\n\nChecklist:\n")]
    parts = re.split(r"^===== FILE: (.+) =====\n", source, flags=re.M)
    return [(parts[i], parts[i + 1].rstrip("\n") + "\n") for i in range(1, len(parts) - 1, 2)]


def fullstack_fix(user_prompt):
    for path, code in request_files(user_prompt):
        fixed = code
        for old, new in FULLSTACK_RULES:
            fixed = fixed.replace(old, new)
        if fixed != code:
            return json.dumps({"file_path": path, "fixed_code": fixed, "new_errors": [],
                               "success": True, "is_confident": True})
    return json.dumps({"file_path": "", "fixed_code": "", "new_errors": [],
                       "success": False, "is_confident": False})


# ---------- Szenario "report": tests/run_report_test.sh ----------
# C++-Projekt mit drei Compilerfehlern. Pro Checklisten-Kategorie: (alt, neu, Erklärungsfelder).
# undeclared_variable fügt eine Zeile ein (verschiebt die Zeilennummern) und liefert absichtlich
# keine fix_explanation; wrong_data_type wird vom Reviewer beanstandet und vom Verifier
# immer abgelehnt, bleibt also unbehoben.
SEMICOLON = ("    return 0\n}", "    return 0;\n}", {
    "root_cause_explanation": "Nach der return-Anweisung fehlt das Semikolon. In C++ muss jede "
                              "Anweisung mit einem Semikolon enden, sonst kann der Compiler das "
                              "Ende der Anweisung nicht erkennen und bricht beim folgenden '}' ab.",
    "fix_explanation": "Das fehlende Semikolon wurde hinter 'return 0' ergänzt. Damit ist die "
                       "Anweisung vollständig und main() kompiliert wieder.",
    "affected_lines": [15], "error_category": "missing_semicolon", "side_effects": "Keine."})
UNDECLARED = ("    int z = y + a;\n", "    int y = 1;\n    int z = y + a;\n", {
    "root_cause_explanation": "Die Variable y wird in compute() verwendet, ist dort aber nirgends "
                              "deklariert. Der Compiler kennt den Namen deshalb nicht.",
    "affected_lines": [8], "error_category": "undeclared_variable", "side_effects": ""})
CONVERSION = ('int count = std::string("42");', 'int count = (int)std::string("42");', {
    "root_cause_explanation": "Ein std::string wird einer int-Variablen zugewiesen. Es gibt keine "
                              "implizite Umwandlung von std::string nach int.",
    "fix_explanation": "Die Zuweisung wird explizit nach int gecastet.",
    "affected_lines": "13", "error_category": "type_mismatch",
    "side_effects": ["Der Cast verdeckt den eigentlichen Typfehler."]})
REPORT_FIXES = {
    "syntax_error": SEMICOLON, "missing_semicolon_cpp": SEMICOLON,
    "undeclared_variable": UNDECLARED, "missing_include_cpp": UNDECLARED,
    "wrong_data_type": CONVERSION,
}


# ---------- Szenario "history": tests/run_fix_history_test.sh ----------
# Java-Projekt: Parser.parse("c") wirft ArrayIndexOutOfBoundsException (Main.java:6 ruft auf).
# Versuch 1 ist ein Workaround in Parser.java, der daraus nur eine NumberFormatException an
# derselben Stelle macht (muss zurückgenommen werden); Versuch 2 behebt die Ursache in Main.java.
HISTORY_WORKAROUND = ("        return Integer.parseInt(parts[1]);\n",
                      "        String value = parts.length > 1 ? parts[1] : \"\";\n"
                      "        return Integer.parseInt(value);\n",
                      "Parser.java", "Default to an empty value when the entry has no '='.")
HISTORY_ROOT_FIX = ("        for (String v : values) {\n",
                    "        for (String v : values) {\n"
                    "            if (!v.contains(\"=\")) continue;\n",
                    "Main.java", "Skip entries without '=' before parsing them.")


def history_fix(user_prompt, n):
    old, new, name, why = HISTORY_WORKAROUND if n == 1 else HISTORY_ROOT_FIX
    for path, code in request_files(user_prompt):
        if path.endswith("/" + name) and old in code:
            return json.dumps({"file_path": path, "fixed_code": code.replace(old, new, 1), "new_errors": [],
                               "success": True, "is_confident": True, "fix_explanation": why})
    return json.dumps({"file_path": "", "fixed_code": "", "new_errors": [],
                       "success": False, "is_confident": False})


# ---------- Szenarien "detect_*": tests/run_detection_test.sh ----------
# detect_exit: Programm endet mit Exit-Code 3 ohne bekannte Meldung -> unrecognized_runtime_failure
# detect_gdb:  Segfault unter --gdb; Versuch 1 ändert nur einen Kommentar (darf nicht als behoben
#              gelten), Versuch 2 behebt den Nullzeiger
# detect_env:  Dienst nicht erreichbar -> darf gar nicht erst angefragt werden
DETECT_FIXES = {
    "detect_exit": [("Main.java", "        System.exit(3);\n", "")],
    "detect_gdb": [("crash.c", "    int *p = NULL;\n", "    /* p is set below */\n    int *p = NULL;\n"),
                   ("crash.c", "    int *p = NULL;\n", "    int v = 0;\n    int *p = &v;\n")],
    # Python project with two wrong results; one attempt of every kind, in this order:
    # not writable, no effect, syntax error, progress (next assert fails), fixed
    "loop": [("readonly.py", "VALUE = 1\n", "VALUE = 2\n"),
             ("calc.py", "def double(x):\n", "# helper functions\ndef double(x):\n"),
             ("calc.py", "def double(x):\n", "def double(x)\n"),
             ("calc.py", "    return x + x + 1\n", "    return x + x\n"),
             ("calc.py", "    return x * x + 1\n", "    return x * x\n")],
}


def replace_fix(user_prompt, name, old, new):
    for path, code in request_files(user_prompt):
        if path.endswith("/" + name) and old in code:
            return json.dumps({"file_path": path, "fixed_code": code.replace(old, new, 1), "new_errors": [],
                               "success": True, "is_confident": True})
    return json.dumps({"file_path": "", "fixed_code": "", "new_errors": [],
                       "success": False, "is_confident": False})


def error_name(user_prompt):
    m = re.search(r"^Error name: (.*)$", user_prompt, re.M)
    return m.group(1) if m else ""


def report_fix(user_prompt):
    path, code = request_files(user_prompt)[0]
    old, new, extra = REPORT_FIXES.get(error_name(user_prompt), (None, None, {}))
    if old is None or old not in code:
        return json.dumps({"file_path": path, "fixed_code": "", "new_errors": [],
                           "success": False, "is_confident": False})
    body = {"file_path": path, "fixed_code": code.replace(old, new, 1), "new_errors": [],
            "success": True, "is_confident": True}
    body.update(extra)
    return "```json\n" + json.dumps(body) + "\n```"


def answer(scenario, role, n, user_prompt):
    """Liefert (http_status, body_dict)."""
    if role == "report":
        if scenario == "report":
            return 200, chat_response("## Summary\nZwei von drei Fehlern wurden behoben.\n\n"
                                      "## Severity\nmedium\n\n## Bugs\n### Bug 1: Typfehler\n"
                                      "- **File:** main.cpp:13\n```\nint count = std::stoi(\"42\");\n```\n")
        return 200, chat_response("## Summary\nMissing semicolon.\n\n## Severity\nlow\n")

    if role == "debugger":
        if scenario == "badjson" and n == 1:
            return 200, chat_response("Sorry, I cannot produce JSON right now.")
        if scenario == "length" and n == 1:
            return 200, chat_response("", finish_reason="length")
        if scenario == "retry429" and n == 1:
            return 429, {"error": {"message": "Rate limit exceeded", "type": "rate_limit"}}
        if scenario == "fullstack":
            return 200, chat_response(fullstack_fix(user_prompt))
        if scenario == "report":
            return 200, chat_response(report_fix(user_prompt))
        if scenario == "history":
            return 200, chat_response(history_fix(user_prompt, n))
        if scenario in DETECT_FIXES:
            fixes = DETECT_FIXES[scenario]
            return 200, chat_response(replace_fix(user_prompt, *fixes[min(n, len(fixes)) - 1]))
        if scenario == "badpath":
            return 200, chat_response(debugger_fix(user_prompt, "../../etc/passwd"))
        return 200, chat_response(debugger_fix(user_prompt))

    if role == "reviewer":
        if scenario == "report":
            if error_name(user_prompt) == "wrong_data_type":
                return 200, chat_response(json.dumps({
                    "verdict": "CHANGES_REQUESTED",
                    "findings": [{"file": "main.cpp", "problem": "C-Cast von std::string nach int ist ungültig",
                                  "severity": "high"}],
                    "summary": "Der Cast behebt den Typfehler nicht.",
                    "approved": False, "review_comment": "Der Cast verdeckt den Typfehler nur."}))
            return 200, chat_response(json.dumps({
                "verdict": "APPROVE", "findings": [], "summary": "Root cause fixed.",
                "approved": True, "review_comment": "Minimaler Fix an der eigentlichen Ursache."}))
        if scenario == "credit":
            return 400, {"type": "error", "error": {"type": "invalid_request_error",
                         "message": "Your credit balance is too low to access the API "
                                    "(key sk-fake-claude-123456)"}}
        if scenario == "changes" and n == 1:
            return 200, chat_response(json.dumps({
                "verdict": "CHANGES_REQUESTED",
                "findings": [
                    {"file": "main.c", "problem": "Fix touches unrelated code", "severity": "medium"},
                    {"file": "main.c", "problem": "Missing explanation", "severity": "low"}],
                "summary": "Please keep the change minimal."}))
        return 200, chat_response(json.dumps({"verdict": "APPROVE", "findings": [],
                                              "summary": "Root cause fixed."}))

    if role == "verifier":
        if scenario == "report" and error_name(user_prompt) == "wrong_data_type":
            return 200, chat_response(json.dumps({
                "confirmed": False,
                "reason": "Ein Cast von std::string nach int kompiliert nicht; der Fehler bleibt.",
                "remaining_issues": ["std::stoi statt Cast verwenden"]}))
        if scenario == "report":
            return 200, chat_response(json.dumps({"confirmed": True, "reason": "Diff behebt die Ursache.",
                                                  "remaining_issues": []}))
        if scenario == "reject":
            return 200, chat_response(json.dumps({
                "confirmed": False, "reason": "Diff does not address the root cause",
                "remaining_issues": ["semicolon still missing elsewhere"]}))
        return 200, chat_response(json.dumps({"confirmed": True, "reason": "ok",
                                              "remaining_issues": []}))
    return 500, {"error": {"message": "unknown role"}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(length) or b"{}")
        parts = self.path.strip("/").split("/")
        scenario, provider = parts[0], parts[1]
        msgs = req.get("messages", [])
        system_prompt = next((m["content"] for m in msgs if m.get("role") == "system"), "")
        user_prompt = next((m["content"] for m in msgs if m.get("role") == "user"), "")
        role = role_of(system_prompt)

        with lock:
            counters[(scenario, role)] += 1
            n = counters[(scenario, role)]
            with open(LOG_FILE, "a") as log:
                log.write(json.dumps({
                    "scenario": scenario, "provider": provider, "role": role, "n": n,
                    "path": self.path, "auth": self.headers.get("Authorization", ""),
                    "keys": sorted(req.keys()), "model": req.get("model"),
                    "max_tokens": req.get("max_tokens"),
                    "max_completion_tokens": req.get("max_completion_tokens"),
                    "revision": "REVIEW OF YOUR PREVIOUS FIX" in user_prompt,
                    "error": (re.search(r"^Error name: (.*)$", user_prompt, re.M) or [None, None])[1],
                    "files": [p for p, _ in request_files(user_prompt)],
                    "cross": "===== CROSS-COMPONENT CONTEXT =====" in user_prompt,
                    "prompt_chars": len(user_prompt),
                    "first_file": next((p for p, _ in request_files(user_prompt)), None)
                                  if "\n\nChecklist:\n" in user_prompt else None,
                    "explanation_language": (re.search(
                        r"Write root_cause_explanation, fix_explanation and side_effects in (\w+)",
                        user_prompt) or [None, None])[1],
                    "history": "Previous attempts for this error that did NOT solve it" in user_prompt,
                    "reverted": "this change was REVERTED" in user_prompt,
                    # scenario "history" checks what the requests contain in full
                    "prompt": user_prompt if scenario in ("history", "loop") else None,
                }) + "\n")

        status, body = answer(scenario, role, n, user_prompt)
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(PORT_FILE, "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
