# 2buggAI — AI-powered Debugging Tool

2buggAI is a **CLI tool** written in C++ that automatically compiles, runs, debugs, and fixes source code using an AI-powered loop. It supports C, C++, Java, and Python — single files and multi-file projects.

---

## Platform

**This tool runs on Linux (Ubuntu).** It does not run natively on Windows or macOS.

**On Windows:** Use WSL (Windows Subsystem for Linux) with Ubuntu. You can develop inside WSL using:
- VS Code with the [WSL extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-wsl)
- CLion with the WSL toolchain

See [Guide.md](Guide.md) for IDE-specific WSL setup instructions.

---

## Requirements

The following must be installed inside your Ubuntu / WSL environment:

```bash
sudo apt update && sudo apt upgrade -y

sudo apt install -y \
    build-essential \
    gdb \
    valgrind \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    git \
    python3 \
    default-jdk \
    maven \
    wget \
    unzip
```

The following are installed automatically by the tool at runtime:
- `python3-pip` and `python3-venv` — when a Python project needs them and they are missing
- Gradle — downloaded automatically from services.gradle.org when needed
- Specific JDK versions (8, 11, 17, 21) — when a project requires one

System packages (pip, venv, JDKs, Node.js, database services of web projects) are installed with
`sudo -n`, i.e. only when sudo works without a password. The tool never waits for a password: if
sudo needs one, the step fails right away and the tool prints the command to run yourself (for
Python, for example, `sudo apt install python3-venv`).

---

## OpenAI API Key

The tool requires an OpenAI API key.

