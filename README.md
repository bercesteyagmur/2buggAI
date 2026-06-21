# 2buggAI — AI-powered Debugging Tool

2buggAI is a CLI tool written in C++ that automatically compiles, runs, debugs, and fixes source code using an AI-powered loop. It supports C, C++, Java, and Python — single files and multi-file projects.

---

## How it works

```
Source path
    │
    ├── Collect source files (recursive or single)
    ├── Detect language from file extensions
    ├── Compile / set up environment
    ├── Run program (normally, with debugger, or with Valgrind)
    ├── Collect all error output (compile + runtime + debugger + Valgrind)
    ├── Match errors against checklist → sort by difficulty
    ├── Auto-fix loop: AI fix → recompile → redetect (up to 20 rounds)
    ├── Build JSON report
    └── Send to OpenAI → structured Markdown analysis
```

The tool reads the actual source files from disk before each fix attempt, fully rebuilds the project after every applied fix, and re-detects errors from the real output — not from AI-reported state. This makes the loop reliable even when multiple bugs interact.

---

## Supported languages

| Language | Compilation | Debugger | Notes |
|----------|------------|---------|-------|
| C | `gcc -g` | GDB | single & multi-file with include dirs |
| C++ | `g++ -g` | GDB | single & multi-file with include dirs |
| Java | `javac` / `mvn` / `gradle` | JDB | auto-detects Plain, Maven, Gradle, Spring Boot |
| Python | — | PDB | auto venv + dependency install from imports |

### Java project type detection

`JavaProjectDetector` scans the project directory for build files and selects the appropriate build tool automatically:

| Detected | Build command |
|----------|--------------|
| `pom.xml` | `mvn package` |
| Spring Boot + `pom.xml` | `mvn package` |
| `build.gradle` | `gradle build` |
| Spring Boot + `build.gradle` | `gradle build` |
| Nested module with `pom.xml` / `build.gradle` | recurse into module |
| None of the above | `javac` |

### Python environment management

For Python projects, the tool:
1. Creates a virtual environment in the project directory
2. Installs `requirements.txt` via pip
3. On install failure — scans all `.py` files for `import` statements and installs detected packages
4. Detects entry point: looks for `main.py`, `app.py`, `run.py`, `manage.py`, `server.py`, `cli.py` — then scans for `if __name__ == "__main__"` — then falls back to the first file
5. If a missing package is detected at runtime (`No module named ...`), installs it and retries automatically

---

## Auto-fix loop

The core repair cycle. Runs only when errors are detected after the initial run.

```
while errors remain AND round < 20:
    pick the easiest error not yet given up on
    for attempt in 1..5:
        read fresh source code from disk
        send FixRequest to OpenAI:
            { error_name, error_output, language, source_code, checklist }
        receive FixResult:
            { file_path, fixed_code, success, is_confident }
        write fixed_code to file_path (CodeChanger)
        recompile + re-run → redetect errors
        if error is gone → mark fixed, move to next
    if still failing after 5 attempts → give up on this error, try next
```

After the loop, a full report is sent to OpenAI for a final analysis. The AI response is parsed for new error categories, which are automatically appended to `errorchecklist.txt` to improve future runs.

---

## Error types detected

**Memory errors (C/C++ via GDB / Valgrind)**
- Null pointer dereference (SIGSEGV)
- Memory leaks
- Use-after-free
- Buffer overflow
- Uninitialized memory reads

**Compilation errors (all languages)**
- Syntax errors
- Missing includes / imports
- Type errors
- Linker errors

**Runtime errors (all languages)**
- Unhandled exceptions
- Division by zero
- Index out of bounds

**Java-specific**
- `NullPointerException`, `ClassNotFoundException`
- Maven / Gradle build failures

**Python-specific**
- `SyntaxError`, `ImportError`, `ModuleNotFoundError`
- Broken packages inside virtual environment (auto-reinstalled)

**Platform issues**
- If platform-specific headers are missing (`windows.h`, `conio.h`, `jni.h`, etc.), the fix loop is skipped and only AI analysis is performed — these are environment issues, not code bugs.

