# Pipeline: 2buggAI Debugging Tool

**Project:** 2buggAI — AI-powered Debugging Tool  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Pipeline Overview

```
User Input (CLI)
        │
        ▼
Argument Parsing & Validation
        │
        ▼
File Collection & Source Reading
        │
        ▼
Language Detection
        │
        ▼
Compilation / Environment Setup
        │
        ▼
Interactive Detection
        │
        ▼
Debugger Execution (GDB / JDB / PDB)   ←── optional (-g flag)
        │
Valgrind Execution                      ←── optional (--valgrind flag)
        │
Normal Program Run                      ←── default (no debug flag)
        │
        ▼
Error Output Collection
        │
        ▼
Platform Header Check
        │
        ▼
Checklist Matching & Error Sorting
        │
        ▼
Auto-Fix Loop (AI + CodeChanger + Rebuild)
        │
        ▼
JSON Report Assembly
        │
        ▼
OpenAI Analysis Call
        │
        ▼
Checklist Update
        │
        ▼
User Output
```

---

## 2. Stage Definitions

### Stage 1: Argument Parsing

**Input:** `argc`, `argv`  
**Processing:**
- Flag recognition (`-g`, `--valgrind`, `-r`, `-v`, `--json-out`, `--api-url`, `--api-token`)
- Target path existence check
- Passthrough argument collection (after `--`)

**Output:** Validated `ArgumentParser` configuration object  
**Error handling:** Exception on missing or invalid arguments

---

### Stage 2: File Collection & Source Reading

**Input:** Target path (file or directory)  
**Processing:**
- If directory: collect all source files recursively or non-recursively via `FileCollector`
- Collect include directories for C/C++ multi-file compilation
- Read file contents (line count, byte size, raw source)
- Display file list in verbose mode

**Output:** List of file paths + concatenated source string

---

### Stage 3: Language Detection

**Input:** List of file paths  
**Processing:** Extension-based detection via `LanguageDetector`

| Extension | Language |
|-----------|----------|
| `.c` | C |
| `.cpp`, `.cc`, `.cxx` | C++ |
| `.py` | Python |
| `.java` | Java |

**Output:** Language string (`"c"`, `"cpp"`, `"python"`, `"java"`)

---

### Stage 4: Compilation / Environment Setup

**C / C++:**
- Single file: `g++ -g <file> -o <binary>`
- Multi-file: `g++ -g <files> -I<includeDirs> -o <binary>`

**Java:**
- `JavaProjectDetector` scans for `pom.xml`, `build.gradle`, Spring Boot markers, nested modules
- Dispatches to `javac`, `mvn package`, or `gradle build` accordingly

**Python:**
- `EnvironmentManager` creates a virtual environment in the project directory
- Installs `requirements.txt`; on failure, `DependencyManager` scans imports and installs packages
- Detects entry point: preferred names (`main.py`, `app.py`, ...) → `__main__` scan → first file

**Output:** Path to compiled binary / Python executable; compile output captured for error detection

---

### Stage 5: Interactive Detection

**Input:** Source files  
**Processing:** Scans each file for interactive input patterns:
- Python: `input()`, `sys.stdin`, `getpass()`
- C/C++: `std::cin`, `cin >>`, `scanf()`, `getline()`, `getchar()`, `fgets()`

**Output:** Boolean flag `isInteractive`; user is informed if true. Program is always run with `yes ""` piped as stdin to prevent hangs.

---

### Stage 6: Debugger / Tool Execution

Three mutually exclusive paths based on CLI flags:

**Path A — Debugger (`-g`):**

| Language | Debugger | Runner |
|----------|----------|--------|
| C / C++ | GDB | `GdbRunner` |
| Java | JDB | `JdbRunner` |
| Python | PDB | `PdbRunner` |

GDB batch command:
```bash
gdb --batch -ex "run" -ex "bt" -ex "info locals" <program>
```

**Path B — Valgrind (`--valgrind`, C/C++ only):**
```bash
valgrind --leak-check=full --xml=yes --xml-file=output.xml <program>
```

**Path C — Normal run (default):**
```bash
cd <project_dir> && yes "" | <program> [passthrough args]
```

Execution result classified as: `SUCCESS`, `TEST_FAILURE`, `TIMEOUT`, `SIGNAL_TERMINATED`, or `RUNTIME_ERROR`.

---

### Stage 7: Error Output Collection

All outputs are aggregated into a single error string:

1. Compiler output (`Compiler::getLastCompileOutput()`)
2. Runtime output (`RunResult::output`)
3. Debugger output (`RunResult::output` from GDB/JDB/PDB)
4. Valgrind output (`ValgrindResult::run.output`)

**Python special case:** If `No module named` is detected in the output, `DependencyManager` detects and installs missing packages, then re-runs the program. The new output is appended.

---

### Stage 8: Platform Header Check

