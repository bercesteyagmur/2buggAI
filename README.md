# 2buggAI — AI-powered Debugging Tool

2buggAI is a CLI tool written in C++ that automatically compiles, runs, debugs, and fixes source code using an AI-powered loop. It supports C, C++, Java, and Python projects.

---

## How it works

1. Collects source files from a given path (single file or directory)
2. Detects the programming language automatically
3. Compiles the code or sets up a Python virtual environment
4. Runs the program - optionally with GDB, JDB, PDB, or Valgrind
5. Collects all error output (compile + runtime + debugger)
6. Matches errors against a persistent checklist
7. Sends errors to the OpenAI API and applies fixes automatically
8. Rebuilds and retests after each fix until errors are resolved
9. Outputs a structured debug analysis in Markdown

---

## Supported languages

| Language | Compilation | Debugger | Notes |
|----------|------------|---------|-------|
| C | gcc | GDB | single & multi-file |
| C++ | g++ | GDB | single & multi-file |
| Java | javac / Maven / Gradle | JDB | auto-detects project type |
| Python | — | PDB | venv + auto dependency install |

---

## Usage

```bash
./buggy <path> [options]
```

| Option | Description |
|--------|-------------|
| `-g` | Run debugger (GDB / JDB / PDB, auto-selected) |
| `--valgrind` | Run Valgrind memory checker (C/C++ only) |
| `-r` | Recursive directory scan |
| `-v` | Verbose mode — show full source and extended output |
| `--json-out <file>` | Write full JSON report to file |
| `--api-url <url>` | Override OpenAI API endpoint |
| `--api-token <token>` | Set OpenAI API key |
| `-- <args>` | Passthrough arguments forwarded to the target program |

**Examples:**
```bash
./buggy crash.cpp
./buggy -g -v crash.cpp
./buggy --valgrind server -- --port 8080
./buggy -r ./my_project
./buggy --json-out report.json program.c
```

---

## Requirements

- g++ with C++17 support
- libcurl
- nlohmann/json
- GDB (optional, for `-g` with C/C++)
- Valgrind (optional, for `--valgrind`)
- JDK + Maven/Gradle (optional, for Java projects)
- Python 3 + pip (optional, for Python projects)
- OpenAI API key set as `OPENAI_API_KEY`

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
| `OPENAI_MODEL` | `gpt-5.2` | Model to use |

---

## Components

| Component | Description |
|-----------|-------------|
| `ArgumentParser` | CLI argument parsing and validation |
| `FileCollector` | Source file collection from directories |
| `LanguageDetector` | Extension-based language detection |
| `Compiler` | C/C++/Java compilation; Python venv setup |
| `JavaProjectDetector` | Detects Maven, Gradle, Spring Boot project types |
| `EnvironmentManager` | Python virtual environment management |
| `DependencyManager` | Python package detection and installation |
| `GdbRunner / JdbRunner / PdbRunner` | Language-appropriate debugger execution |
| `ValgrindRunner` | Memory leak and access error detection |
| `ProcessRunner` | Program execution and output capture |
| `ChecklistReader / ErrorMatcher` | Error pattern matching against persistent checklist |
| `ErrorCollector` | Sorts errors by difficulty for the fix loop |
| `OpenAIClient` | OpenAI API calls for analysis and auto-fix |
| `CodeChanger` | Applies AI-generated fixes to source files |
| `ReportJson` | Structured JSON report assembly |

---

## Team

| Name |
|------|
| Nikola Cvetkovic |
| Krystian Piotr Kedzior |
| Berceste Yagmur Aslan |

**Supervisor:** Prof. (FH) Dr. DI Mehnen
