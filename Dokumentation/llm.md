# Sprint 3: LLM-Prototyp Konzept

**Projekt:** 2buggAI - KI-gestütztes Debugging-Tool  
**Sprint:** 3  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Scope-Anpassung

Parallel zur Pipeline-Definition wurde die LLM-Integration auf konzeptioneller Ebene vorbereitet. Die tatsächliche Implementation wurde auf Sprint 4 verschoben, um eine solide technische Grundlage zu schaffen.

---

## 2. API-Client-Design

### 2.1 Anforderungen

**Funktionalität:**
- HTTP POST-Requests an LLM-API-Endpunkt
- JSON-Request-Building
- JSON-Response-Parsing
- Authentifizierung (Bearer Token)
- Timeout-Handling

**Konfiguration:**
- API-Key aus Environment-Variable
- Base-URL konfigurierbar (ANTHROPIC_BASE_URL)
- Modell-Selection (Environment oder Default)

### 2.2 Klassenstruktur (konzeptionell)

```
ClaudeClient:
  - Konstruktor: Lädt Config aus Environment
  - debug_report(json_report): Hauptfunktion
  - Private: HTTP-Request-Logik
  
Datenstrukturen:
  ClaudeResult:
    - http_status: int
    - raw_json: string
    - text: string (extrahiert)
```

---

## 3. JSON-Payload-Format

### 3.1 Request

**Struktur:**
```json
{
  "model": "claude-3-opus",
  "instructions": "System-Prompt",
  "input": "User-Prompt mit JSON-Report",
  "max_output_tokens": 500
}
```

**Alternativen für verschiedene APIs:**
- Anthropic: direktes Format wie oben

### 3.2 Response

**Erwartete Strukturen:**

**Anthropic Claude:**
```json
{
  "content": [
    {"type": "text", "text": "..."}
  ]
}
```

**Parsing-Strategie:** Best-Effort, unterstützt beide Formate

---

## 4. Prompt-Template

### 4.1 System-Prompt (Instructions)

```
Du bist ein Senior Debugger. 
Analysiere Debug-Reports (GDB/Valgrind) und erkläre 
Fehler präzise und verständlich.

Gib:
(1) Wahrscheinlichste Ursache
(2) Belege aus Debug-Output
(3) Konkrete Fix-Schritte

Antwort auf Deutsch.
```

### 4.2 User-Prompt

```
Hier ist der Report als JSON:

{...vollständiger JSON-Report...}

Bitte analysieren und den Fehler erklären + Fix vorschlagen.
```

### 4.3 Output-Erwartung

```
FEHLERANALYSE:

1. FEHLERBESCHREIBUNG:
   [Kurze Zusammenfassung]

2. URSACHE:
   [Detaillierte Analyse]

3. BELEGE:
   - [Debug-Output-Referenz]
   - [Code-Stelle]

4. FIX-SCHRITTE:
   a) [Konkreter Schritt]
   b) [Konkreter Schritt]
```

---

## 5. Fehlerbehandlung

### 5.1 HTTP-Fehler

**Status-Codes:**
- 200-299: Success
- 401: Invalid API Key
- 429: Rate Limit Exceeded
- 500: API Server Error
- Timeout: Nach 60 Sekunden

**Handling:**
- Status-Check vor Parsing
- Error-Message-Extraktion aus Response
- User-freundliche Fehlermeldung

### 5.2 Parsing-Fehler

**Szenarien:**
- Ungültiges JSON
- Unerwartete Struktur
- Fehlende Felder

**Handling:**
- JSON-Parse mit Error-Check
- Fallback auf raw_json bei Fehler
- Warnung an User

---

## 6. LLM-Evaluierung

### 6.1 Anthropic Claude

**Status:** Favorit basierend auf Sprint-2-Recherche  
**Nächster Schritt:** Praktische Tests in Sprint 4 (Credits kaufen, API-Calls)

**Erwartete Vorteile:**
- Großes Context-Window (100K+ tokens)
- Gute Code-Analyse

---

## 7. Offene Implementierungsdetails

**HTTP-Library:**
- libcurl (C, weit verbreitet)
- cpp-httplib (C++ Header-only)
- Andere Optionen

**JSON-Library:**
- nlohmann/json (C++ Header-only, beliebt)
- RapidJSON
- Andere Optionen

**Text-Extraction:**
- Robustes Parsing verschiedener Response-Formate
- Fallback-Mechanismen

Diese Entscheidungen werden in Sprint 4 getroffen.

---

## 8. API-Parameter

**Empfohlene Werte:**
- `max_output_tokens`: 500 (ausreichend für detaillierte Analysen)
- `temperature`: 0.3 (deterministisch, fokussiert)
- `model`: Claude-3-Opus(beste Qualität)

**Begründung:**
- Niedrige Temperature für konsistente technische Antworten
- 500 Tokens ermöglichen ausführliche Erklärungen

---

## 9. Testplan

**Unit-Tests:**
- Environment-Variable-Loading
- JSON-Serialization/Deserialization
- Text-Extraction aus verschiedenen Response-Formaten

**Integration-Tests:**
- Echte API-Calls mit Test-Reports
- Error-Handling bei 401/429/500
- Timeout-Handling

**Validierung:**
- Qualität der LLM-Antworten subjektiv bewerten
- Verschiedene Fehlertypen testen

---

## 10. Ergebnis

Der grundlegende Aufbau des LLM-Prototyps wurde vorbereitet. Die Ausarbeitung umfasst API-Client-Design, JSON-Payload-Format-Definition, Klassen-Strukturierung und den Ablauf für die spätere Fehleranalyse. Es wurde festgelegt, wie Code und Fehlermeldungen verarbeitet und an das LLM übergeben werden sollen.

✓ API-Client-Design spezifiziert  
✓ JSON-Payload-Formate definiert  
✓ Prompt-Templates entwickelt  
✓ Error-Handling konzipiert  
✓ Environment-Konfiguration geplant  

Die technische Grundlage für die Implementation ist gelegt.
