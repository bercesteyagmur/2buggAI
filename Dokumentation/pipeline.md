# Sprint 3: Pipeline-Definition

**Projekt:** 2buggAI - KI-gestütztes Debugging-Tool  
**Sprint:** 3  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Scope-Anpassung

Ursprünglich war die Implementation der Pipeline für Sprint 3 geplant (7h). Nach Evaluierung wurde entschieden, zunächst eine detaillierte Definition zu erstellen und die Implementation auf Sprint 4 zu verschieben.

---

## 2. Pipeline-Übersicht

Die Fehleranalyse-Pipeline umfasst den gesamten Datenfluss von der Codeeingabe bis zur LLM-Antwort.

### 2.1 Pipeline-Stufen

```
User Input
    ↓
Argument Parsing & Validation
    ↓
Source Code Reading
    ↓
Auto-Compilation (falls nötig)
    ↓
Debug-Tool Execution (GDB/Valgrind/Plain)
    ↓
Output Extraction & Summarization
    ↓
JSON Report
    ↓
LLM API Call
    ↓
Response Parsing & Formatting
    ↓
User Output
```

---

## 3. Stufen-Definitionen

### Stufe 1: Argument Parsing

**Input:** `argc`, `argv` (Command-line-Argumente)  
**Verarbeitung:**
- Flag-Erkennung (--gdb, --valgrind, --verbose, etc.)
- Pfad-Validierung (Existenzprüfung)
- Passthrough-Argument-Sammlung (nach `--`)

**Output:** Konfigurationsobjekt mit validierten Parametern

**Error-Handling:** Exception bei fehlenden/invaliden Argumenten

---

### Stufe 2: Source Code Reading

**Input:** Dateipfad  
**Verarbeitung:**
- Extension-Check (.c, .cpp, .h, .hpp)
- File-Reading (ifstream)
- Optional: Truncation bei großen Dateien (> 50KB)

**Output:** String mit Quellcode oder leer bei Binaries

---

### Stufe 3: Auto-Compilation

**Input:** Dateipfad  
**Verarbeitung:**
- Erkennung: Source vs. Binary
- Falls Source: Kompilierung mit gcc/g++
- Compiler-Flags: `-g` (Debug-Symbole), `-pthread`

**Command:**
```bash
g++ -g -pthread -o /tmp/buggy_compiled_<PID> <source_file>
```

**Output:** Pfad zum ausführbaren Binary

**Error-Handling:** Compiler-Errors werden ausgegeben, Prozess bricht ab

---

### Stufe 4: Debug-Tool Execution

**GDB Execution:**
```bash
gdb -q --batch "
        "-ex \"set pagination off\" "
        "-ex \"set confirm off\" "
        "-ex \"set print pretty on\" "
        "-ex \"set print frame-arguments all\" "
        "-ex \"set print elements 200\" "
        "-ex \"echo \\n===BUGGY_GDB:RUN===\\n\" "
        "-ex \"run\" "
        "-ex \"echo \\n===BUGGY_GDB:INFO_PROGRAM===\\n\" "
        "-ex \"info program\" "
        "-ex \"echo \\n===BUGGY_GDB:THREADS===\\n\" "
        "-ex \"info threads\" "
        "-ex \"echo \\n===BUGGY_GDB:BT_ALL===\\n\" "
        "-ex \"thread apply all bt full\" "
        "-ex \"echo \\n===BUGGY_GDB:FRAME0===\\n\" "
        "-ex \"frame 0\" "
        "-ex \"info args\" "
        "-ex \"info locals\" "
        "-ex \"echo \\n===BUGGY_GDB:REGS===\\n\" "
        "-ex \"info registers\" "
        "--args " + program;
```

**Valgrind Execution:**
```bash
"valgrind "
        "--leak-check=full --show-leak-kinds=all --track-origins=yes "
        "--num-callers=30 "
        "--error-exitcode=42 "
        "--xml=yes --xml-file=" + " " +
        program;
```

**Output:** Strukturierte Ergebnisse (exit_code, output, optional XML)

---

### Stufe 5: Output Extraction

**Aufgabe:** Extraktion relevanter Informationen aus Tool-Outputs