**Input:** Combined error output  
**Processing:** Checks for missing platform-specific headers:
- `windows.h`, `conio.h`, `process.h`, `jni.h`, `ldap.h`, `dos.h`, `graphics.h`, `bios.h`

If any are found → `detectedErrors` is cleared, fix loop is skipped, only static AI analysis runs.  
**Reason:** These are environment/portability issues, not code bugs.

---

### Stage 9: Checklist Matching & Error Sorting

**Input:** Combined error output, detected language  
**Processing:**
- `ChecklistReader` loads `errorchecklist.txt` — a list of known error patterns per language
- `ErrorMatcher` compares the error output against each pattern
- `ErrorCollector` sorts matched errors by difficulty: easy → medium → hard

**Output:** Sorted list of error name strings (e.g. `["null_pointer", "memory_leak"]`)

---

### Stage 10: Auto-Fix Loop

Runs only when errors are detected. Operates within budget limits:
- **Max 20 rounds** (global)
- **Max 5 attempts** per error

```
while errors remain AND round < 20:
    current = easiest error not in give-up list
    if none → stop

    for attempt in 1..5:
        read fresh source code from disk
        build FixRequest { error_name, error_output, language, source_code, checklist }
        send to OpenAI → receive FixResult { fixed_code, file_path, success }
        apply fix via CodeChanger (writes to source file)
        recompile + re-run (redetect())
        redetect errors from fresh output
        if current error is gone → mark fixed, break

    if not fixed → add to give-up list
```

**`redetect()`** fully rebuilds and reruns the project:
- Java: re-detect project type, recompile, re-run
- C/C++: recompile with `compileMultiple` or `compileIfNeeded`, re-run
- Python: re-run with existing venv

**Python venv special case:** If a syntax error is inside `site-packages`, skip AI retries — uninstall the broken package, reinstall, then redetect.

---

### Stage 11: JSON Report Assembly

Built by `ReportJson::make_report_json()`. Contains:

```json
{
  "meta": { "format": "buggy-report-v1" },
  "config": {
    "target_path": "...",
    "fix_description": "...",
    "recursive": true,
    "verbose": false,
    "file_extensions": [],
    "passthrough_args": []
  },
  "source_code": { "content": "..." },
  "gdb":      { "exit_code": 139, "output": "..." },
  "valgrind": { "output": "...", "xml": "..." },
  "run":      { "exit_code": 0,   "output": "..." },
  "detected_errors": ["null_pointer"],
  "language": "cpp",
  "compile_output": "..."
}
```

After the OpenAI call, `"ai_analysis"` is appended and the report is written to `--json-out` if specified.

---

### Stage 12: OpenAI Analysis Call

**Input:** JSON report string  
**Processing:**
- `OpenAIClient::debug_report()` sends a POST request to the OpenAI API (libcurl)
- Bearer token authentication via `--api-token` or environment variable
- Response parsed for `text` field (Markdown analysis)

**Output:** `OpenAIResult { http_status, raw_json, text }`  
**Error handling:** Non-2xx status → print HTTP code and raw JSON, exit with code 5

---

### Stage 13: Checklist Update

**Input:** AI response text  
**Processing:**
- Scans response lines for `**Category:**` and `**Language:**` markers
- Extracts category and language values
- Appends new entries to `errorchecklist.txt` via `ChecklistReader::appendIfNew()` if not already present

**Purpose:** The checklist grows over time, improving future error detection without manual updates.

---

## 3. Error Handling Summary

| Failure Point | Behavior |
|--------------|----------|
| Invalid CLI arguments | Exception, exit |
| File / path not found | Exception, exit |
| Compilation failure | Output shown; binary absent, tool continues to AI analysis |
| Debugger not available | Step skipped |
| Program timeout | Classified as `TIMEOUT`, continues to error detection |
| Platform headers missing | Fix loop skipped, AI analysis only |
| AI fix fails after 5 attempts | Error added to give-up list, next error tried |
| OpenAI HTTP error | Error printed, exit code 5 |

---

## 4. Data Flow Example

```
$ ./buggy -g crash.cpp

→ Parse:    targetPath=crash.cpp, useGdb=true
→ Collect:  [crash.cpp]
→ Detect:   language=cpp
→ Compile:  g++ -g crash.cpp -o /tmp/buggy_compiled_1234
→ GDB:      exit_code=139, output="SIGSEGV at line 10..."
→ Collect errors: "SIGSEGV..." + compile output
→ Match:    ["null_pointer"]  (sorted by difficulty)
→ Fix loop: round 1, attempt 1
              FixRequest → OpenAI → FixResult
              CodeChanger writes fix to crash.cpp
              recompile → redetect → null_pointer gone
              → fixed in 1 attempt
→ Report:   JSON assembled
→ OpenAI:   debug_report() → HTTP 200 → analysis text
→ Checklist updated if new category found
→ Output:   analysis printed to stdout
```
