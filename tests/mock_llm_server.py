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


def answer(scenario, role, n, user_prompt):
    """Liefert (http_status, body_dict)."""
    if role == "report":
        return 200, chat_response("## Summary\nMissing semicolon.\n\n## Severity\nlow\n")

    if role == "debugger":
        if scenario == "badjson" and n == 1:
            return 200, chat_response("Sorry, I cannot produce JSON right now.")
        if scenario == "length" and n == 1:
            return 200, chat_response("", finish_reason="length")
        if scenario == "retry429" and n == 1:
            return 429, {"error": {"message": "Rate limit exceeded", "type": "rate_limit"}}
        if scenario == "badpath":
            return 200, chat_response(debugger_fix(user_prompt, "../../etc/passwd"))
        return 200, chat_response(debugger_fix(user_prompt))

    if role == "reviewer":
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
