# Analyse: Umbau der Report-Generierung (Fehler-/Fix-Abschnitte)

Stand: 2026-11-17, Branch `main` (inkl. der noch nicht committeten Fullstack- und PDF-Erweiterungen).
Grundlage für [PLAN_REPORT_REDESIGN.md](PLAN_REPORT_REDESIGN.md).

## 1. Aktuelle Report-Generierung

### 1.1 Klasse, Library, Layout

| Was | Wo |
|-----|----|
| PDF-Export | `PdfReportExporter` – [includes/PdfReportExporter.h](../includes/PdfReportExporter.h), [src/PdfReportExporter.cpp](../src/PdfReportExporter.cpp) |
| Library | keine. Eigener minimaler PDF-1.4-Writer (`render()`), Standardfonts Courier / Courier-Bold, WinAnsi-Encoding, unkomprimierte Content-Streams (Entscheidung in [PLAN_PDF_EXPORT.md](PLAN_PDF_EXPORT.md)) |
| Layout-Modell | anonymer Namespace in `PdfReportExporter.cpp`: `Span` (Text + `Style`), `Row` (Spans + `shaded` grau / `note` hellblau), `Section` (Zeilen eines Dokuments). Jede Zeile ist eine Textzeile à 10 pt; Umbruch über `wrapInto()` (max. 107 Spalten, hängender Einzug), Paginierung starr alle 74 Zeilen (`ROWS_PER_PAGE`), jedes Dokument beginnt auf einer neuen Seite |
| Inhalt | `buildSection()`: (1) optional die Final-Analyse `ai_analysis` als hellblaue Box „Fix recommendations“, (2) **das komplette JSON eingerückt und eingefärbt** (`writeValue()`), (3) jeder mehrzeilige String als nummerierter Codeblock (`extractBlocks()`, `writeNumberedLines()`) |
| Masking | `isSensitiveKey()`, `maskText()` (`sk-…`, `Bearer …`), `maskSecrets()` (rekursiv), `maskRawLine()` für ungültiges JSON |
| Konfiguration | `PdfExportConfig` (`enabled`, `combined`, `perIteration`, `outputDir`), gelesen aus `<projekt>/.buggai.json` → `pdf_export` (`parseConfig()`/`loadConfig()`), CLI überschreibt (`--no-pdf`, `--no-pdf-combined`, `--pdf-per-iteration`, `--pdf-dir`) in [src/argumentParser.cpp](../src/argumentParser.cpp) |
| Fehlerverhalten | alle Hooks `noexcept`, fangen alles, loggen `[PDF] …` – der Run läuft weiter |
| Werkzeug | [tools/json2pdf.cpp](../tools/json2pdf.cpp) (`make json2pdf`) wandelt beliebige JSON-Dateien mit demselben Exporter um |

### 1.2 Aufrufstellen

