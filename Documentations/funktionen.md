# Features & Functional Specification: 2buggAI

**Project:** 2buggAI — AI-powered Debugging Tool  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Overview

2buggAI is a CLI tool that automatically compiles, runs, debugs, and fixes code using an AI-powered loop. It supports C, C++, Java, and Python projects — single files and multi-file directories.

---

## 2. Core Features

### 2.1 CLI Input Handling

Accepts and validates all user input via command-line arguments:

| Argument | Description |
|----------|-------------|
| `<path>` | File or directory to analyze (required) |
| `--gdb` | Run debugger (GDB / JDB / PDB, auto-selected by language) |
| `--valgrind` | Run Valgrind memory checker (C/C++ only) |
| `-r` | Recursive directory scan |
| `-v` | Verbose mode — shows full source code and extended output |
| `--json-out <file>` | Write JSON report to file |
| `--api-url <url>` | Override OpenAI API endpoint |
| `--api-token <token>` | Set API key |
| `-- <args>` | Passthrough arguments forwarded to the target program |

**Example usage:**
```bash
./buggy program.cpp "fix the error"
./buggy --gdb -v crash.cpp "debug the crash"
./buggy --valgrind server "check memory" -- --port 8080
./buggy -r ./my_project "fix all errors"
```

### 2.2 Language Detection

Automatically detects the programming language from file extensions:

| Extension | Language |
|-----------|----------|
| `.c` | C |
| `.cpp`, `.cc`, `.cxx` | C++ |
| `.py` | Python |
| `.java` | Java |

The detected language controls which compiler, debugger, and error patterns are used throughout the pipeline.

### 2.3 File Collection

For directory inputs, collects all matching source files and include directories, with optional recursive traversal. For single-file inputs, the file is used directly.

### 2.4 Compilation & Environment Setup

**C / C++:**
- Single file: `g++ -g <file>`
- Multi-file: `g++ -g <files> -I<includeDirs>`
- Compile output is captured and included in error detection

**Java:**
Automatically detects the project type and uses the appropriate build tool:

| Project Type | Build Command |
|--------------|--------------|
| Plain Java | `javac` |
| Maven | `mvn package` |
| Spring Boot (Maven) | `mvn package` |
| Gradle | `gradle build` |
| Spring Boot (Gradle) | `gradle build` |
| Unknown / Nested | Falls back to nested scan, then plain `javac` |

**Python:**
- Creates a virtual environment in the project directory
- Installs `requirements.txt` via pip
- On failure: scans source files for import statements and installs detected packages
- Detects the entry point: looks for `main.py`, `app.py`, `run.py`, etc., then scans for `if __name__ == "__main__":`

### 2.5 Interactive Program Detection

Before running, scans source files for interactive input patterns (`std::cin`, `scanf`, `input()`, `sys.stdin`, etc.). If detected, informs the user and pipes `yes ""` as stdin so the program doesn't hang waiting for input.

### 2.6 Debugger Execution

The `--gdb` flag triggers a debugger. The tool automatically selects the right one based on language:

| Language | Debugger | Notes |
|----------|----------|-------|
| C / C++ | GDB | Batch mode — runs, generates backtrace, inspects locals |
| Java | JDB | Java Debugger |
| Python | PDB | Python Debugger |

GDB batch command:
```bash
gdb --batch -ex "run" -ex "bt" -ex "info locals" <program>
```

### 2.7 Valgrind Memory Analysis

Available for C/C++ via `--valgrind`. Detects:
- Memory leaks
- Invalid memory accesses
- Use-after-free
- Uninitialized memory reads

```bash
valgrind --leak-check=full --xml=yes --xml-file=output.xml <program>
```

### 2.8 Program Execution & Status Detection

When no debugger is used, the program runs normally. Output (stdout + stderr) is captured. Execution result is classified into one of five statuses:

| Status | Meaning |
|--------|---------|
| `SUCCESS` | Exited cleanly (exit code 0) |
| `TEST_FAILURE` | Ran but unit tests failed |
| `TIMEOUT` | Exceeded time limit |
| `SIGNAL_TERMINATED` | Killed by OS signal |
| `RUNTIME_ERROR` | Non-zero exit with errors |

### 2.9 Error Collection & Matching

