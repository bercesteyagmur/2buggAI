# Plan: fullstack / multi-service support

Based on [ANALYSIS_STACK_EXTENSION.md](ANALYSIS_STACK_EXTENSION.md). Principle: the existing loop in
`WebPipeline` stays the only fix loop; the new stack plugs into it through hooks. The C/C++, Java,
Python and PHP/JS paths keep their behaviour.

```
main.cpp ── StackDetector::detect ── isFullstack? ──► StackPipeline::run
                                                        │ builds PipelineHooks
                                                        ▼
                                              WebPipeline::runWithHooks   (shared loop)
     hooks.setup   ─► StackToolchain::setup     (venv / npm ci, with timeout)
     hooks.check   ─► StackToolchain::check     (affected component + dependents)
     hooks.parse   ─► ToolOutputParser          (one parser per tool -> CodeError)
     hooks.context ─► StackContext              (related files of other components)
                                                        ▼
                               OpenAIClient::fix_code (Debugger -> Reviewer -> Verifier)
```

## Step 1: Stack detection (`StackDetector`)

Data model:

```cpp
struct StackComponent {
    std::string name;       // unique, e.g. "backend", "frontend", "db"
    std::string service;    // docker-compose service or folder under services/
    std::string kind;       // "python" | "node" | "sql"
    std::string language;   // "python" | "typescript" | "javascript" | "sql"
    std::string root;       // relative to the project root, "." for the root itself
    std::vector<std::string> dependsOn;   // component names
};
struct ProjectStack { std::vector<StackComponent> components; /* compose info, config */ };
```

Markers, searched in the root and up to three levels below it (same skip list as `FileCollector`):

| Marker | Component |
|--------|-----------|
| `pyproject.toml`, `requirements.txt`, `setup.py`, `Pipfile`, `manage.py` | python |
| `package.json` (+ `tsconfig.json` decides typescript/javascript) | node |
| `alembic.ini`, a `migrations/` folder with `*.sql`, any folder with `*.sql` | sql |
| `prisma/schema.prisma` inside a Node component | extra check (`prisma validate`) of that component |
| `docker-compose.yml`, `docker-compose.yaml`, `compose.yml`, `compose.yaml` | services, build contexts, `depends_on`, DB image |
| `services/<name>/...` | service name |

- Several services per repo: every marker folder is its own component with its own root.
- Service of a component: compose service whose build context contains the root, else `services/<name>`,
  else the first path segment.
- Dependencies: `.buggai.json` > compose `depends_on` > heuristics (Python components depend on SQL
  components; a Node component without compose information depends on the Python components).
- `isFullstack()`: Python + Node component, or two or more code components that are compose services or
  live under `services/`.
- `componentOf(file)`: deepest component root that contains the file, matching the file's language when
  several components share a root.
- Routing in `main.cpp`: only for directories, only when `detect()` returned `python` or `unknown`, not
  with a root `composer.json`. `--stack legacy|fullstack|auto`.

## Step 2: Toolchain per component (`StackToolchain`)

Each command is `{tool, command, phase(setup|check|test), requires}` and becomes one `SetupStep` named
`"<component>: <tool>"`. Defaults:

| Kind | Setup | Checks |
|------|-------|--------|
| python | venv through `EnvironmentManager::createVirtualEnv`, `pip install -r requirements.txt` (or `pip install -e .`), `pip install pytest` when tests exist and pytest is missing | `python -m compileall -q` (py_compile for the whole tree), `ruff check` or `flake8` if installed, `mypy` if configured and installed, `python -m pytest -q --tb=short` if tests exist |
| node | `npm ci` / `pnpm install --frozen-lockfile` / `yarn install` (`WebEnvironmentManager::detectPackageManager`) | `tsc --noEmit --pretty false` (`typeCheckCommand`), `eslint .` if configured, `<pm> run test` for jest/vitest (`testCommand`), `prisma validate` if present |
| sql | none | raw `.sql`: all files in name order into an in-memory SQLite database (Python stdlib); alembic: `alembic upgrade head --sql` (offline, no connection); other dialects: `sqlfluff lint` if installed, otherwise skipped |

Rules:

- Every command runs as `timeout <n> ...` (default 300 s, setup 900 s); exit code 124 is reported as a timeout.
- A missing binary skips the step and lists it under "skipped"; nothing is installed system-wide and `sudo`
  is never used.
- Overrides in `<project>/.buggai.json`:

```json
{
  "stack": "fullstack",
  "timeout_sec": 300,
  "sql_dialect": "sqlite",
  "components": [
    { "name": "api", "kind": "python", "root": "services/api", "service": "api", "depends_on": ["db"] }
  ],
  "commands": {
    "python": { "ruff": false, "pytest": "python -m pytest -q -x" },
    "api":    { "contract": "python scripts/check_contract.py" }
  }
}
```

`commands` keys are a component name or a kind; a string replaces or adds a tool, `false` disables it.
`{python}` in a command is replaced by the component's interpreter. `components` replaces auto-detection.

## Step 3: Error parsers (`ToolOutputParser`)

`CodeError` becomes `{file, line, column, message, step, severity, tool, service, component}` (new fields
default to empty/0). `ToolOutputParser::parse(tool, output, componentRoot, projectRoot)` looks the parser up
in a registry; unknown tools fall back to the existing `WebPipeline::parseErrors`.

