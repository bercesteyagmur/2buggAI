# Plan: Report mit Fehler-/Fix-Abschnitten

Grundlage: [ANALYSIS_REPORT_REDESIGN.md](ANALYSIS_REPORT_REDESIGN.md).

Leitlinien: kein zweiter Exporter (der bestehende `PdfReportExporter` bekommt ein zweites Layout),
der Record wird **im Fix-Loop** befüllt, alles Neue ist ohne LLM testbar, Fehler im Report-Code
brechen den Run nie ab.

```
Fix-Loop (main.cpp / WebPipeline::runWithHooks)
   │ beginError()  ──────────────► FixRecord  (Kategorie, Datei, Zeilen, Tool, Meldung …)
   │ fix_code() → FixResult (+ explanation, review, verification, failure_reason)
   │ CodeChanger::apply_fix() → previousContent()
   │ redetect()
   │ addAttempt(record, result, original, applied, gone, Checks)  ──► FixAttempt (+ Hunks, Diff)
   │ finishError()
   ▼
FixReportBuilder::toJson()  ──► "fix_report" im JSON-Report (--json-out und PDF)
   ▼
PdfReportExporter: Dokument mit "fix_report" → neues Layout, sonst wie bisher (JSON-Dump)
```

## 1. Datenmodell

`includes/FixRecord.h`:

```cpp
struct FixAttempt {
    int number;                    // 1-basiert
    std::string file;              // geänderte Datei (projektrelativ)
    bool applied;                  // in die Datei geschrieben
    bool errorGone;                // echter Re-Run: Fehler verschwunden
    std::string outcome;           // "fixed" | "still_present" | "rejected" | "no_fix" | "apply_failed"
    std::string reason;            // warum nicht behoben (Agent, Re-Run, Schreibfehler)
    FixExplanation explanation;    // Debugger: root cause, fix, affected_lines, Kategorie, Nebenwirkungen
    ReviewOutcome review;          // ran, approved, comment, findings
    VerifyOutcome verification;    // ran, confirmed, reason, remaining_issues
    std::vector<std::string> remainingErrors, failedChecks;   // Zustand nach dem Re-Run
    std::vector<SnippetHunk> hunks;                            // vorberechnet (context_lines)
    std::string diff;                                          // unified diff
};

struct FixRecord {
    int id;                                    // Abschnittsnummer
    std::string errorName, category, difficulty, title;
    std::string file; std::vector<int> lines;  // Ort laut Tool-Ausgabe
    std::string language, tool, severity;
    std::string message;                       // gekürzte Fehlermeldung
    std::string status;                        // "fixed" | "not_fixed"
    std::string statusReason;                  // z. B. "Verifier abgelehnt: …", "mit Fix #1 behoben"
    int resolvedBy = 0;                        // indirekt behoben durch Record #n
    std::vector<FixAttempt> attempts;          // Iterationen = attempts.size()
};
```

`FixReportBuilder` (eine Instanz pro Run) bietet `observe(errors, causedBy)` (merkt sich jede gesehene
Kategorie samt Ort/Meldung und welche nach einem Fix mit verschwunden sind), `beginError()`,
`skipError()` (kein Fix-Versuch), `addAttempt()`, `finishError()`, `finalize(finalErrors)` und `toJson()`.
Ort, Tool und Meldung liefert ein vom Loop gesetzter `Describer` aus der jeweils aktuellen Ausgabe. Gespeichert werden nur Snippets (Hunks) und Diff, nicht ganze Dateien. Im Bericht erscheint
pro Record der **übernommene** Versuch bzw. bei nicht behobenen Fehlern der **letzte Versuch mit
Code-Vorschlag**; alle Versuche stehen kompakt in einer Versuchsliste.

JSON-Schema (`fix_report`, Version 1):

```json
{ "schema": 1, "project": "proj", "target": "...", "run_id": "...", "language": "de",
  "models": {"mode": "pipeline", "debugger": "gemini/…", "reviewer": "claude/…", "verifier": "openai/…", "report": "llm/…"},
  "summary": {"found": 5, "fixed": 4, "not_fixed": 1},
  "fixes": [ { "id": 1, "title": "...", "error_name": "...", "category": "...", "file": "...", "lines": [7],
               "language": "cpp", "tool": "Compiler (g++)", "severity": "error", "difficulty": "leicht",
               "status": "fixed", "status_reason": "", "resolved_by": 0, "message": "...", "iterations": 1,
               "attempts": [ { "number": 1, "outcome": "fixed", "reason": "", "applied": true, "error_gone": true,
                               "file": "...", "explanation": {...}, "review": {...}, "verification": {...},
                               "remaining_errors": [], "failed_checks": [],
                               "hunks": [ {"old": [{"line": 5, "text": "...", "mark": false}], "new": [...]} ],
                               "diff": "..." } ] } ] }
```

