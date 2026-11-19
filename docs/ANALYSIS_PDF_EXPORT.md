# Analysis: automatic PDF export of the JSON reports

State of the code **before** the change. Plan: [PLAN_PDF_EXPORT.md](PLAN_PDF_EXPORT.md).

## 1. Build, dependencies, tests

| Item | Finding |
|------|---------|
| Build | [makefile](../makefile), `g++ -std=c++20 -Wall -Wextra -Wpedantic`, all `*.cpp` in the root and `src/` are compiled automatically. No CMake, so no `FetchContent`/`find_package`. |
| Third-party code | Only system packages: libcurl (`-lcurl`) and the header-only nlohmann/json (`nlohmann-json3-dev`). Nothing is vendored or downloaded by the build. |
| PDF tooling on the dev machine | libharu and PoDoFo are **not** installed; `pdftotext`, `qpdf`, `gs` and Python `reportlab` are missing too. Installing packages needs `sudo`, which is not available in the implementation session. |
| `tools/json2pdf.py` | **Does not exist** (there is no `tools/` folder). There is no layout reference; the layout follows the requirements. |
| Tests | `make test`: C++ unit tests (`tests/unit/`, small Catch2-compatible header `minitest.h`), `tests/run_mock_tests.sh` (binary against a mock LLM server), `tests/run_fullstack_test.sh`. Baseline: 60 unit tests, 53 + 40 script checks, all passing. |

## 2. Where JSON files are written

The description "several JSON files per run (source, compiler output, agent answers)" does not match
the code. **One** JSON report is written per run, and **only** when `--json-out <file>` is given.
Agent answers are not stored anywhere; they are only logged to stdout.

| Path through the program | Where | When | Content |
|--------------------------|-------|------|---------|
| C / C++ / Java / Python | `main.cpp`, after the fix loop: `make_report_json()` ([src/ReportJson.cpp](../src/ReportJson.cpp)) extended with `detected_errors`, `language`, `compile_output` | 1. before the final LLM analysis, 2. again after it with `ai_analysis` | `meta`, `config`, `source_code{path, content, ...}`, `gdb`, `valgrind`, `run{exit_code, output}`, `detected_errors`, `language`, `compile_output`, `ai_analysis` |
| PHP/JS and fullstack | `WebPipeline::runWithHooks` ([src/WebPipeline.cpp](../src/WebPipeline.cpp)): early "no errors" return, and the `writeJson` lambda | 1. before the final analysis, 2. after it | `target`, `fix_description`, `languages`, `steps[{name, command, exit_code, output}]`, `detected_errors`, `fixed_errors`, `errors_with_location`, `source_code`, `stack`, `ai_analysis` |

- JSON library: **nlohmann/json**. `nlohmann::json` sorts keys alphabetically; `nlohmann::ordered_json`
  keeps the order of the file.
- Multi-line strings in these reports: `source_code.content`, `run.output`, `gdb.output`,
  `compile_output`, `steps[].output`, `ai_analysis` (Markdown).
- There is no run ID and no per-iteration file. A fix iteration ends in two places: `main.cpp` after
  `changer.apply_fix()` + `redetect()`, and `WebPipeline::runWithHooks` after `redetect(res.file_path)`.
- A run ends at several `return` statements (`main.cpp`: `return 0`, `return 5`; the pipelines return
  through `WebPipeline::runWithHooks`).

## 3. Configuration, logging, error handling

- Configuration: CLI flags (`ArgumentParser`), environment variables for the LLM providers, and
  `<project>/.buggai.json` (read by `StackDetector::parseConfig`, optional).
  **An environment variable starting with `BUGGAI_` switches the agent pipeline on**
  (`LlmProvider::pipelineEnabled`), so new settings must not use that prefix.
- Logging: plain `std::cout` / `std::cerr`, partly with tags (`[PIPELINE]`, `[STACK]`), e.g.
  `JSON Report gespeichert: <file>`.
- Errors: exceptions, caught in `main` (`Error: ...`, exit code 2). An exception from a new hook would
  therefore abort the run, so the exporter must catch everything itself.
- Secrets: API keys are only in the environment and are redacted in logs. The reports do not contain key
  fields today, but tool output inside them could.

## 4. Files to change or create

| File | Change | Why |
|------|--------|-----|
| `includes/PdfReportExporter.h`, `src/PdfReportExporter.cpp` | new | Config, masking, block extraction, layout, PDF writer, hooks |
| `includes/argumentParser.h`, `src/argumentParser.cpp` | `--no-pdf`, `--pdf-dir`, `--pdf-per-iteration`, `--no-pdf-combined` | CLI part of the configuration |
| `main.cpp` | create the exporter; call it where the JSON is written and after each fix iteration | hook for the single-language path |
| `includes/WebPipeline.h`, `src/WebPipeline.cpp` | `WebPipelineOptions::pdf`; same hooks | hook for PHP/JS and fullstack |
| `tests/unit/test_pdf_report.cpp` | new | unit tests |
| `tests/run_pdf_test.sh`, `makefile` | new script, added to `make test` | integration tests with the mock LLM |
| `README.md`, `docs/PLAN_PDF_EXPORT.md` | documentation | |

## 5. Risks and open points

1. **New default output.** With `--json-out report.json` a run now also writes `report.pdf` and
   `report.combined.pdf`. `--no-pdf` restores the old output exactly.
2. **No PDF library.** A self-written writer has to be correct PDF. It is kept to the minimum subset
   (uncompressed text and rectangles, standard fonts) and cannot be checked with an external tool here.
3. **Character set.** Standard PDF fonts cover Latin-1 / Windows-1252 only. Umlauts work; characters
   outside that set (CJK, emoji) are shown as `?`.
4. **Size.** Streams are not compressed; a 5 MB JSON gives a PDF of roughly the same order.
5. Without `--json-out` no JSON exists. A PDF is then only written when an output folder is configured.