| Parser | Format |
|--------|--------|
| `python` | `File "x.py", line N` frames + final `XError: message`; compileall `*** Error compiling`; SyntaxError caret blocks |
| `pytest` | short tracebacks `path.py:N: in func`, `E   ...` lines, `FAILED path::test - msg`, `ERROR path - msg` collection errors |
| `lint` (ruff, flake8, mypy) | `path:line:col: CODE message`, `path:line: error: message  [code]` |
| `tsc` | `path(line,col): error TSxxxx: message` and the `path:line:col - error TSxxxx:` pretty form |
| `eslint` | stylish blocks (path line, then `line:col  error  message  rule`) and `path:line:col: message [Error/rule]` |
| `jest` (jest, vitest) | `FAIL path`, `● suite › test`, `at ... (path:line:col)`, vitest `❯ path:line:col` and `AssertionError` lines |
| `sql` | `path.sql:line: error: message` from the dry run, sqlite3 CLI `Parse error near line N`, alembic tracebacks |
| `sqlfluff` | `== [path] FAIL` + `L: n | P: n | CODE | message` |

Paths are resolved against the component root, made relative to the project root, and dropped when they
are outside the project or inside `node_modules` / a venv / `site-packages`.

## Step 4: Checklist entries

New section in `errorchecklist.txt` (existing entries already cover `ImportError`, `TS23xx`, `no such table`,
...): pytest collection errors, circular imports, missing fixtures, mypy and undefined-name findings,
missing environment variables, API contract errors (422 / validation), TS property/shape mismatches,
failing JS tests, eslint errors, failing migrations, foreign key errors, ORM/schema mismatches, and the
environment-type errors `pip_install_failed`, `npm_install_failed`, `command_timeout` (added to
`WebPipeline::isEnvironmentError`, never sent to the model).

## Step 5: Agent context across components (`StackContext`)

For an error in component C:

1. Candidates are files of the components C depends on and of the components that depend on C.
2. Identifiers are taken from the error messages (quoted names, `no such column: x`, `Property 'x'`, type
   names); each one is also tried in snake_case and camelCase (`full_name` <-> `fullName`), because API and
   ORM field renames usually differ exactly there.
3. Files are scored by distinct identifier hits, plus a bonus for contract-like paths (models, schemas,
   routes, api, types, migrations, `.sql`). Test files, lock files, `.env*` and key files are never used.
4. Budget: at most 3 related files, 12 000 characters each, 24 000 in total. A file inside the budget is
   sent as a normal `===== FILE:` block (the Debugger may fix it there). A larger file is reduced to
   excerpts around the hits and sent as read-only text, so it can never be written back truncated.
5. `FixRequest::cross_context` carries a short note (which component the error is in, what the related
   files are, read-only excerpts). When it is empty, all prompts are identical to today.
   - Debugger: note is appended to the input.
   - Reviewer: gets the note and the related files and is asked to check that the other side still works.
   - Verifier (agent): gets the note. Verifier (tool): after a fix `hooks.check(changedFile)` re-runs the
     checks of the changed file's component **and of every component that depends on it**; other components
     keep their last result.

## Step 6: Safety

- Timeouts on every command (step 2).
- Databases: SQL is only executed in an in-memory SQLite database; alembic only in offline mode; check and
  test commands run with `DATABASE_URL=sqlite://` so a test suite cannot reach a configured real database.
  `.env` files are not loaded.
- Secrets: `.env*`, `*.pem`, `*.key`, `id_rsa*`, `credentials*`, `secrets*` are excluded from context; the
  tool never commits anything; API keys still only come from the environment and stay redacted in logs.
- Backups: `CodeChanger` keeps `backups/<path>.bak` (previous version, as before), plus
  `backups/<path>.orig` (state before the first fix) and `backups/<path>.<n>.diff` for every applied fix.

## Implementation order

1. `CodeError` fields, `PipelineHooks`, `runWithHooks`; build; existing mock suite.
2. `StackDetector` + unit tests.
3. `ToolOutputParser` + fixtures + unit tests.
4. `StackToolchain`.
5. `StackContext`, `FixRequest::cross_context` + unit tests.
6. `StackPipeline`, `--stack`, routing in `main.cpp`.
7. Checklist, `CodeChanger` backups.
8. Fixture project, mock scenario, integration test, `make test`.
9. README, final full build and test run.

## Changes made while implementing

- **Code under test.** The first end-to-end run showed that a failing test only names the test file in
  its traceback, so the Debugger never saw the defective module. `StackContext` now also adds the project
  modules imported by a failing test file (same budget).
- **Weak matches.** A related file needs two matching names, or one name of four or more characters in a
  contract-like file. A single short word pulled in unrelated files.
- **Unit test framework.** Catch2 could not be downloaded in the implementation session. `tests/unit/minitest.h`
  provides the Catch2 macros that the tests use (`TEST_CASE`, `CHECK`, `REQUIRE`), so the tests can move to
  Catch2 by swapping the include.
- **Frontend without Node.js.** `tests/run_fullstack_test.sh` replaces `tsc` by `tests/helpers/tsc_stand_in.py`
  (through the `.buggai.json` override) when `node` is missing, so the cross-component path is tested on
  every machine. With `node` installed the real `tsc` and `vitest` run.

## Test plan

- `make unit-test`: detection (single service, multi-service, mixed repo, no match, config override),
  every parser against files in `tests/fixtures/tool_output/`, checklist matching for the new entries
  (using the real `errorchecklist.txt`), context selection.
- `tests/run_fullstack_test.sh`: copy of `tests/fixtures/fullstack_sample`, mock LLM, asserts detection,
  classification, routing through the three agents, applied fixes, clean re-check, backups. Parts whose
  tools are missing (pytest, node) are reported as skipped. With `FULLSTACK_LIVE_TEST=1` and a real key in
  the environment one live pass is run; otherwise it is skipped.
- `make test`: unit tests, the existing `tests/run_mock_tests.sh`, then the fullstack test.
