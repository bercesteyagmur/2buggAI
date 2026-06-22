# Architecture: 2buggAI Debugging Tool

**Project:** 2buggAI — AI-powered Debugging Tool  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior, Berceste Yagmur Aslan  

---

## 1. Objective

An automated CLI debugging tool that analyzes source code across multiple languages, runs appropriate debuggers, detects errors using a checklist, and uses an AI (OpenAI) to generate fix suggestions and apply them automatically.

---

## 2. System Architecture

### 2.1 Three-Layer Architecture

```
┌────────────────────────────────────────┐
│         Frontend (CLI Tool)             │
│  - Argument parsing                     │
│  - File collection & display            │
│  - Language detection                   │
└───────────────┬────────────────────────┘
                │
                ▼
┌────────────────────────────────────────┐
│           Backend / Core                │
│  - Compilation (C/C++, Java, Python)   │
│  - Debugger execution (GDB/JDB/PDB)    │
│  - Valgrind memory analysis            │
│  - Error matching & auto-fix loop      │
│  - JSON report assembly                │
└───────────────┬────────────────────────┘
                │
                ▼
┌────────────────────────────────────────┐
│          LLM Integration                │
│  - Prompt & report construction        │
│  - OpenAI API communication (libcurl)  │
│  - Response parsing & checklist update │
└────────────────────────────────────────┘
```

---

## 3. Components

### 3.1 ArgumentParser
Parses CLI flags and options:
- `targetPath` — file or directory to analyze
- `--recursive` / `-r` — enable recursive directory scan
- `--verbose` / `-v` — show full source code and extended output
- `--gdb` — run debugger (GDB / JDB / PDB depending on language)
- `--valgrind` — run Valgrind memory checker
- `--json-out <file>` — write full JSON report to file
- `--api-url`, `--api-token` — OpenAI endpoint configuration
- Passthrough args forwarded to the target program

### 3.2 FileCollector
Collects source files from a given path. Supports single-file and recursive/non-recursive directory modes. Returns list of file paths and include directories.

### 3.3 LanguageDetector
Detects the programming language from file extensions:
- `.c` → C
- `.cpp`, `.cc`, `.cxx` → C++
- `.py` → Python
- `.java` → Java

### 3.4 Compiler
Handles compilation per language:

| Language | Strategy |
|----------|----------|
| C / C++  | `g++ -g` single or multi-file with include dirs |
| Java (Plain) | `javac` |
| Java (Maven) | `mvn package` |
| Java (Gradle) | `gradle build` |
| Java (Spring Boot Maven/Gradle) | same as above |
| Python | no compilation; virtual environment setup via `EnvironmentManager` |

### 3.5 JavaProjectDetector
Detects Java project type by scanning for `pom.xml`, `build.gradle`, Spring Boot markers, or nested module structures. Falls back to plain `javac` if no build file is found.

**Supported types:** `PlainJava`, `Maven`, `SpringBootMaven`, `Gradle`, `SpringBootGradle`

### 3.6 EnvironmentManager & DependencyManager
Manages Python project environments:
- Creates a virtual environment in the project directory
- Installs `requirements.txt` via pip
- On failure: scans imports in source files and installs missing packages
- Detects and reinstalls broken packages inside `site-packages`

### 3.7 Debugger Runners

| Debugger | Used for | CLI flag |
|----------|----------|----------|
| GdbRunner | C / C++ | `-g` |
| JdbRunner | Java | `-g` |
| PdbRunner | Python | `-g` |

GDB runs in batch mode:
```bash
gdb --batch -ex "run" -ex "bt" -ex "info locals" <program>
```

### 3.8 ValgrindRunner
Runs Valgrind for memory leak and invalid access detection. Used only for C/C++ programs via `--valgrind` flag:
```bash
valgrind --leak-check=full --xml=yes --xml-file=output.xml <program>
```

### 3.9 ProcessRunner
Executes compiled programs and captures stdout/stderr. Detects and classifies execution outcomes:

| Status | Meaning |
|--------|---------|
| `SUCCESS` | Program exited cleanly |
| `TEST_FAILURE` | Unit tests ran but failed |
| `TIMEOUT` | Program exceeded time limit |
| `SIGNAL_TERMINATED` | Killed by OS signal |
| `RUNTIME_ERROR` | Non-zero exit with errors |

Programs are run with `yes ""` piped as stdin to handle interactive prompts automatically. Interactive usage (e.g. `scanf`, `std::cin`, `input()`) is detected and the user is informed.

### 3.10 Platform Header Detection
Before fix attempts, the tool checks for platform-specific headers in the error output (e.g. `windows.h`, `conio.h`, `jni.h`). If found, fix attempts are skipped and only static analysis is performed, as these are environment issues, not code bugs.

### 3.11 ChecklistReader & ErrorMatcher
The tool maintains a persistent error checklist (`errorchecklist.txt`) with known error patterns per language. After collecting all error output (compile + runtime + debugger + Valgrind), `ErrorMatcher` matches the output against the checklist.

New error categories discovered in AI responses are automatically appended to the checklist to improve future runs.

### 3.12 ErrorCollector
Sorts detected errors by difficulty (easy → medium → hard) so the auto-fix loop starts with the simplest problems first.

### 3.13 Auto-Fix Loop (OpenAIClient + CodeChanger)
The core automated repair cycle:

```
while errors remain AND round < 20:
    pick easiest unfixed error
    for attempt in 1..5:
        read current source from disk
        send FixRequest to OpenAI
        apply fix via CodeChanger
        recompile + re-run
        re-detect errors
        if error gone → break
    if still not fixed → mark as give-up
```

- Maximum 20 global rounds, 5 attempts per error
- After each fix, the project is rebuilt from scratch to get a clean error state
- Python venv syntax errors inside installed packages trigger a reinstall instead of an AI fix

### 3.14 ReportJson
Assembles a structured JSON report containing:
- Target path, fix description, CLI flags
- Source code content
- GDB / Valgrind / run output
- Detected errors, language, compile output
- AI analysis text (appended after OpenAI response)

### 3.15 ShellQuote
Safely quotes all file paths passed to shell commands to handle spaces and special characters.

---

## 4. Data Flow

```
1. Parse CLI arguments
        │
2. Collect source files from path
        │
3. Detect language (C/C++/Python/Java)
        │
4. Compile or set up environment
        │
5. Run program (normal / GDB / JDB / PDB / Valgrind)
        │
6. Collect all error output
        │
7. Match errors against checklist → sort by difficulty
        │
8. Auto-fix loop (AI fix → recompile → redetect)
        │
9. Build JSON report
        │
10. Send to OpenAI → display analysis
        │
11. Update checklist with new error patterns
```

---

## 5. Technology Stack

| Component | Technology |
|-----------|-----------|
| Implementation language | C++17 |
| Build system | GNU Make |
| JSON | nlohmann/json |
| HTTP client | libcurl |
| LLM provider | OpenAI API |
| Debuggers | GDB, JDB, PDB |
| Memory checker | Valgrind |
| Java build tools | javac, Maven, Gradle |
| Python environment | venv + pip |

---

## 6. MVP Scope

**Included:**
- CLI-based tool
- Multi-language support (C, C++, Java, Python)
- Automated compilation and environment setup
- Debugger integration (GDB / JDB / PDB)
- Valgrind memory analysis
- AI-powered auto-fix loop with checklist learning
- JSON report output

**Not included:**
- Web-based interface
- Database integration
- User management
- IDE plugins
- REST API endpoint
- Multi-user operation