All error output is aggregated from:
- Compiler output
- Runtime output
- Debugger output
- Valgrind output

`ErrorMatcher` compares this combined output against a persistent checklist (`errorchecklist.txt`) of known error patterns, filtered by language. Matched errors are returned as a list of error names (e.g. `segmentation_fault`, `null_pointer`, `syntax_error_python`).

`ErrorCollector` then sorts them by difficulty (easy → medium → hard) so the fix loop starts with the simplest problems.

### 2.10 Platform Header Detection

Before any fix attempt, the tool scans the error output for missing platform-specific headers:

- `windows.h`, `conio.h`, `process.h`, `jni.h`, `ldap.h`, `dos.h`, `graphics.h`, `bios.h`

If any are found, fix attempts are skipped entirely. These indicate an environment/portability issue, not a code bug. Only static AI analysis is performed.

### 2.11 Auto-Fix Loop

The core automated repair cycle. Runs only when errors are detected:

```
while errors remain AND round < 20:
    pick the easiest error not yet given up on
    for attempt in 1..5:
        read fresh source from disk
        build FixRequest (error name, error output, source, checklist)
        send to OpenAI → receive FixResult (fixed code, file path)
        apply fix via CodeChanger
        recompile + re-run → redetect errors
        if error is gone → mark fixed, break
    if not fixed after 5 attempts → add to give-up list
```

- **Max 20 rounds** (global budget to prevent endless loops)
- **Max 5 attempts** per individual error
- After each fix the project is fully rebuilt — the real output is the source of truth, not AI-reported state
- Errors in the give-up list are never retried

**Special case — Python venv syntax errors:**  
If a syntax error is detected inside an installed package (inside `site-packages`), the tool skips AI retries, uninstalls the broken package, reinstalls it, and redetects.

### 2.12 AI Analysis (OpenAI)

After the fix loop, a full JSON report is sent to the OpenAI API for a final debug analysis. The AI returns a structured explanation with:

- Error description
- Root cause
- Evidence from debug output
- Concrete fix steps
- Error category and language tag

New error categories found in the AI response are automatically appended to the checklist for future runs.

### 2.13 JSON Report

A structured report is built after execution and optionally written to a file (`--json-out`). It contains:

- Target path, fix description, CLI options
- Full source code
- GDB / Valgrind / run output
- Detected errors, language, compile output
- AI analysis text (appended after OpenAI response)

---

## 3. Data Models

**RunResult:**
```cpp
struct RunResult {
    int exit_code;
    std::string output;
    ExecutionStatus status;  // SUCCESS | TEST_FAILURE | TIMEOUT | SIGNAL_TERMINATED | RUNTIME_ERROR
};
```

**ValgrindResult:**
```cpp
struct ValgrindResult {
    RunResult run;
    std::string xml;
};
```

**FixRequest:**
```cpp
struct FixRequest {
    std::string error_name;
    std::string error_output;
    std::string language;
    std::string source_code;
    std::string checklist;
};
```

**FixResult:**
```cpp
struct FixResult {
    std::string fixed_code;
    std::string file_path;
    bool success;
    bool is_confident;
};
```

**OpenAIResult:**
```cpp
struct OpenAIResult {
    long http_status;
    std::string raw_json;
    std::string text;
};
```

---

## 4. Supported Error Types

### Memory Errors (C/C++)
- Null pointer dereference — detected via GDB (SIGSEGV)
- Memory leaks — detected via Valgrind
- Use-after-free — detected via Valgrind
- Buffer overflow — detected via Valgrind / GDB

### Compilation Errors (all languages)
- Syntax errors
- Missing includes / imports
- Type errors
- Linker errors

### Runtime Errors (all languages)
- Unhandled exceptions
- Division by zero
- Index out of bounds
- Missing Python dependencies (`No module named`)

### Java-specific
- `NullPointerException`
- `ClassNotFoundException`
- Build tool failures (Maven / Gradle)

### Python-specific
- `SyntaxError`
- `ImportError` / `ModuleNotFoundError`
- Broken packages in virtual environment

---

## 5. Not in Scope (MVP)

- Web-based interface
- Database or history storage
- IDE integration
- User management / multi-user support
- Performance profiling
- Security vulnerability scanning
- Concurrency error detection (race conditions, deadlocks)
