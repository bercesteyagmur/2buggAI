# Sprint 2: Kernfunktionalitäten und Anforderungsdefinition

**Projekt:** 2buggAI - KI-gestütztes Debugging-Tool  
**Sprint:** 2  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Zielsetzung

Definition der Kernfunktionalitäten des Systems, Eingrenzung relevanter Fehlertypen für den Prototyp und Festlegung grundlegender Anforderungen an die LLM-Integration.

---

## 2. Kernfunktionalitäten

### 2.1 Eingabeverarbeitung

**Funktionalität:**
Entgegennahme und Validierung von Benutzereingaben über CLI-Interface.

**Eingaben:**
- Dateipfad (Quellcode oder kompiliertes Binary)
- Fehlerbeschreibung (User-Beschreibung des Problems)
- Optional: Flags für Tool-Auswahl (--gdb, --valgrind)
- Optional: Programmargumente (nach `--`)

**Beispiel:**
```bash
./buggy --gdb program.cpp "Fix segmentation fault"
./buggy --valgrind server -- --port 8080
```

### 2.2 Automatische Kompilierung

**Funktionalität:**
Automatische Erkennung von Quellcode und Kompilierung mit Debug-Symbolen.

**Unterstützte Dateitypen:**
- `.c` → gcc
- `.cpp`, `.cc` → g++

**Compiler-Flags:**
- `-g` : Debug-Symbole für GDB
- `-pthread` : Thread-Support

**Error-Handling:**
Kompilierungsfehler werden dem User angezeigt, Tool wird beendet.

### 2.3 Debug-Tool-Ausführung

**GDB-Integration:**
- Batch-Mode-Ausführung
- Crash-Analyse und Backtrace-Generierung
- Variable- und Register-Inspektion

**Valgrind-Integration:**
- Memory-Leak-Detection
- Invalid Memory Access Detection
- XML-Output für strukturiertes Parsing

**Direkter Run (Fallback):**
- Ausführung ohne Debug-Tools
- Output-Capture (stdout/stderr)
- Timeout-Mechanismus (Deadlock-Prevention)

### 2.4 Datensammlung und Strukturierung

**Zu sammelnde Daten:**
- Quellcode (falls verfügbar)
- GDB-Output (Signals, Backtrace, Variablen)
- Valgrind-Output (Errors, Leaks)
- Exit-Codes
- User-Beschreibung

**Ausgabeformat:**
JSON-Struktur mit kompakten Code- und Debug-Daten als Input für LLM.

**Truncation-Strategie:**
Große Outputs werden gekürzt um API-Token-Limits einzuhalten.

### 2.5 LLM-basierte Analyse

**LLM-Input:**
- Strukturierter JSON-Report
- System-Prompt (Rolle: Debugging-Experte)
- User-Prompt (Analyseanforderung)

**Erwarteter LLM-Output:**
- Fehlerbeschreibung (Was ist passiert?)
- Vermutete Ursache (Warum ist es passiert?)
- Belege aus Debug-Output
- Konkrete Fix-Schritte

---

## 3. Fehlertypen-Eingrenzung

### 3.1 Priorität 1: Speicherfehler

**Kategorie:** Memory Errors  
**Begründung:** Hohe Häufigkeit, gute Tool-Unterstützung, klare LLM-Analysierbarkeit

**Typen:**
- **Null-Pointer-Dereference**
  - Detection: GDB (SIGSEGV)
  - Häufigkeit: Sehr hoch
  
- **Memory Leaks**
  - Detection: Valgrind
  - Häufigkeit: Sehr hoch
  
- **Use-After-Free**
  - Detection: Valgrind
  - Häufigkeit: Hoch
  
- **Buffer Overflow**
  - Detection: Valgrind/GDB
  - Häufigkeit: Mittel

### 3.2 Priorität 2: Einfache Logikfehler

**Kategorie:** Logic Errors  
**Begründung:** Häufig, aber schwerer automatisch zu erkennen

**Typen:**
- Off-by-One Errors (Schleifenfehler)
- Falsche Bedingungen (= statt ==)
- Integer Overflow

**Einschränkung:**
Ohne Expected Output schwer zu validieren, daher sekundäre Priorität.

### 3.3 Ausgeschlossen: Race Conditions

**Kategorie:** Concurrency Errors  
**Begründung:** Nicht-deterministisch, erfordert spezialisierte Tools (Helgrind, ThreadSanitizer)

**Entscheidung:**
Nicht im MVP-Scope, möglicherweise zukünftige Erweiterung.

---

## 4. LLM-Integration: Anforderungen

### 4.1 Input-Format