## 2. Agent-Erweiterung

* **Debugger** (`FIX_INSTRUCTIONS`): das JSON bekommt zusätzlich
  `root_cause_explanation`, `fix_explanation`, `affected_lines` (Zeilen im **Original**),
  `error_category`, `side_effects`. Alle bisherigen Felder bleiben; der vage Satz „also include at which
  error u are …“ wird durch die Felder ersetzt. Die Erklärungen sollen in `report.language` geschrieben
  werden (`FixRequest::explanation_language`, leer = Prompt wie bisher ohne Sprachhinweis).
* **Reviewer**: bisheriges Schema bleibt (`verdict` ist maßgeblich). Neu dokumentiert und optional:
  `approved` (bool) und `review_comment`. Parsing: fehlt `verdict`, aber `approved` ist da, wird es daraus
  abgeleitet; `review_comment` fällt auf `summary` zurück.
* **Verifier**: Schema bleibt (`confirmed`, `reason`, `remaining_issues`). Der „Testergebnis“-Teil kommt
  nicht vom Modell, sondern vom echten Re-Run des Tools (`redetect()`), das ist verlässlicher.
* `FixResult` bekommt `explanation`, `review`, `verification`, `failure_reason`. Bei Ablehnung bleiben
  `file_path`/`fixed_code` des Vorschlags gesetzt, `success` ist `false` – beide Aufrufer prüfen `success`
  zuerst, das Verhalten des Loops ändert sich nicht.
* **Robustes Parsing** (`parse_fix_explanation()`, `parse_review_outcome()`, öffentlich und getestet):
  Strings oder String-Arrays werden akzeptiert, `affected_lines` als Zahlen, Ziffern-Strings oder
  Bereiche („12-14“), alles andere wird ignoriert (max. 200 Zeilen). Fehlt ein Text, steht im Report
  „Keine Erklärung vom Modell geliefert.“ – der Run läuft unverändert weiter.

## 3. Snippets und Zeilenmapping

* Originalinhalt **vor** jedem Versuch: `CodeChanger::previousContent()` (der Inhalt, den `apply_fix()`
  ohnehin liest); bei abgelehnten Vorschlägen der frisch gelesene Dateistand.
* `UnifiedDiff`: der vorhandene LCS-Diff wird als `diff_lines(old, new)` öffentlich (Operationen mit
  Zeilenindex alt **und** neu); `unified_diff()` nutzt ihn unverändert.
* `FixSnippet::build(old, new, errorLines, context)`:
  1. „interessante“ Operationen = geänderte Zeilen (`-`/`+`) und gemeldete Fehlerzeilen (alt),
  2. Gruppierung wie bei Unified-Diff-Hunks (Abstand ≤ 2·context+1 → ein Hunk), dadurch **mehrere Hunks
     pro Fix**,
  3. pro Hunk zwei Sichten: alt (`' '` und `'-'`, echte alte Zeilennummern, Markierung = entfernt/geändert
     oder gemeldete Fehlerzeile) und neu (`' '` und `'+'`, echte neue Zeilennummern, Markierung =
     hinzugefügt/geändert). Weil jede Operation beide Indizes trägt, stimmen die Nummern auch nach
     eingefügten oder gelöschten Zeilen.
  4. Kontext an Dateianfang/-ende wird abgeschnitten, nicht aufgefüllt.
* Ohne Code-Vorschlag (z. B. kein Fix geliefert): `FixSnippet::excerpt(old, errorLines, context)` zeigt nur
  den fehlerhaften Ausschnitt.
* Grenzen: max. 8 Hunks und 120 Zeilen pro Seite eines Fixes, danach Hinweis „… gekürzt, siehe Diff“.

## 4. Rendering

`PdfReportExporter::buildSection()` verzweigt: enthält das JSON-Objekt `fix_report`, wird das neue
Layout gebaut, sonst das bisherige (für beliebige JSON-Dateien, `json2pdf` und die Iterations-PDFs).

Aufbau:

1. **Kopf** (erste Seite): Titel „2buggAI Fehlerbericht“, Projekt, Ziel, Run-ID, Datum, Modus und Modelle
   pro Rolle, Fix-Beschreibung.