1. Create an account at [platform.openai.com](https://platform.openai.com)
2. Generate an API key under **API Keys**
3. Add it to your shell permanently:

```bash
echo 'export OPENAI_API_KEY="sk-proj-YOUR_KEY_HERE"' >> ~/.bashrc
source ~/.bashrc
```

Verify it is set:
```bash
echo $OPENAI_API_KEY
```

---

## Build

```bash
git clone https://github.com/bercesteyagmur/2buggAI.git
cd 2buggAI
make
```

The build produces a binary called `buggy`. To rebuild from scratch:

```bash
make clean && make
```

The Makefile requires `g++` with C++20 support and `libcurl`. Both are provided by `build-essential` and `libcurl4-openssl-dev`.

---

## Usage

```bash
./buggy <path> <fix-description> [options]
```

`<path>` and `<fix-description>` are both required.

| Option | Description |
|--------|-------------|
| `--gdb` | Run debugger (GDB for C/C++, JDB for Java, PDB for Python) |
| `--valgrind` | Run Valgrind memory checker (C/C++ only) |
| `-r`, `--recursive` | Recursive directory scan |
| `-v`, `--verbose` | Show full source code and extended output |
| `-e`, `--extensions` | Filter by file extensions (e.g. `-e .cpp,.h`) |
| `--stack <mode>` | Fullstack detection: `auto` (default), `fullstack` (force), `legacy` (off). See [Fullstack projects](#fullstack-projects) |
| `--json-out <file>` | Write full JSON report to file (and, automatically, `<file>.pdf`, see [PDF reports](#pdf-reports)) |
| `--no-pdf` | Do not write PDF reports |
| `--no-pdf-combined` | Do not write the combined PDF of the run |
| `--pdf-per-iteration` | Also write one PDF after every fix iteration |
| `--pdf-dir <dir>` | Write the PDFs into this folder instead of next to the JSON file |
| `--run-timeout <sec>` | Time limit for each run of the target program (default `120`, `0` = no limit). A server that is still running at the limit is stopped and counted as a timeout, not as a crash |
| `--api-url <url>` | Override OpenAI API base URL |
| `--api-token <token>` | Set OpenAI API key via CLI |
| `--` | Arguments after `--` are forwarded to the target program |
| `-h`, `--help` | Show help |

**Examples:**

```bash
# Analyze and auto-fix a single C++ file
./buggy crash.cpp "fix the segfault"

# Run with GDB in verbose mode
./buggy --gdb -v crash.cpp "debug the crash"

# Check memory with Valgrind
./buggy --valgrind program.cpp "find memory leaks"

# Analyze a full project recursively
./buggy -r ./my_project "fix all errors"

# Pass arguments to the target program
./buggy server.cpp "fix startup" -- --port 8080

# Save JSON report
./buggy --json-out report.json program.c "analyze errors"
```

**On Windows (WSL), use the WSL mount path for files on your Windows filesystem:**

```bash
./buggy /mnt/c/Users/YourName/Desktop/my-project "fix the bug" -r
```

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

The tool reads source files from disk before each fix attempt, fully rebuilds the project after every applied fix, and re-detects errors from the real output — not from AI-reported state. This makes the loop reliable even when multiple bugs interact.

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

## Fullstack projects

2buggAI can work on a repository that consists of several parts, each with its own tools:

- a **Python backend** (FastAPI / Flask / Django, pytest)
- a **JS/TS frontend** or Node service (npm / pnpm / yarn, tsc, eslint, jest / vitest)
- **SQL** (schema and migration files, alembic)
- **several services** in one repository (docker-compose, `services/<name>/`)

```bash
./buggy ./my-shop "fix all errors"                    # detected automatically
./buggy ./my-shop "fix all errors" --stack fullstack  # force it
./buggy ./my-shop "fix all errors" --stack legacy     # old single-language behaviour
```

### Detection

The root and up to three folder levels below it are searched for marker files. Every match is a
*component* with its own root folder:

| Marker | Component |
|--------|-----------|
| `pyproject.toml`, `requirements.txt`, `setup.py`, `Pipfile`, `manage.py` | Python |
| `package.json` (`tsconfig.json` makes it TypeScript) | Node |
| folder with `*.sql` files, `alembic.ini` | SQL |
| `docker-compose.yml` / `compose.yaml` | service names, build contexts, `depends_on`, database image |

The fullstack mode is used when a Python **and** a Node component exist, or when at least two code
components are docker-compose services or live under `services/`. C/C++, Java and PHP projects and
single-language Python or Node projects keep their existing behaviour. The run starts with a `STACK`
block that lists the components, their services and what depends on what.

### Checks per component

| Component | Setup | Checks |
|-----------|-------|--------|
| Python | virtualenv in `<component>/.venv`, `pip install -r requirements.txt`, pytest if tests exist | `python -m compileall` (syntax), `ruff` or `flake8` (errors only) if installed, `mypy` if configured, `pytest` |
| Node | `npm ci` / `npm install` / `pnpm install` / `yarn install` | `tsc --noEmit`, `eslint` if configured, `prisma validate` if present, `npm run test` (jest / vitest) |
| SQL | none | all `.sql` files in name order against an **in-memory SQLite** database, `alembic upgrade head --sql` (offline), `sqlfluff` for PostgreSQL / MySQL projects |

Errors are parsed into file, line, column, message, severity, tool, service and component and matched
against the checklist like in the other modes.

### Errors that cross a component boundary

When an error in one component can be caused by another one (a frontend type error after the backend
renamed an API field, a model that does not match a migration), the Debugger also receives the related
files of the neighbouring components, and for a failing test the code under test. The Reviewer is asked to
check that the fix keeps the other side working. After every applied fix the checks of the changed
component **and of all components that depend on it** are run again; an error only counts as fixed when
it is gone from that real output.

Related files are limited to 3 files, 12 000 characters each and 24 000 in total. Larger files are shown
as short read-only excerpts.

### Required tools

| Tool | Needed for |
|------|-----------|
| `python3` (with `venv`) | Python components and the SQL dry run |
| `node` + `npm` (or `pnpm` / `yarn`) | Node components |
| `sqlite3` | not needed: the SQL dry run uses Python's built-in `sqlite3` module |
| `ruff`, `flake8`, `mypy`, `sqlfluff`, `alembic` | optional, used when installed |

The fullstack mode never installs system packages and never uses `sudo`. A step whose tool is missing is
skipped and listed under "Skipped steps" and in the JSON report (`stack.skipped_steps`).

### Configuration: `.buggai.json`

Optional file in the root of the analysed project. All keys are optional.

```json
{
  "stack": "fullstack",
  "timeout_sec": 300,
  "sql_dialect": "sqlite",
  "components": [
    { "name": "api", "kind": "python", "root": "services/api", "service": "api", "depends_on": ["db"] },
    { "name": "web", "kind": "node",   "root": "web", "depends_on": ["api"] },
    { "name": "db",  "kind": "sql",    "root": "db" }
  ],
  "commands": {
    "python": { "ruff": false, "pytest": "{python} -m pytest -q -x" },
    "api":    { "contract": "{python} scripts/check_contract.py" }
  }
}
```

| Key | Meaning |
|-----|---------|
| `stack` | `fullstack` or `legacy`: same as `--stack` |
| `timeout_sec` | time limit per check command (default 300; setup commands get at least 900) |
| `sql_dialect` | `sqlite`, `postgres` or `mysql`. Default: taken from the docker-compose database image, otherwise `sqlite` |
| `components` | replaces the automatic detection. `kind` is `python`, `node` or `sql`; `root` must be inside the project |
| `commands` | per kind or per component name: a string replaces a check or adds a new one, `false` disables it. `{python}` is the component's interpreter. Tool names: `compileall`, `ruff`, `flake8`, `mypy`, `pytest`, `tsc`, `eslint`, `prisma`, `jest`, `vitest`, `sql`, `sqlfluff`, `alembic` |

LLM providers and API keys are configured exactly as before (environment variables, see
[Configuration](#configuration)).

### Safety

- Every command runs with a time limit.
- SQL is only executed in an in-memory SQLite database; alembic only renders SQL offline. Check commands
  run with `DATABASE_URL=sqlite://`, and `.env` files are not loaded, so tests cannot reach a real database
  through that variable.
- `.env*`, key and credential files are never sent to a model.
- Before a fix is written, the file is saved under `<project>/backups/`: `<file>.bak` (previous version),
  `<file>.orig` (state before the first fix) and `<file>.<n>.diff` (one diff per fix).

Side effects in the analysed project: `<component>/.venv`, `node_modules` and `backups/`.

### Tests

```bash
make test        # unit tests + agent pipeline mock tests + PDF, report and fullstack integration tests
make unit-test   # C++ unit tests only
```

`tests/run_fullstack_test.sh` runs the tool on a copy of `tests/fixtures/fullstack_sample` (a small project
with four deliberate bugs) with all LLM calls answered by the local mock server.
`FULLSTACK_LIVE_TEST=1 tests/run_fullstack_test.sh` adds one pass against the real provider configured in
your environment.
`tests/run_report_test.sh` checks the report with fix sections on a small C++ project with three compiler
errors (one of them is rejected by the mocked Verifier and stays unfixed).

---

## PDF reports

Every JSON report is exported to a readable PDF automatically. There is nothing to run afterwards and
nothing to install: the PDF is written by the tool itself (no Python, no PDF library).

```bash
./buggy ./my_project "fix all errors" --json-out out/report.json
# out/report.json           as before
# out/report.pdf            the report as PDF
# out/report.combined.pdf   all documents of the run in one PDF
```

| File | Content | When |
|------|---------|------|
| `<name>.pdf` | the JSON report | whenever the JSON file is written |
| `<name>.iterNN.pdf` | state after fix iteration NN: error, changed file, new code, remaining errors | only with `per_iteration` |
| `<name>.combined.pdf` | cover page plus every document of the run, each starting on a new page | at the end of the run |

The PDFs are written next to the JSON file, or into `output_dir` / `--pdf-dir`. Without `--json-out` no
JSON exists, so a PDF is only written when an output folder is set (`<project>_report.pdf`).

### Report layout: what was wrong, why, and how it was fixed

The report PDF (`<name>.pdf`) explains every error the run worked on. It is built from the field
`fix_report` of the JSON report, which the fix loop fills while it runs (see
[docs/PLAN_REPORT_REDESIGN.md](docs/PLAN_REPORT_REDESIGN.md)). An example is in
[docs/examples/sample_report.pdf](docs/examples/sample_report.pdf).

1. **Header**: project, target, run ID, date, mode (agent pipeline or single model) and the model of
   every role (`provider/model`, never a key), the task and the languages.
2. **Summary**: errors found / fixed / not fixed and a table of all errors
   (`No. | File:line | Category | Status | Section`). Every row links to its section; the last column
   is the page number.
3. **One section per error**:
   - title (`syntax_error in main.cpp`, or the exception name such as `ImportError in backend/app.py`)
     and a status bar: green `BEHOBEN` / red `NICHT BEHOBEN` with the reason
   - metadata: file, line(s), language, tool/source (`Compiler (g++)`, `pytest (backend)`, `tsc (frontend)`,
     ...), severity and difficulty, checklist category (and the category the model named)
   - error message, shortened to the relevant lines (the compiler message with its source and caret
     lines, the traceback frames of the file and the exception line)
   - **FEHLERHAFTER CODE**: an excerpt of the file before the fix with its real line numbers and
     `report.context_lines` lines of context; faulty lines have a red background and a `>` marker
   - **WARUM DAS EIN FEHLER IST**: the root cause in a few sentences, written by the Debugger
   - **GEFIXTER CODE**: the same excerpt after the fix; changed lines have a green background and a
     `+` marker. Line numbers stay correct when the fix inserts or deletes lines, and a fix with
     several changed places shows several excerpts
   - **DIFF** (unified, `-` red / `+` green), can be switched off
   - **ERKLÄRUNG DER BEHEBUNG**: what was changed and why that fixes the error, plus possible side effects
   - **ERGEBNIS**: Reviewer verdict and comment, Verifier verdict and reason, the result of the real
     re-run of the tool (error gone, which checks still fail) and the number of iterations
   - a list of all attempts when there was more than one

   An error that was not fixed shows the last proposal as `LETZTER VORSCHLAG (nicht übernommen)` and
   why it was not used (for example `Verifier abgelehnt: ...`). An error without a fix attempt
   (environment problem, no source file found) says so. A checklist category that disappeared together
   with another fix (g++ reports one missing semicolon as `syntax_error` and `missing_semicolon_cpp`)
   is marked `behoben (mit Nr. n)`. When the model leaves out an explanation, the report says
   `Keine Erklärung vom Modell geliefert.` and the run continues.
4. **Final analysis of the model**: the free-text analysis that is requested after the run
   (`ai_analysis`), in a light blue box.
5. **Appendix** (only with `report.include_raw_json_appendix`): the raw JSON report as before, coloured,
   with its multi-line strings as numbered blocks.

To get the explanations, the Debugger prompt asks for five more JSON fields next to the existing ones:
`root_cause_explanation`, `fix_explanation`, `affected_lines` (lines of the original file),
`error_category` and `side_effects`. The Reviewer may add `approved` and `review_comment`. All of them
are optional; answers in the old format still work.

Code blocks of up to 50 lines and their heading are not split across pages; longer lines wrap with an
empty line-number gutter. Values of fields such as `api_key`, `token`, `authorization`, `password` or
`secret` are replaced by `***`, as are `sk-...` keys, `Bearer ...` values and string literals assigned
to such names in code (`api_key = "***"`). The same masking is applied to `fix_report` in the JSON file.

JSON files without `fix_report` (for example converted with `json2pdf`, and the `iterNN` snapshots) keep
the plain JSON layout: the fix recommendations in a box at the top, the JSON indented and coloured, its
multi-line strings as numbered blocks after it, and a notice plus the raw content for invalid JSON.

Configuration, in `<project>/.buggai.json` or on the command line (the command line wins):

```json
{
  "pdf_export": { "enabled": true, "combined": true, "per_iteration": false, "output_dir": "reports" },
  "report": { "context_lines": 3, "show_diff": true, "include_raw_json_appendix": false, "language": "de" }
}
```

| Key | CLI | Default | Meaning |
|-----|-----|---------|---------|
| `pdf_export.enabled` | `--no-pdf` | `true` | write PDFs at all |
| `pdf_export.combined` | `--no-pdf-combined` | `true` | write `<name>.combined.pdf` |
| `pdf_export.per_iteration` | `--pdf-per-iteration` | `false` | write `<name>.iterNN.pdf` after each applied fix |
| `pdf_export.output_dir` | `--pdf-dir <dir>` | next to the JSON | target folder; in the config file relative to the project |
| `report.context_lines` | - | `3` | unchanged lines shown before and after a faulty / changed line (0 to 20) |
| `report.show_diff` | - | `true` | unified diff in every fix section |
| `report.include_raw_json_appendix` | - | `false` | raw JSON report as an appendix at the end |
| `report.language` | - | `de` | `de` or `en`: labels of the report and the language the Debugger is asked to write its explanations in |

To see what the PDFs look like without running an analysis (no API key needed):

```bash
make pdf-sample                                  # writes and checks build/pdf_sample/*.pdf (incl. fix_report.pdf)
make report-sample                               # runs tests/run_report_test.sh and keeps docs/examples/sample_report.pdf
build/json2pdf --out some/folder file.json ...   # convert any JSON files (after `make json2pdf`)
```

A failing export (for example an unwritable folder) is logged as `[PDF] Export fehlgeschlagen ...` and
never stops the run. Limits: the built-in PDF fonts cover Western European characters (umlauts work);
other characters such as emoji or CJK are printed as `?`. The PDFs are not compressed, so they are about
as large as the JSON. Reports with more than 250 000 lines end with a notice.

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

After the loop, a full report is sent to OpenAI for final analysis. The AI response is parsed for new error categories, which are automatically appended to `errorchecklist.txt` to improve future runs.

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
| `WebPipeline` | Shared detect / fix / re-check loop for multi-step projects (PHP/JS and fullstack) |
| `StackDetector` | Finds the components and services of a fullstack repository, reads `.buggai.json` |
| `StackToolchain` | Build / lint / test commands per component, timeouts, SQL dry run |
| `ToolOutputParser` | Parsers for pytest, Python tracebacks, ruff / flake8 / mypy, tsc, eslint, jest / vitest, SQL |
| `StackContext` | Selects related files of other components for a fix request |
| `StackPipeline` | Connects the four classes above to the shared loop |
| `PdfReportExporter` | Automatic PDF export of the JSON reports (masking, code blocks, built-in PDF writer) |

---

## Team

| Name |
|------|
| Nikola Cvetkovic |
| Krystian Piotr Kedzior |
| Berceste Yagmur Aslan |

**Supervisor:** Prof. (FH) Dr. DI Mehnen