**Struktur:**
```json
{
  "source_code": "...",
  "gdb_output": "...",
  "valgrind_output": "...",
  "user_description": "..."
}
```

**Anforderungen:**
- Kompakte Darstellung (Token-Limit-Compliance)
- Strukturierte Daten (JSON)
- Kontextreiche Information (Code + Debug-Output)

### 4.2 Output-Format

**Erwartete Struktur:**
```
FEHLERANALYSE:

1. FEHLERBESCHREIBUNG:
   [Zusammenfassung]

2. URSACHE:
   [Root-Cause-Analyse]

3. BELEGE:
   - [Debug-Output-Zitat]
   - [Code-Stelle]

4. FIX-SCHRITTE:
   a) [Konkreter Schritt]
   b) [Konkreter Schritt]
```

**Qualitätsanforderungen:**
- Technisch präzise
- Konkrete Code-Referenzen
- Umsetzbare Fix-Vorschläge
- Deutsche Ausgabe

### 4.3 Prompt-Strategien

Verschiedene Ansätze wurden evaluiert:
- Strukturiertes Output-Forcing
- Few-Shot-Examples
- Chain-of-Thought-Reasoning

Finale Entscheidung wird in Sprint 3/4 nach praktischen Tests getroffen.

---

## 5. Nicht-Funktionale Anforderungen

### 5.1 Usability

**CLI-Anforderungen:**
- Intuitive Befehle
- `--help` Dokumentation
- Verständliche Fehlermeldungen
- Minimale Konfiguration nötig

### 5.2 Zuverlässigkeit

**Error-Handling:**
- Graceful Degradation bei Tool-Fehlern
- Timeout-Mechanismen
- Validierung aller Eingaben

---

## 6. Use-Cases

### Use-Case 1: Segmentation Fault

```
Eingabe: C++-Programm mit Crash
Erwartung: Identifikation von Null-Pointer-Dereference
Output: Zeile und Ursache, Fix-Vorschlag
```

### Use-Case 2: Memory Leak

```
Eingabe: Programm mit Speicherleck
Erwartung: Identifikation fehlender Deallokation
Output: Leak-Größe und -Stelle, delete-Vorschlag
```

### Use-Case 3: Use-After-Free

```
Eingabe: Programm mit Use-After-Free
Erwartung: Identifikation invalider Speicherzugriffe
Output: Problemstelle, Pointer-Handling-Empfehlung
```

---

## 7. Datenmodelle (konzeptionell)

**RunResult:**
```
- exit_code: int
- output: string
```

**ValgrindResult:**
```
- run: RunResult
- xml: string
```

**AnalysisReport:**
```
- program_path: string
- source_code: string
- gdb_output: string (optional)
- valgrind_output: string (optional)
- llm_analysis: string
```

Detaillierte Implementierung wird in Sprint 3/4 spezifiziert.

---

## 8. Abgrenzung

### Nicht im Scope

**Ausgeschlossen für MVP:**
- Multi-File-Projekte
- Project-weite Analyse
- IDE-Integration
- Automatisches Code-Fixing
- Verlaufs-/History-Funktion
- Performance-Profiling
- Security-Vulnerability-Scanning

### Fokus

**Kern-Scope:**
- Einzeldatei-Analyse
- Memory Error Detection
- LLM-gestützte Erklärungen
- CLI-basiertes Interface

---

## 9. Offene Punkte

**Technische Details:**
- Output-Parsing-Strategien (Regex vs. String-Matching)
- HTTP-Library-Auswahl
- JSON-Library-Auswahl
- Code-Strukturierung

**Funktionale Details:**
- LLM-Wahl (Claude vs. GPT - praktische Tests nötig)
- Prompt-Template-Finalisierung
- Truncation-Limits
- Output-Formatierung

**Testing:**
- Testfall-Definition
- Validierungskriterien
- Qualitätsmetriken

Diese Punkte werden in Sprint 3 adressiert.

---

## 10. Ergebnis

Die Kernfunktionalitäten wurden erfolgreich definiert. Das System umfasst fünf Hauptfunktionen, priorisiert Speicherfehler und einfache Logikfehler, und spezifiziert grundlegende Anforderungen an die LLM-Integration.

✓ Eingaben definiert (Quellcode, Fehlermeldungen, Debug-Output)  
✓ Ausgaben spezifiziert (Fehlerbeschreibung, Ursache, Code-Stellen)  
✓ Fehlertypen eingegrenzt (Fokus auf Memory Errors)  
✓ LLM-Input/Output-Anforderungen festgelegt  

Die definierten Anforderungen bilden die Basis für den Sprint-3-Prototyp.