# Analysis: extending 2buggAI to fullstack / multi-service projects

Scope: let 2buggAI handle a repository made of a Python backend, a JS/TS frontend, SQL
(schema/migrations) and several services, by reusing the existing pipeline.
This document describes the code as it is **before** the change. The plan is in
[PLAN_STACK_EXTENSION.md](PLAN_STACK_EXTENSION.md).

## 1. Repository map

| Item | Where |
|------|-------|
| Build system | [makefile](../makefile): `g++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iincludes`, links `-lcurl`. Compiles `*.cpp` in the root and in `src/` into `build/`, output binary `buggy`. No CMake. |
| Entry point | [main.cpp](../main.cpp) (`main`, one ~830 line function) |
| Headers / sources | `includes/*.h`, `src/*.cpp` (23 translation units) |
| Dependencies | libcurl, nlohmann/json (system header), C++20 `std::filesystem` |
| Runtime config | environment variables only (LLM keys/models). `errorchecklist.txt` is read **from the current working directory** and is appended to at runtime. |
| Tests | [tests/run_mock_tests.sh](../tests/run_mock_tests.sh) + [tests/mock_llm_server.py](../tests/mock_llm_server.py): black-box tests of the binary against a local OpenAI-compatible mock server (11 scenarios, 53 checks). Run with `tests/run_mock_tests.sh` after `make`. There is **no C++ unit test framework** and no `make test` target. |
| Docs | `README.md`, `Guide.md`, `Documentations/*.md` (architecture, pipeline, LLM; partly outdated: they describe the single-model OpenAI flow) |

Baseline before any change: `make` builds without warnings, `tests/run_mock_tests.sh` reports
53 ok / 0 failed.

## 2. How it works today

```
main.cpp
  ArgumentParser ──► FileCollector ──► LanguageDetector::detect(files)
                                             │
        ┌──────────────────────┬─────────────┴───────────┬───────────────────────┐
        ▼                      ▼                         ▼                       ▼
   "c" / "cpp"             "java"                    "python"               "unknown"
   Compiler               JavaProjectDetector        EnvironmentManager     WebEnvironmentManager::isWebProject?
   (gcc/g++)              + Compiler (mvn/gradle)    (venv, pip) + run          │ yes
        └──────────────┬───────┴─────────────────────────┘                      ▼
                       ▼                                                 WebPipeline::run
          raw output string (compile + run + gdb + valgrind)             setupProject / runChecks -> SetupStep[]
                       ▼                                                 collectErrorOutput -> "===== STEP: ..." text
          ErrorMatcher::match(output, language)  ◄── ChecklistReader     parseErrors -> CodeError{file,line,message,step}
                       ▼                                                 ErrorMatcher (several languages)
          ErrorCollector::sortedErrors (easy -> hard)                    filesForError -> only the files of one error
                       ▼                                                        │
          fix loop (20 rounds x 5 attempts)  ◄──────────────────────────────────┘ (same loop shape)
            FixRequest ──► OpenAIClient::fix_code
                              Debugger ─► Reviewer ─► (one revision) ─► Verifier      (LlmProvider x3)
            FixResult  ──► CodeChanger::apply_fix (backup + overwrite)
            redetect: rebuild / re-run checks, match again; error gone => fixed
                       ▼
          JSON report ──► OpenAIClient::debug_report ──► ChecklistReader::appendFromAiAnalysis
```

### 2.1 Project selection and language detection

- `ArgumentParser::parse` ([src/argumentParser.cpp](../src/argumentParser.cpp)): positional
  `<path> <fix>` or `--path/--fix`, plus `--gdb`, `--valgrind`, `--runtime`, `--json-out`, `-r`, `-v`, `-e`.
  `--api-url` / `--api-token` are parsed but never used.
- `FileCollector::collectSourceFiles` ([src/FileCollector.cpp](../src/FileCollector.cpp)): every file whose
  extension `LanguageDetector::languageOf` knows, skipping `build`, `.venv`, `node_modules`, `vendor`, `dist`, ...
- `LanguageDetector` ([src/LanguageDetector.cpp](../src/LanguageDetector.cpp)):
  - `languageOf(path)`: extension table. It **already knows** `javascript`, `typescript`, `vue`, `php`, `sql`.
  - `detect(files)`: returns exactly one of `java`, `c`, `cpp`, `python`, `unknown` (hardcoded priority).
  - `detectAll(files)`: file count per language (only used for logging and by `WebPipeline`).
- Routing in `main.cpp` (lines 160-365): an `if / else if` chain on that single string.
  `unknown` + `WebEnvironmentManager::isWebProject` (root `composer.json`, `package.json` or `*.php`)
  goes to `WebPipeline::run`; everything else stays in `main`.