---

## Usage

```bash
./buggy <path> [options]
```

| Option | Description |
|--------|-------------|
| `-g` | Run debugger (GDB for C/C++, JDB for Java, PDB for Python) |
| `--valgrind` | Run Valgrind memory checker (C/C++ only) |
| `-r` | Recursive directory scan |
| `-v` | Verbose — show full source code and extended output |
| `--json-out <file>` | Write full JSON report to file |
| `--api-url <url>` | Override OpenAI API base URL |
| `--api-token <token>` | Set OpenAI API key via CLI |
| `-- <args>` | Passthrough arguments forwarded to the target program |

**Examples:**

```bash
# Analyze and auto-fix a single C++ file
./buggy crash.cpp

# Run with GDB in verbose mode
./buggy -g -v crash.cpp

# Check memory with Valgrind
./buggy --valgrind program.cpp

# Analyze a full Java/Python/C++ project recursively
./buggy -r ./my_project

# Pass arguments to the target program
./buggy server.cpp -- --port 8080

# Save JSON report
./buggy --json-out report.json program.c
```

Interactive programs (`scanf`, `std::cin`, `input()`, etc.) are automatically detected. The tool informs the user and pipes empty input so the program does not hang.

---

## Output

After analysis, the tool prints a structured Markdown report:

```
## Summary
Brief description of the main issue.

## Severity
critical / high / medium / low

## Bugs

### Bug 1: [Title]
- **File:** path/to/file:line
- **Category:** memory_leak / null_pointer / ...
- **Language:** c / cpp / java / python / general
- **Problem:** What is wrong

**Buggy code:**
...

**Fixed code:**
...

**Explanation:** Why the fix works.

## Recommendations
...
```

---

## Requirements

- `g++` with C++17 support
- `libcurl`
- `nlohmann/json`
- `GDB` — optional, for `-g` with C/C++
- `Valgrind` — optional, for `--valgrind`
- `JDK` + `Maven` / `Gradle` — optional, for Java projects
- `Python 3` + `pip` — optional, for Python projects
- `OPENAI_API_KEY` set in environment

---

## Build

```bash
make
```

---

## Configuration

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `OPENAI_API_KEY` | *(required)* | OpenAI API key |
| `OPENAI_BASE_URL` | `https://api.openai.com` | API base URL |
| `OPENAI_MODEL` | `gpt-5.2` | Model used for analysis and fixes |

---

## Components

| Component | Description |
|-----------|-------------|
| `ArgumentParser` | CLI argument parsing and validation |
| `FileCollector` | Source file and include dir collection |
| `LanguageDetector` | Extension-based language detection |
| `Compiler` | C/C++/Java compilation |
| `JavaProjectDetector` | Detects Maven, Gradle, Spring Boot project types |
| `EnvironmentManager` | Python virtual environment creation and management |
| `DependencyManager` | Python package detection and installation |
| `GdbRunner` | GDB batch-mode execution for C/C++ |
| `JdbRunner` | JDB execution for Java |
| `PdbRunner` | PDB execution for Python |
| `ValgrindRunner` | Valgrind memory analysis |
| `ProcessRunner` | Program execution and output capture with status classification |
| `ChecklistReader` | Loads and updates `errorchecklist.txt` |
| `ErrorMatcher` | Matches error output against checklist patterns |
| `ErrorCollector` | Sorts errors by difficulty (easy → medium → hard) |
| `OpenAIClient` | OpenAI API — `debug_report()` and `fix_code()` |
| `CodeChanger` | Writes AI-generated fixes to source files |
| `ReportJson` | Structured JSON report assembly |
| `ShellQuote` | Safe shell quoting for paths with spaces |

---

## Team

| Name |
|------|
| Nikola Cvetkovic |
| Krystian Piotr Kedzior |
| Berceste Yagmur Aslan |

**Supervisor:** Prof. (FH) Dr. DI Mehnen
