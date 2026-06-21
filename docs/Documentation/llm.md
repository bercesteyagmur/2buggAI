# LLM Integration: 2buggAI

**Project:** 2buggAI — AI-powered Debugging Tool  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Overview

2buggAI uses the **OpenAI API** for two distinct AI tasks:

1. **`debug_report()`** — Final analysis: explains the bug, its root cause, and recommends fixes
2. **`fix_code()`** — Auto-fix: given a specific error, returns corrected source code to be written to disk

Both calls go to the same endpoint and model, but use different prompts and response formats.

---

## 2. Configuration

All settings are loaded from environment variables at startup:

| Variable | Default | Description |
|----------|---------|-------------|
| `OPENAI_API_KEY` | *(required)* | API key — throws if not set |
| `OPENAI_BASE_URL` | `https://api.openai.com` | API base URL (overridable for proxies) |
| `OPENAI_MODEL` | `gpt-5.2` | Model to use for all calls |

The API key can also be passed via `--api-token` and the base URL via `--api-url` on the CLI.

---

## 3. HTTP Client

**Library:** libcurl  
**Endpoint:** `POST {OPENAI_BASE_URL}/v1/responses`  
**Authentication:** `Authorization: Bearer <OPENAI_API_KEY>`  
**Content-Type:** `application/json`  
**Timeout:** 60 seconds  

---

## 4. `debug_report()` — Final Analysis Call

Called once after the auto-fix loop completes. Sends the full JSON report and returns a structured Markdown analysis.

### Request

```json
{
  "model": "gpt-5.2",
  "instructions": "<system prompt>",
  "input": "Here is the report as JSON:\n\n{...report_json...}\n\nPlease analyze and explain the error, then propose a fix.",
  "max_output_tokens": 4000
}
```

### System Prompt

```
You are a Senior Debugger. Analyze the JSON (debugger output, runtime errors,
Valgrind output if available, detected errors).
Structure your response in Markdown with the following sections:

## Summary
A brief summary of the main issue (max 2 sentences).

## Severity
critical, high, medium, or low

## Bugs
For each bug:

### Bug N: [Title]
- **File:** path/to/file.ext:line
- **Category:** Choose the most specific category name
  (e.g., memory_leak, null_pointer, uninitialized_variable,
  off_by_one, integer_overflow, etc.). Use snake_case.
- **Language:** general / c / cpp / java / python.
  Use 'general' if the bug can occur in multiple languages.
  Use a specific language only if the bug is unique to that
  language's syntax or runtime.
- **Problem:** What is the problem (brief description)

**Buggy code:**
```
// Original buggy code
```

**Fixed code:**
```
// Corrected code
```

**Explanation:** Brief explanation of why the fix works.

## Recommendations (only if needed)
OMIT THIS SECTION ENTIRELY unless there is at least one concrete,
non-trivial systemic recommendation. Do NOT include generic advice
like 'write tests' or 'use a linter'.

Respond in English.
```

### Response Parsing

`extract_text_best_effort()` tries two paths in order:

1. `response["output_text"]` (string)
2. `response["output"][*]["content"][*]["text"]` (array traversal)

Returns the extracted Markdown text as `OpenAIResult::text`.

### Checklist Update

After a successful call, the tool scans the response text line by line for:
- `**Category:** <value>`
- `**Language:** <value>`

If a new category/language pair is not already in `errorchecklist.txt`, it is appended automatically.

---

## 5. `fix_code()` — Auto-Fix Call

Called inside the fix loop for each error attempt. Returns corrected source code as a JSON object.

### Request

```json
{
  "model": "gpt-5.2",
  "instructions": "<system prompt>",
  "input": [
    {
      "type": "message",
      "role": "user",
      "content": "Error name: ...\n\nLanguage: ...\n\nError output:\n...\n\nSource code:\n...\n\nChecklist:\n...\n\nPlease analyze and return ONLY the JSON object as specified."
    }
  ],
  "max_output_tokens": 4000
}
```

### System Prompt

```
You are a Senior Debugger. Analyze the error and the code.
The source code may be in C, C++, Java, or Python — use the
appropriate syntax and idioms for the language indicated by
the 'language' field and the source code itself.

Respond ONLY with a JSON object, no text before or after:
{
  "file_path": "...path to the fixed file...",
  "fixed_code": "...only the content of this one file, in the original language...",
  "new_errors": ["error1", "error2"],
  "success": true,
  "is_confident": true
}

Go through errors by order of difficulty (easy → medium → hard).
Only include errors that are actually present in the code and can
be fixed by changing the source code. Do NOT include errors that
would require changes to the build system, project structure, or
adding new files.
new_errors should only contain errors listed in the checklist.

Respond in English.
```

### Response Parsing

The response text is parsed as JSON and mapped to `FixResult`:

| Field | Type | Description |
|-------|------|-------------|
| `file_path` | string | Path of the file to overwrite |
| `fixed_code` | string | Complete corrected file content |
| `new_errors` | `vector<string>` | Other errors the AI noticed (checklist names) |
| `success` | bool | Whether the AI believes the fix is correct |
| `is_confident` | bool | Whether the AI is confident the error is fully resolved |

If JSON parsing fails at any step, an empty `FixResult` is returned (`success = false`) and the fix loop retries.

---

## 6. Data Structures

```cpp
struct OpenAIResult {
    long http_status;
    std::string raw_json;  // full API response body
    std::string text;      // extracted text content
};

struct FixRequest {
    std::string error_name;    // e.g. "null_pointer"
    std::string error_output;  // raw compiler/debugger/runtime output
    std::string language;      // "c", "cpp", "java", "python"
    std::string source_code;   // current source (re-read from disk each attempt)
    std::string checklist;     // raw errorchecklist.txt content
};

struct FixResult {
    std::string fixed_code;
    std::string file_path;
    std::vector<std::string> new_errors;
    bool success = false;
    bool is_confident = false;
};
```

---

## 7. Error Handling

| Scenario | Behavior |
|----------|----------|
| `OPENAI_API_KEY` not set | Throws `runtime_error` at startup |
| `curl_easy_init()` fails | Throws `runtime_error` |
| curl network error | Throws `runtime_error` with curl error message |
| HTTP 4xx / 5xx | `http_status` set; caller prints error and exits with code 5 |
| Invalid JSON in response | `parsed.is_discarded()` → empty result returned |
| Missing fields in fix JSON | Fields skipped; `success` stays `false` |

---

## 8. API Parameters

| Parameter | Value | Reason |
|-----------|-------|--------|
| `max_output_tokens` | 4000 | Enough for full file rewrites and detailed analysis |
| `timeout` | 60s | Prevents hanging on slow responses |
| `model` | `gpt-5.2` (default) | Configurable via `OPENAI_MODEL` |