| Stelle | Aufruf |
|--------|--------|
| [main.cpp:57-62](../main.cpp#L57-L62) | Exporter wird pro Run angelegt; Destruktor schreibt `<name>.combined.pdf` |
| [main.cpp:806-814](../main.cpp#L806-L814) | `exportIteration()` nach jedem angewandten Fix (nur `per_iteration`) |
| [main.cpp:837-876](../main.cpp#L837-L876) | `exportJson()` nach dem Schreiben von `--json-out` (bzw. nur mit `--pdf-dir`) |
| [main.cpp:896-921](../main.cpp#L896-L921) | erneuter `exportJson()` nach der Final-Analyse (`ai_analysis`) |
| [src/WebPipeline.cpp](../src/WebPipeline.cpp) `runWithHooks()` | gleiche Hooks für PHP/JS (`WebPipeline::run`) und Fullstack (`StackPipeline::run`), `writeJson()` zweimal (vor und nach `debug_report`) |

**Fazit:** Der PDF-Report ist heute ein formatierter JSON-Dump. Pro-Fehler-Abschnitte (was / warum / wie)
gibt es nicht; die einzige „Erklärung“ ist die nachträgliche Markdown-Analyse `ai_analysis`.

## 2. Welche Daten pro Fehler tatsächlich vorliegen

Es gibt **zwei Fix-Loops** mit gleicher Logik (Fehlerkategorie wählen → bis zu 5 Versuche → neu bauen/prüfen):

* Legacy-Loop für C/C++/Java/Python: [main.cpp:676-833](../main.cpp#L676-L833)
* gemeinsamer Loop für PHP/JS und Fullstack: `WebPipeline::runWithHooks()` in [src/WebPipeline.cpp](../src/WebPipeline.cpp)

| Datum | Legacy-Loop (`main.cpp`) | Web-/Stack-Loop (`runWithHooks`) | im JSON-Report? |
|-------|--------------------------|----------------------------------|-----------------|
| Fehlerbezeichnung | `current` = Checklisten-Kategorie (`syntax_error`, …) | dito | nur als Liste `detected_errors` (Endzustand) |
| Fehlerkategorie (Checklisten-Match) | `ErrorMatcher::match()` → Kategoriename; Schwierigkeit über `ErrorCollector::difficultyOf()` | dito | Liste ohne Zuordnung |
| Datei / Zeile / Spalte | **nicht vorhanden** (nur roher `error_output`) | `CodeError` (`file`, `line`, `column`) aus `parseErrors()` / `ToolOutputParser` | `errors_with_location` (Endzustand, nicht mit Fixes verknüpft) |
| Sprache | `language` aus `LanguageDetector` | `LanguageDetector::languageOf(file)` | `language` / `languages` |
| Tool / Quelle | nicht vorhanden (Compiler-Output und Laufzeit-Output werden zusammengeklebt) | `CodeError::tool`, `step`, `component`, `service` | teilweise in `errors_with_location` |
| Schweregrad | nicht vorhanden | `CodeError::severity` (nur Fullstack-Parser) | teilweise |
| Fehlermeldung | gesamter `error_output` | `relevantOutput()` (Zeilen mit Datei/Keyword, max 12 000 Zeichen) | `compile_output` / `steps[].output` (roh, ungekürzt bzw. 6000 Zeichen) |
| Originalcode vor dem Fix | wird vor jedem Versuch von Platte gelesen (`FileCollector::readSourceCode`), aber nicht aufgehoben | dito | nur `source_code` (Stand **nach** allen Fixes) |
| Gefixter Code | `FixResult::fixed_code` (komplette Datei) | dito | nein (nur in `iterNN`-PDFs) |
| Diff | `unified_diff()` im Agent-Pipeline-Code (für Reviewer/Verifier) und in `CodeChanger` (`backups/<file>.<n>.diff`) | dito | nein |
| Debugger-Antwort | `file_path`, `fixed_code`, `new_errors`, `success`, `is_confident` | dito | nein |
| Reviewer-Ergebnis | `verdict`, `findings`, `summary` – **nur auf stdout**, nicht zurückgegeben | dito | nein |
| Verifier-Ergebnis | `confirmed`, `reason`, `remaining_issues` – **nur auf stdout** | dito | nein |
| Ergebnis des echten Re-Runs | `redetect()` → `gone` (Fehler weg ja/nein), neue Fehlerliste | `redetect()` → `gone`, Schritte (`SetupStep`) mit Exit-Code | nur Endzustand |
| Anzahl Iterationen | Schleifenzähler `attempt` (lokal) | dito | nein |
| Grund für Scheitern | stdout (`Could not fix error …`, Agent-Logs) | stdout (`[ENVIRONMENT] …`, `[ROOT CAUSE] …`, `No source file …`) | nein |
| behobene Fehler | nicht gesammelt | `fixedErrors` | `fixed_errors` (nur Web/Stack) |

## 3. Prompts und Antwortformat der Agents

Alle Prompts stehen in [src/OpenAiClient.cpp](../src/OpenAiClient.cpp); Ablauf in `fix_code_pipeline()`.
Ohne `BUGGAI_*`-Variablen läuft `fix_code_single()` mit **einem** Modell und demselben Debugger-Prompt
(kein Reviewer, kein Verifier).

| Agent | Prompt | Antwort (JSON, Parsing) | Erklärungen? |
|-------|--------|-------------------------|--------------|
| Debugger | `FIX_INSTRUCTIONS` | `{file_path, fixed_code, new_errors[], success, is_confident}` → `parse_fix()`; robust über `extract_json_object()` (auch in ```json-Blöcken) | **Nein.** Der Prompt bittet vage „also include at which error u are and the difficulty“, ohne Feld; nichts davon wird gelesen |
| Reviewer | `REVIEWER_INSTRUCTIONS` | `{verdict: APPROVE \| CHANGES_REQUESTED, findings[{file, problem, severity}], summary}` → `parse_review()`; ungültiges `verdict` = ungültige Antwort | Ja, strukturiert (`summary`, `findings`), aber nur geloggt |
| Verifier | `VERIFIER_INSTRUCTIONS` | `{confirmed, reason, remaining_issues[]}` | Ja (`reason`), nur geloggt |
| Report | inline in `debug_report()` | Markdown (Summary, Severity, Bugs mit „Buggy code / Fixed code / Explanation“) | Ja, aber **Freitext nach dem Run**, rekonstruiert aus dem JSON, nicht mit den echten Fixes verknüpft (das Modell rät Code und Zeilen) |

Bei jedem Scheitern gibt `fix_code_pipeline()` `FixResult{}` zurück – der vorgeschlagene Code, das
Review und der Ablehnungsgrund gehen für den Aufrufer verloren. Sprache aller Prompts: Englisch
(„Respond in English.“). Der Mock-Server ([tests/mock_llm_server.py](../tests/mock_llm_server.py))
erkennt die Rollen an „Senior Code Reviewer“, „final Verifier“ und „Analyze the JSON“; diese Texte
müssen erhalten bleiben.

## 4. Wird der Originalcode vor dem Fix gesichert?

Ja, aber nur auf Platte und nicht im Report:

* `CodeChanger::apply_fix()` ([src/CodeChanger.cpp](../src/CodeChanger.cpp)) schreibt vor dem
  Überschreiben `<projekt>/backups/<rel>/<datei>.bak` (letzter Stand), `<datei>.orig` (Stand vor dem
  allerersten Fix) und `<datei>.<n>.diff` (Diff jedes Fixes).
* Der Inhalt vor dem Fix wird intern gelesen (`before`), aber nicht zurückgegeben.
* Die Fix-Loops lesen den Dateistand vor jedem Versuch frisch von Platte, verwerfen ihn aber danach.

Für den Report heißt das: Originalcode und Fix-Ergebnis sind **zum Zeitpunkt des Fixes** verfügbar;
nachträglich müsste man sie aus `backups/` rekonstruieren (fehleranfällig, mehrere Fixes pro Datei).
Also muss der Record während des Loops befüllt werden.

`unified_diff()` ([src/UnifiedDiff.cpp](../src/UnifiedDiff.cpp)) enthält bereits einen LCS-Zeilendiff
(`diffOps`, Zeilenindizes alt und neu je Operation) – genau das, was für das Zeilenmapping
(alt ↔ neu, verschobene Zeilen) gebraucht wird. Er ist aber im anonymen Namespace versteckt.

## 5. Lückenanalyse

| Gefordertes Report-Feld | Status | Lücke / Quelle nach dem Umbau |
|-------------------------|--------|-------------------------------|
| 1 Titel („ImportError in backend/app.py“) | fehlt | aus Kategorie + Datei bilden |
| 2 Datei, Zeile(n) | Web/Stack: ja (`CodeError`); Legacy: **fehlt** | Legacy: Compiler-/Traceback-Ausgabe mit `ToolOutputParser::parse("generic", …)` lokalisieren; danach Debugger-`affected_lines`; zuletzt die geänderten Zeilen aus dem Diff |
| 2 Sprache | ja | – |
| 2 Tool/Quelle | Web/Stack: ja; Legacy: **fehlt** | Legacy: Compiler (Keyword steht im Compile-Output) vs. Laufzeit/Debugger/Valgrind |
| 2 Schweregrad | nur Fullstack | Tool-Severity, sonst „error“; zusätzlich Schwierigkeit (leicht/mittel/schwer) |
| 2 Fehlerkategorie | ja (Kategoriename) | + optionale Kategorie des Modells (`error_category`) |
| 3 Originale Fehlermeldung, gekürzt | **fehlt** (nur roher Output) | Auszug: Zeilen mit Keyword / Datei, Traceback-Kopf und Ausnahmezeile, max. ~14 Zeilen |
| 4 Fehlerhafter Code mit echten Zeilennummern + Kontext, rot markiert | **fehlt** | Original vor dem Versuch sichern, Hunks aus dem Zeilendiff, Kontext `report.context_lines` |
| 5 Warum (Root Cause, 2–5 Sätze) | **fehlt** | neues Debugger-Feld `root_cause_explanation` |
| 6 Gefixter Code mit Zeilennummern, grün markiert | **fehlt** | gleiche Hunks, neue Seite des Diffs |
| 7 Diff (- rot / + grün) | nur in `backups/` | `unified_diff()` je Fix, abschaltbar (`report.show_diff`) |
| 8 Erklärung der Behebung + Nebenwirkungen | **fehlt** | neue Debugger-Felder `fix_explanation`, `side_effects` |
| 9 Reviewer-Urteil + Kommentar | geht verloren | `FixResult` um Review erweitern (`approved`, `review_comment`, Findings) |
| 9 Verifier-Status, Tests bestanden | geht verloren / nur Endzustand | `FixResult` um Verifier erweitern; „Tests“ = echter Re-Run des Tools (Fehler weg?, fehlgeschlagene Checks) |
| 9 Iterationen | fehlt | Versuchszähler je Fehler |
| Deckblatt: Projekt, Run-ID, Datum, Modell/Provider | Run-ID/Datum im Seitenkopf; Modell fehlt | Modelle aus `LlmProvider` (nur Provider/Modell, nie Keys) |
| Zusammenfassung gefunden/behoben/nicht behoben, Tabelle mit Verweis | fehlt (Web/Stack: `fixed_errors`-Liste) | aus den Records; Verweis = Abschnittsnummer + Seitenzahl, klickbarer Link |
| Nicht behobene Fehler markiert, letzter Versuch + Grund | fehlt | Ablehnungsgrund aus Agent/Re-Run im Versuch speichern; vorgeschlagenen Code des letzten Versuchs aufheben |
| Anhang roher JSON (abschaltbar) | JSON ist heute der Hauptteil | `report.include_raw_json_appendix` (Default false) |
| Konfig `report.*` | fehlt | in `parseConfig()` neben `pdf_export` |
| Sprache der Erklärungen | Englisch fest | `report.language` (Default `de`), an die Prompts durchgereicht |
| Secrets maskieren | JSON-Felder und `sk-…`/`Bearer` ja | zusätzlich String-Literale nach `api_key = "…"` u. ä. in Code-Snippets |
| Seitenumbrüche, kurze Codeblöcke nicht trennen | fehlt (starr) | „keep together“-Gruppen in der Paginierung |

Sonderfälle, die der Report abbilden muss:

* Kategorien, die ohne eigenen Versuch verschwinden (z. B. `syntax_error` und `missing_semicolon_cpp`
  matchen auf dieselbe Compiler-Meldung) → „mit Fix #n behoben“.
* Fehler ohne Fix-Versuch: Umgebungsfehler, Root Cause in der Umgebung, keine Datei gefunden,
  Plattform-Header, Rundenlimit erreicht → „nicht behoben“ mit Grund.
* Einzelmodell-Modus: kein Reviewer/Verifier → im Report ausdrücklich so benannt.

## 6. Dateien, die geändert oder angelegt werden

Neu:

| Datei | Inhalt |
|-------|--------|
| `includes/FixRecord.h`, `src/FixRecord.cpp` | Datenmodell `FixRecord` / `FixAttempt` + `FixReportBuilder` (befüllt im Loop, erzeugt `fix_report`-JSON), Helfer für Fehlerauszug und Lokalisierung |
| `includes/FixSnippet.h`, `src/FixSnippet.cpp` | Snippet-Extraktion: Hunks mit echten Zeilennummern alt/neu, Markierungen, Kontext |
| `tests/unit/test_fix_snippet.cpp`, `tests/unit/test_fix_report.cpp` | Unit-Tests (Snippets, Markierung, Agent-Parsing, Maskierung, Rendering) |
| `tests/run_report_test.sh`, `tests/helpers/pdf_text.py` | Integrationstest (C++-Projekt mit 3 Fehlern, Mock-LLM) + Textextraktion ohne `pdftotext` |
| `tests/fixtures/pdf_sample/fix_report.json` | Beispiel-JSON mit `fix_report` für `make pdf-sample` |
| `docs/PLAN_REPORT_REDESIGN.md`, `docs/examples/sample_report.pdf` | Plan, Beispiel-Report |

Geändert:

| Datei | Änderung |
|-------|----------|
| `includes/UnifiedDiff.h`, `src/UnifiedDiff.cpp` | Zeilendiff (`diff_lines`) öffentlich machen, `unified_diff` nutzt ihn weiter |
| `includes/OpenAiClient.h`, `src/OpenAiClient.cpp` | neue Debugger-Felder im Prompt, `FixResult` um Erklärung/Review/Verifier/Ablehnungsgrund erweitert, Parser öffentlich testbar, Erklärungssprache |
| `includes/CodeChanger.h`, `src/CodeChanger.cpp` | Inhalt vor dem Fix zurückgeben (`previousContent()`) |
| `includes/PdfReportExporter.h`, `src/PdfReportExporter.cpp` | `report.*`-Konfiguration, neues Layout für Dokumente mit `fix_report`, Hintergrundfarben rot/grün, Keep-together, Titelzeilen, Links, Anhang |
| `main.cpp`, `src/WebPipeline.cpp`, `includes/WebPipeline.h` | `FixReportBuilder` im Loop befüllen, `fix_report` ins JSON |
| `tests/mock_llm_server.py` | Szenario `report` (3 Fehler, einer wird vom Verifier abgelehnt), neue Felder in den Antworten |
| `tests/run_pdf_test.sh`, `tests/run_pdf_sample.sh`, `tests/run_fullstack_test.sh` | Erwartungen an das neue Layout; die alten JSON-Prüfungen laufen mit eingeschaltetem Anhang weiter |
| `makefile` | `report-test` und `report-sample` in `make test` |
| `README.md` | Abschnitt zum neuen Report-Aufbau und zu den `report.*`-Optionen |
