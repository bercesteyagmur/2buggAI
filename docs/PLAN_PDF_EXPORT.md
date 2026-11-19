# Plan: automatic PDF export

Based on [ANALYSIS_PDF_EXPORT.md](ANALYSIS_PDF_EXPORT.md).

## Decision: native C++ without a library

| Option | Assessment |
|--------|------------|
| libharu / PoDoFo | Not installed, the makefile has no mechanism to fetch dependencies, and installing needs `sudo`. Every user would need a new system package. |
| Python fallback (`tools/json2pdf.py` + reportlab) | The script does not exist, reportlab is not installed, and it would add a runtime dependency on Python packages. |
| **Own minimal PDF writer** (chosen) | The report needs only monospace text, colours and grey rectangles. That is a small, stable subset of PDF 1.4 (about 150 lines of writer code), needs no dependency and no licence, and fits the existing "all `src/*.cpp` are compiled" build. |

Limits accepted with this choice: the 14 standard fonts only (Courier, Windows-1252 characters), no
compression.

## Design

```
main.cpp / WebPipeline::runWithHooks
   │  exportJson(text, jsonPath)          where the JSON report is written
   │  exportIteration(n, data, jsonPath)  after each applied fix (per_iteration)
   ▼
PdfReportExporter ── maskSecrets ── extractBlocks ── layout (rows, wrapping) ── PDF writer
   │  destructor / finish(): combined report of the run
   ▼
<name>.pdf, <name>.iterNN.pdf, <name>.combined.pdf   (next to the JSON or in output_dir)
```

- **One class**, `PdfReportExporter`, with static, testable building blocks:
  `parseConfig`, `maskSecrets`, `extractBlocks`, `blockReference`, `render`.
- **Hooks never throw.** Every public hook catches all exceptions, logs `[PDF] ...` and returns `false`.
- **Run end:** the exporter lives in `main`; its destructor writes the combined report, so every
  `return` path is covered.
- **Run ID:** none exists in the tool; the exporter creates `YYYYMMDD-HHMMSS-<pid>` for the PDF header.
  The JSON files are not changed.

### PDF content

- Page header: file name, run ID, timestamp. Footer: `Page n of N`.
- JSON pretty-printed with 2 spaces in the order of the file (`ordered_json`), coloured: keys blue,
  strings green, numbers orange, `true`/`false`/`null` purple. Long lines wrap with a hanging indent.
- Strings with two or more lines are replaced by `"[block #1: 12 lines] first line..."` and printed after
  the JSON as blocks: heading with the JSON path (`$.steps[0].output`), line numbers, grey background.
- Invalid JSON: red notice with the parser message, then the raw text with line numbers.
- Masking: values of keys such as `api_key`, `token`, `*_token`, `authorization`, `password`, `secret`
  become `***`; `sk-...` keys and `Bearer ...` values inside any text are masked as well.
- Combined report: cover page with the list of documents, then every document on a new page.

### Configuration (defaults < `.buggai.json` < CLI)

| `.buggai.json` | CLI | Default |
|----------------|-----|---------|
| `pdf_export.enabled` | `--no-pdf` | `true` |
| `pdf_export.combined` | `--no-pdf-combined` | `true` |
| `pdf_export.per_iteration` | `--pdf-per-iteration` | `false` |
| `pdf_export.output_dir` | `--pdf-dir <dir>` | next to the JSON |

No environment variables (the `BUGGAI_` prefix would switch the agent pipeline on).

## Steps

1. `PdfReportExporter` (config, masking, blocks, layout, writer) + unit tests.
2. CLI flags.
3. Hooks in `main.cpp` and `WebPipeline`.
4. `tests/run_pdf_test.sh` with the mock LLM: PDF per JSON, combined report, per iteration, `--no-pdf`,
   failing export does not abort the run, no keys in the PDF.
5. README, full build and `make test`.

## Tests

- Unit: block extraction and reference texts, masking, invalid / empty / > 5 MB JSON, umlauts, PDF starts
  with `%PDF` and has the expected text (read back from the uncompressed streams; `pdftotext` is used in
  addition when installed), wrapping, page numbers, config parsing, export into an unwritable folder.
- Integration: see step 4.