Consequence for the target stack: a repo with `backend/*.py`, `frontend/*.ts` and `db/*.sql` is classified
as **`python`**. `main` then creates one venv in the repo root, looks for `main.py`/`app.py`, runs that
single file and only matches Python checklist entries. The frontend and the SQL are never checked.

### 2.2 ErrorCollector and error representation

- `ErrorCollector` ([src/ErrorCollector.cpp](../src/ErrorCollector.cpp)) does **not** capture or parse errors.
  It ranks error names by difficulty (`sortedErrors`, `difficultyOf`, keyword lists) and has an unused
  `collectErrors` line filter.
- Capturing is done by `run_capture` ([src/ProcessRunner.cpp](../src/ProcessRunner.cpp)): `popen`, merges
  stderr, echoes to stdout, returns `RunResult{exit_code, output, status}`. Exit code 124 is mapped to
  `ExecutionStatus::TIMEOUT`, but nothing in the generic path applies a timeout.
- Two representations exist:
  1. **Legacy path (main.cpp):** one concatenated `std::string error_output`; an error is just a checklist
     name (`std::string`). No file/line information.
  2. **Web path:** `SetupStep{name, command, result}` per executed command
     ([includes/WebEnvironmentManager.h](../includes/WebEnvironmentManager.h)) and the only structured error
     type, `CodeError{file, line, message, step}` ([includes/WebPipeline.h](../includes/WebPipeline.h)),
     filled by `WebPipeline::parseErrors` with four generic regexes limited to
     `php|ts|tsx|js|jsx|mjs|cjs|vue`. No column, severity, tool, service or component.

### 2.3 Checklist matching

- `errorchecklist.txt`: `language | name | detection_type | keywords`. Loaded by
  `ChecklistReader::load` ([src/ChecklistReader.cpp](../src/ChecklistReader.cpp)).
- `ErrorMatcher::match(output, language)` ([src/ErrorMatcher.cpp](../src/ErrorMatcher.cpp)): case-insensitive
  substring search; an entry is considered when its language is `general` or equals the given language and
  its type is `detect_from_output`. `ai_presume` entries are only sent to the model as text.
- `WebPipeline` calls it once per detected language and merges (`matchAll`), then
  `WebPipeline::filesForError` maps an error name back to files through the keywords of that entry.
- The debug report's `**Category:**` lines are appended as new entries
  (`appendFromAiAnalysis`), so the file grows and contains duplicates and very generic keywords
  (`null`, `timeout`, `permissions`).

### 2.4 Fix loop and the agent pipeline

- Loop (twice, nearly identical: `main.cpp` 636-784 and `WebPipeline::run`): pick the easiest error not given
  up on, up to 5 attempts, `FixRequest{error_name, error_output, language, source_code, checklist}`,
  apply, **redetect from the real tool output**, error name gone means fixed.
- `OpenAIClient::fix_code` ([src/OpenAiClient.cpp](../src/OpenAiClient.cpp)):
  - `fix_code_single` (one model) or `fix_code_pipeline` (when `LlmProvider::pipelineEnabled()`).
  - Prompts are the constants `FIX_INSTRUCTIONS`, `REVIEWER_INSTRUCTIONS`, `VERIFIER_INSTRUCTIONS`.
  - Debugger input (`fix_input`): error name, language, error output, source as
    `===== FILE: <path> =====` blocks, raw checklist. Output JSON:
    `{file_path, fixed_code, new_errors, success, is_confident}` (one whole file).
  - `request_files` + `parse_fix`: `file_path` **must** be one of the `===== FILE:` paths of the request.
  - The tool builds the diff itself (`unified_diff`), the Reviewer gets error + original file + diff and
    answers `{verdict, findings[], summary}`; `CHANGES_REQUESTED` triggers exactly one revision.
  - The Verifier gets error + diff + review and answers `{confirmed, reason, remaining_issues[]}`.
    Only a confirmed fix is returned.
- `CodeChanger::apply_fix` ([src/CodeChanger.cpp](../src/CodeChanger.cpp)): copies the file to
  `<project>/backups/<rel dir>/<name>.bak` (overwritten on every fix, so only the previous version
  survives) and writes `fixed_code`.
- Real verification is the redetect step, not the Verifier agent.

### 2.5 LLM provider abstraction and keys

- `LlmProvider` ([src/LlmProvider.cpp](../src/LlmProvider.cpp)): every provider is called through an
  OpenAI-compatible `/chat/completions` endpoint with a Bearer key; retries on 429/5xx; keys are redacted
  from logs; usage statistics at exit.
