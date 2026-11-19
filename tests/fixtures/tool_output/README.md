# Tool output samples for the parser tests

| File | Origin |
|------|--------|
| `pytest_short.txt`, `pytest_long.txt` | **captured**: pytest 9.2.0 on `../fullstack_sample/backend` (`--tb=short` / default) |
| `python_traceback.txt` | **captured**: `python -c "import app.main"` in the backend |
| `python_compileall.txt` | **captured**: `python -m compileall -q` with an extra file `app/broken.py` (`def f(:`) |
| `flake8.txt`, `ruff_concise.txt`, `mypy.txt` | **captured**: flake8 7.5.0, ruff 0.17.5, mypy 2.4.1 on an extra file `app/lint_me.py` (not part of the fixture) |
| `sqlfluff.txt` | **captured**: sqlfluff 4.5.0, `lint --dialect sqlite` on `../fullstack_sample/db` |
| `sql_dry_run.txt` | **captured**: the tool's own SQLite dry run on `../fullstack_sample/db` |
| `tsc.txt`, `tsc_pretty.txt`, `eslint_stylish.txt`, `eslint_unix.txt`, `jest.txt`, `vitest.txt` | **written by hand** from the documented output formats. Node.js was not available when the tests were created. Replace them with real captures when possible. |

In the captured files the absolute path of the working copy was replaced by
`/work/fullstack_sample` and the virtualenv path by `/work/venv`; nothing else was edited.
The tests map `/work/fullstack_sample` back to the real fixture folder.
