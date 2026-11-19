# fullstack_sample

Small, deliberately broken project used by `tests/run_fullstack_test.sh` and the unit tests.
Do not fix the bugs here: the tests copy this folder and let 2buggAI repair the copy.

| Part | Stack | Deliberate bug |
|------|-------|----------------|
| `backend/` | Python, Flask, pytest | `app/main.py` imports `app.modles` (typo, the module is `app.models`) |
| `backend/` | | `app/utils.py`: `slugify` joins with `_`, the test expects `-` |
| `frontend/` | TypeScript, tsc, vitest | `src/format.ts` still reads `user.name`; the API field was renamed to `full_name` |
| `db/` | SQLite schema + migration | `002_orders.sql`: `CREATE TABEL` |

`docker-compose.yml` describes the two services (`backend`, `frontend`).