- Provider table `providerSpecs()`: `gemini` (`GEMINI_API_KEY`, `GEMINI_MODEL`), `claude` (`CLAUDE_API_KEY`,
  `CLAUDE_MODEL`), `openai` (`OPENAI_API_KEY`, `OPENAI_MODEL`); endpoint override `BUGGAI_<P>_ENDPOINT`.
- Role table `roleSpecs()`: `BUGGAI_DEBUGGER` / `BUGGAI_REVIEWER` / `BUGGAI_VERIFIER`.
- `legacyConfig()`: `LLM_API_KEY|OPENAI_API_KEY`, `LLM_MODEL|OPENAI_MODEL`, `LLM_ENDPOINT|OPENAI_BASE_URL`;
  always used for the final report.
- `pipelineEnabled()`: true as soon as **any** environment variable starting with `BUGGAI_` has a value
  (and `BUGGAI_PIPELINE` is not `off`). Keys only come from the environment; nothing is hardcoded.
- The mock server is the existing way to fake providers: the endpoints are pointed at it via env vars.

## 3. Extension points and hardcoded assumptions

| Place | Kind | Note |
|-------|------|------|
| `main.cpp` routing chain | `if / else if` on the language string | Only place where a new project type can be selected. `WebPipeline` is the precedent for "leave `main` early". |
| `LanguageDetector::detect` | hardcoded 4 languages | Not touched; `languageOf`/`detectAll` are already sufficient. |
| `WebPipeline::run` | concrete calls to `WebEnvironmentManager::setupProject/runChecks` and `parseErrors` | The loop itself is generic. Making these three calls replaceable gives the plug-in point. |
| `CodeError` | struct | Add fields with defaults; existing code keeps compiling. |
| `WebPipeline::isEnvironmentError` | hardcoded name set | New environment-type names are added here. |
| `FixRequest` / `fix_input` / `review_input` | struct + prompt builders | New optional field; prompts must stay byte-identical when it is empty. |
| `errorchecklist.txt` | data | New entries are data only. `ChecklistReader::appendIfNew` already whitelists `javascript`, `typescript`, `sql`. |
| `WebEnvironmentManager` helpers (`commandExists`, `detectPackageManager`, `typeCheckCommand`, `testCommand`, `runStep`) | private static | Reusable as they are; need to become public. |
| `EnvironmentManager::createVirtualEnv` / `getPythonExecutable` | public static | Reused for per-component venvs (handles `/mnt/...` paths). |
| `FileCollector::isBuildDirectory` | hardcoded dir names | Reused indirectly; the detector needs the same skip list. |
| `makefile` | `SRCS` wildcard | New `src/*.cpp` files are picked up automatically. A `test` target has to be added. |

Hardcoded assumptions that matter:

1. One project = one language = one root. Nothing models "component" or "service".
2. `WebEnvironmentManager` installs system packages with `sudo apt` / NodeSource. That is not acceptable for
   the new path (a debugger should not change the machine) and it is left as it is for PHP projects.
3. Commands have no timeout except the PHP/JS test steps (`timeout 900`).
4. `pipelineEnabled()` reacts to every `BUGGAI_*` variable. **New configuration must not be an environment
   variable with that prefix**, otherwise it would switch the single-model mode into pipeline mode.
5. `ChecklistReader` reads `errorchecklist.txt` from the cwd and rewrites it; tests must run in a temp copy.
6. `limit(source, 80000)` in `WebPipeline` cuts the middle out of the concatenated sources. A cut file that
   the model then returns would be written back truncated. Context added by this change therefore never
   relies on cutting a file that can be fixed.

## 4. Files to touch or create

Modified:

| File | Why |
|------|-----|
| `main.cpp` | One routing block: fullstack repos go to the stack pipeline before the legacy chain. |
| `includes/argumentParser.h`, `src/argumentParser.cpp` | `--stack auto|fullstack|legacy` to force or disable the new path. |
| `includes/WebPipeline.h`, `src/WebPipeline.cpp` | `CodeError` gets `column, severity, tool, service, component`. The loop is parameterised by `PipelineHooks` (setup, check, parse, context); `WebPipeline::run` keeps its signature and passes the existing PHP/JS functions as hooks. |
| `includes/WebEnvironmentManager.h` | Five helpers move from `private` to `public` (no code change). |
| `includes/OpenAiClient.h`, `src/OpenAiClient.cpp` | `FixRequest::cross_context`; the three agent inputs get an extra section only when it is set. |
| `src/CodeChanger.cpp` | Besides the `.bak`: keep the very first original (`.orig`) and write a numbered diff per fix. |
| `errorchecklist.txt` | New section for the stack. |
| `makefile` | `test`, `unit-test` targets. |
| `tests/mock_llm_server.py` | New scenario `fullstack` with deterministic fixes; extra log fields. Existing scenarios unchanged. |
| `README.md`, `.gitignore` | Usage section; ignore test build output. |