**GDB:**
- Signal-Line (SIGSEGV, SIGABRT)
- Backtrace
- Variable-Values

**Valgrind:**
- Error-Summary-Line
- Leak-Summary-Block
- XML-Struktur

**Methoden:** String-Matching, Pattern-Recognition

---

### Stufe 6: JSON Report Assembly

**Struktur:**
```json
{
  "meta": {"format": "buggy-report-v1"},
  "config": {
    "target_path": "...",
    "fix_description": "..."
  },
  "source_code": {
    "content": "...",
    "truncated": false
  },
  "gdb": {
    "exit_code": 139,
    "output": "..."
  },
  "gdb_summary": {
    "signal_line": "..."
  },
  "valgrind": {
    "output": "...",
    "xml": "..."
  },
  "valgrind_summary": {
    "error_summary_line": "...",
    "leak_summary_block": "..."
  }
}
```

**Truncation:** Outputs > 200KB werden gekürzt (middle-truncation)

---

### Stufe 7: LLM API Call

**Request-Aufbau:**
```json
{
  "model": "<model_name>",
  "instructions": "Du bist ein Senior Debugger...",
  "input": "Hier ist der Report: [JSON]",
  "max_output_tokens": 500
}
```

**HTTP:** POST-Request an LLM-API-Endpunkt  
**Timeout:** 60 Sekunden  
**Authentication:** Bearer Token (Environment-Variable)

---

### Stufe 8: Response Parsing

**Aufgabe:** Extraktion des Analyse-Texts aus API-Response

**Strategie:** Best-Effort-Parsing verschiedener Response-Formate

**Error-Handling:**
- HTTP-Status-Check (200-299 = Success)
- JSON-Parsing mit Fallback
- Text-Extraction aus verschiedenen Strukturen

**Output:** Formatierter Analyse-Text

---

## 4. Fehlerbehandlung

### 4.1 Compilation-Fehler
**Symptom:** gcc/g++ exit_code != 0  
**Handling:** Compiler-Output anzeigen, Tool beenden

### 4.2 Tool-Ausführungsfehler
**Symptom:** GDB/Valgrind nicht verfügbar  
**Handling:** Warnung ausgeben, Schritt überspringen

### 4.3 Timeout
**Symptom:** Programm hängt  
**Handling:** Prozess nach X Sekunden terminieren

### 4.4 API-Fehler
**Symptom:** HTTP 4xx/5xx  
**Handling:** Status und Error-Message anzeigen

---

## 5. Offene Implementierungsdetails

**Parsing:**
- Konkrete Regex/String-Matching-Strategien
- Handling verschiedener GDB/Valgrind-Versionen

**HTTP-Kommunikation:**
- Library-Auswahl (curl, httplib, etc.)
- Request-Builder-Implementation

**Code-Struktur:**
- Klassendesign
- Modularisierung
- Testbarkeit

Diese Details werden in Sprint 4 während der Implementation geklärt.

---

## 6. Datenfluss-Beispiel

**Erfolgreicher Durchlauf:**
```
User: ./buggy --gdb crash.cpp "Fix crash"
  → Parse: targetPath=crash.cpp, useGdb=true
  → Read: sourceCode="int main() {...}"
  → Compile: /tmp/buggy_compiled_12345
  → GDB: exitCode=139, output="SIGSEGV..."
  → Extract: signalLine="SIGSEGV at line 10"
  → JSON: {gdb: {...}, source_code: {...}}
  → API: HTTP 200, response={...}
  → Parse: "FEHLERANALYSE: Null-Pointer..."
  → Output: [Analyse für User]
```
---

## 7. Ergebnis

Die Pipeline für den Fehleranalyseprozess wurde definiert. Der Weg von der Codeeingabe über die Fehlerextraktion bis zur LLM-Übergabe wurde spezifiziert, ebenso die Verarbeitung der LLM-Antworten. Die Fehlertypen aus Sprint 2 wurden integriert.

✓ 9 Pipeline-Stufen definiert  
✓ Datenfluss spezifiziert  
✓ Fehlerbehandlung konzipiert  
✓ JSON-Formate festgelegt  

Der Prozess ist klar strukturiert und bereit für die Implementation in Sprint 4.

---