2. **Zusammenfassung**: gefunden / behoben / nicht behoben, Tabelle `Nr. | Datei:Zeile | Kategorie | Status | Abschnitt`
   mit Seitenzahl des Abschnitts; die Zeile ist ein PDF-Link auf diese Seite.
3. **Fehler-Abschnitte** (jeweils mit großem Titel): Metadaten-Tabelle, Fehlermeldung (grau),
   „FEHLERHAFTER CODE“ (Zeilen mit Fehler rot hinterlegt, Marker `>`), „WARUM DAS EIN FEHLER IST“,
   „GEFIXTER CODE“ (geänderte Zeilen grün, Marker `+`), „DIFF“ (`-` rot, `+` grün, `report.show_diff`),
   „ERKLÄRUNG DER BEHEBUNG“ (+ Nebenwirkungen), „ERGEBNIS“ (Reviewer, Verifier, Re-Run, Iterationen),
   Versuchsliste. Nicht behobene Fehler: roter Statusbalken „NICHT BEHOBEN“, Grund, Code des letzten
   Versuchs als „Letzter Vorschlag (nicht übernommen)“.
4. **Gesamtanalyse des Modells** (`ai_analysis`, wie bisher als hellblaue Box).
5. **Anhang** (nur `report.include_raw_json_appendix`): der bisherige JSON-Dump inkl. Blöcke.

Technik:

* Hintergründe als Enum (`grau`, `blau`, `rot`, `grün`, `orange`) statt zweier Bools.
* Titelzeilen 12 pt fett (eigener Umbruch mit passender Spaltenzahl).
* Paginierung: Zeilen können zu „keep together“-Gruppen gehören (Überschrift + Codeblock, Kopf eines
  Abschnitts). Passt eine Gruppe ≤ 50 Zeilen nicht mehr auf die Seite, beginnt sie auf der nächsten;
  längere Gruppen werden normal getrennt. Jeder Fehler-Abschnitt beginnt auf einer neuen Seite, wenn
  weniger als ein Viertel der Seite frei ist.
* Lange Zeilen: `wrapInto()` mit leerem Zeilennummern-Gutter (Hintergrund läuft mit).
* Maskierung: wie bisher über `maskSecrets()`, zusätzlich `maskCodeLine()` für jede Codezeile
  (String-Literal nach `api_key`/`token`/`password`/`secret`/`authorization` … `=`/`:` → `"***"`).
* Sprache der festen Texte: `report.language` = `de` (Default) oder `en`.

## 5. Konfiguration

`.buggai.json` (bestehender Mechanismus, gleiche Datei wie `pdf_export`):

```json
{ "report": { "context_lines": 3, "show_diff": true, "include_raw_json_appendix": false, "language": "de" } }
```

Ungültige Werte behalten den Default (`context_lines` wird auf 0…20 begrenzt). Keine neuen CLI-Flags.

## 6. Fehlerrobustheit

* `FixReportBuilder`-Methoden fangen alle Ausnahmen, loggen `[REPORT] …` und machen weiter.
* Rendering: ein Fehler beim neuen Layout fällt auf den bisherigen JSON-Dump zurück (geloggt).

## 7. Tests

* Unit: `test_fix_snippet.cpp` (Kontext, Dateianfang/-ende, mehrere Hunks, Einfügen/Löschen verschiebt
  Nummern, Markierung alt/neu, Fehlerzeilen ohne Änderung, Limits), `test_fix_report.cpp` (Agent-Parsing
  inkl. fehlender/kaputter Felder, Review-Fallbacks, Maskierung von Code, Builder-Zählung,
  indirekt behobene Fehler, Config-Parsing, PDF-Rendering des neuen Layouts, Fallback ohne `fix_report`).
* Integration: `tests/run_report_test.sh` – C++-Projekt mit 3 echten Compilerfehlern (fehlendes Semikolon,
  nicht deklarierte Variable → Fix fügt eine Zeile ein, Typfehler → Verifier lehnt immer ab), Mock-LLM
  Szenario `report`. Prüft per `pdftotext` (oder `tests/helpers/pdf_text.py`): alle Abschnitte, Zahlen
  der Zusammenfassung, Markierungen, „NICHT BEHOBEN“ mit Grund, fehlende Erklärung, Maskierung, Anhang an/aus.
* Bestehende Tests: Prüfungen auf JSON-Inhalte im PDF laufen mit `include_raw_json_appendix: true` weiter.
* Beispiel: `make report-sample` kopiert den Report des Integrationstests nach `docs/examples/sample_report.pdf`.