Created:

| File | Why |
|------|-----|
| `includes/StackDetector.h`, `src/StackDetector.cpp` | Marker-file detection, components, services, docker-compose, `.buggai.json`, dependencies between components. |
| `includes/StackToolchain.h`, `src/StackToolchain.cpp` | Default and overridden commands per component, tool availability, timeouts, safe SQL dry run. |
| `includes/ToolOutputParser.h`, `src/ToolOutputParser.cpp` | Parser registry: Python traceback/compileall, pytest, ruff/flake8/mypy, tsc, eslint, jest/vitest, SQL, sqlfluff. |
| `includes/StackContext.h`, `src/StackContext.cpp` | Cross-component context selection with a size budget. |
| `includes/StackPipeline.h`, `src/StackPipeline.cpp` | Builds the `PipelineHooks` for a stack and calls the shared loop. |
| `tests/unit/*` | Unit tests. |
| `tests/fixtures/fullstack_sample/*`, `tests/fixtures/tool_output/*` | Buggy sample project and tool output samples. |
| `tests/run_fullstack_test.sh` | Offline integration test with the mock server, optional live pass. |
| `docs/PLAN_STACK_EXTENSION.md` | Plan. |

## 5. Ambiguities and the choice made

| Question | Options | Choice (least change to existing behaviour) |
|----------|---------|---------------------------------------------|
| Two fix loops exist (`main.cpp` and `WebPipeline`). Which one to extend? | Extend `main`'s loop / extend `WebPipeline`'s loop / write a third | Extend `WebPipeline`'s: it already works with steps, structured errors and per-error file selection. `main`'s loop is not touched. |
| "ErrorCollector parses errors" does not match the code. | Put parsers into `ErrorCollector` / next to `WebPipeline::parseErrors` | New `ToolOutputParser`, result type is the existing `CodeError`. `ErrorCollector` keeps doing difficulty ranking for the new errors unchanged. |
| When is a repo "fullstack"? | Any Python + any `package.json` / only with explicit flag | Auto only when there is a Python component **and** a Node component, or at least two code components that are docker-compose services or live under `services/`; never when C/C++/Java dominates or a root `composer.json` exists. `--stack legacy` restores the old routing, `--stack fullstack` forces the new one. |
| Where does configuration live? | `BUGGAI_*` env vars / project file | Project file `.buggai.json` (see assumption 4). |
| Missing tools (node, pytest, ...) | Install them like `WebEnvironmentManager` / skip | Skip the step, report it as skipped. No `sudo`, no global installs. |
| SQL checks | `sqlite3` CLI / Python stdlib `sqlite3` | Python stdlib in-memory database: no extra tool, exact file and line, never touches a real database. |

## 6. Risks and open questions

1. **Routing change for mixed repos.** A repo with Python and a `package.json` that so far ran as "python"
   now takes the new path (static checks and tests instead of starting `app.py`). Mitigation: narrow rule,
   `--stack legacy`, one log line that names the override.
2. **`WebPipeline::run` refactoring has no automated test** (the mock suite only covers the C path). It is
   kept mechanical: same statements, the three concrete calls replaced by hook calls. A PHP/JS project
   should be run once manually before a release.
3. **Tools that are not installed on the development machine.** `node` and the `sqlite3` CLI are missing
   here (WSL, Windows npm is ignored by `commandExists`), and installing Node.js or downloading a C++ test
   framework was not permitted in this session. Effects: tsc/eslint/jest/vitest parser samples are written
   from the documented output formats instead of being captured, the frontend part of the integration
   test is skipped when `node` is missing, and unit tests use a small Catch2-compatible header instead of
   Catch2 itself. Python, pytest, flake8/ruff/mypy and SQL output is captured for real.
4. **Checklist noise.** Generic entries (`null`, `timeout`, ...) match unrelated output. The loop skips
   errors without a file, but an unrelated name can still cost fix attempts.
5. **docker-compose is parsed with a small indentation parser**, not a YAML library (services, build
   context, `depends_on`, image). Anchors, `extends` and multi-file setups are not understood.
6. **SQL dialects.** The dry run uses SQLite. PostgreSQL/MySQL-only syntax would give false errors, so the
   dry run is skipped for those dialects unless `sqlfluff` is installed.
7. **Token limits.** Related files are capped per file and in total; large files are reduced to excerpts
   that cannot be overwritten.
8. Open: should a missing third-party Python module (`No module named flask`) be treated as an environment
   error? Today it cannot be told apart from a wrong local import by the checklist; left as is.
