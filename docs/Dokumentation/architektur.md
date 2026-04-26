# Sprint 2: Architekturentwurf

**Projekt:** 2buggAI - KI-gestütztes Debugging-Tool  
**Sprint:** 2  
**Team:** Nikola Cvetkovic, Krystian Piotr Kedzior  

---

## 1. Zielsetzung

Ausarbeitung eines groben Architekturentwurfs für das Debugging-Tool. Die Systemarchitektur sollte folgende Komponenten umfassen:
- Frontend/CLI zur Analyse von Code und Debug-Logs
- Backend-Service zur Verarbeitung
- LLM-Schicht für automatische Fehleranalyse
- Integration von Debugging-Tools (GDB, Valgrind)

---

## 2. Team-Situation

Aufgrund von Team-Ausfällen wurde die Gruppengröße von 4 auf 2 Personen reduziert. Dies führte zu einer Anpassung des Projektumfangs, wobei alle notwendigen Komponenten dennoch definiert werden konnten.

---

## 3. Systemarchitektur

### 3.1 Vereinfachter Architekturentwurf

Das System folgt einem dreischichtigen Aufbau:

```
┌────────────────────────────────┐
│     Frontend (CLI-Tool)         │
│  - Entgegennahme von Parametern │
│  - Ausgabe von Ergebnissen      │
└───────────┬────────────────────┘
            │
            ↓
┌────────────────────────────────┐
│     Backend-Service             │
│  - Code-Kompilierung            │
│  - Tool-Ausführung (GDB/Valg.)  │
│  - Datensammlung                │
│  - JSON-Report-Erstellung       │
└───────────┬────────────────────┘
            │
            ↓
┌────────────────────────────────┐
│       LLM-Integration           │
│  - Prompt-Generierung           │
│  - API-Kommunikation            │
│  - Antwort-Verarbeitung         │
└────────────────────────────────┘
```

### 3.2 Komponenten-Beschreibung

**Frontend (CLI):**
- Kommandozeilen-Interface für Benutzerinteraktion
- Entgegennahme von Pfaden, Flags und Beschreibungen
- Ausgabe der Analyseergebnisse

**Backend:**
- Automatische Kompilierung von Quellcode mit Debug-Symbolen
- Ausführung von GDB und/oder Valgrind
- Sammlung und Strukturierung der Debug-Outputs
- Erstellung strukturierter JSON-Reports

**LLM-Schicht:**
- Aufbereitung der Debug-Daten für LLM-Input
- API-Kommunikation mit gewähltem LLM-Anbieter
- Extraktion und Formatierung der Analyseergebnisse

---

## 4. LLM-Evaluierung

### 4.1 Recherche-Kriterien

Folgende Kriterien wurden bei der Evaluierung verschiedener LLM-Anbieter berücksichtigt:
- Code-Verständnis und Debugging-Fähigkeiten
- Context-Window-Größe (für umfangreiche Debug-Logs)
- API-Verfügbarkeit und Dokumentationsqualität
- Pricing-Modell und Kosteneffizienz

### 4.2 Verglichene Anbieter

**Anthropic Claude:**
- Context-Window: 100K+ tokens
- Stärken: Hervorragendes Code-Verständnis, strukturierte Outputs
- Schwächen: Höhere Kosten, Credits-basiertes System
- Bewertung: Favorit für Code-Analyse

**OpenAI GPT:**
- Context-Window: 32K-128K tokens (modellabhängig)
- Stärken: Gut dokumentierte API, etabliertes Pricing
- Schwächen: Kleineres Context-Window als Claude
- Bewertung: Solide Backup-Option

**Mistral AI:**
- Context-Window: 32K tokens
- Stärken: Günstigeres Pricing, Open-Source-Optionen
- Schwächen: Weniger etabliert für Debugging-Anwendungen
- Bewertung: Interessant für zukünftige Erweiterungen

### 4.3 Entscheidung

Basierend auf der Recherche wurde Anthropic Claude als primäre Option identifiziert, hauptsächlich aufgrund der überlegenen Code-Analyse-Fähigkeiten und des großen Context-Windows. OpenAI GPT wurde als Backup-Lösung eingeplant.

---

## 5. Debugging-Tools Integration

### 5.1 GDB (GNU Debugger)

**Funktionalität:**
- Crash-Analyse (SIGSEGV, SIGABRT, etc.)
- Backtrace-Generierung
- Variable-Inspektion
- Thread-Informationen

**Integration:**
Verwendung im Batch-Mode zur automatisierten Ausführung:
```bash
gdb --batch -ex "run" -ex "bt" -ex "info locals" <program>
```

**Herausforderungen:**
- Output-Parsing (textbasiert, nicht vollständig standardisiert)
- Versionsunterschiede zwischen GDB-Installationen

### 5.2 Valgrind

**Funktionalität:**
- Memory-Leak-Detection
- Invalid Memory Access
- Use-After-Free Erkennung
- Uninitialized Memory Detection

**Integration:**
Verwendung mit XML-Output für strukturierte Verarbeitung:
```bash
valgrind --leak-check=full --xml=yes --xml-file=output.xml <program>
```

**Herausforderungen:**
- Performance-Overhead bei Programmausführung
- Plattform-Abhängigkeit (primär Linux)

---

## 6. Datenfluss

Der geplante Datenfluss durch das System:

1. **Input:** User übergibt Programm (Source oder Binary) mit Fehlerbeschreibung
2. **Kompilierung:** Falls Source-Code, automatische Kompilierung mit `-g` Flag
3. **Tool-Ausführung:** GDB/Valgrind laufen lassen, Output sammeln
4. **Report-Assembly:** Strukturierung aller Daten in JSON-Format
5. **LLM-Anfrage:** POST-Request an LLM-API mit aufbereitetem Report
6. **Output:** Strukturierte Fehleranalyse mit Ursache und Fix-Vorschlägen

---

## 7. Scope-Anpassungen

Aufgrund der reduzierten Teamgröße wurden folgende Vereinfachungen vorgenommen:

**Nicht im MVP enthalten:**
- Web-basiertes Interface
- Datenbank-Integration
- User-Management-System
- IDE-Plugins
- REST-API-Endpunkt

**Fokus auf:**
- CLI-basiertes Tool
- Direkte LLM-API-Integration
- Single-User-Betrieb
- Einzeldatei-Analyse

---

## 8. Technologie-Stack (vorläufig)

**Sprache:** C++ (tendiert, noch nicht final)
- Vorteile: Native Integration mit Debug-Tools, Performance
- Nachteile: Höhere Komplexität, längere Entwicklungszeit

**Datenformat:** JSON
- Für interne Kommunikation und LLM-Input
- Gute Library-Unterstützung, LLM-freundlich

**HTTP-Kommunikation:** Noch offen (curl, andere Libraries)

---

## 9. Offene Fragestellungen

**Technisch:**
- Konkrete Implementierung des GDB/Valgrind Output-Parsings
- Auswahl der HTTP-Library für API-Kommunikation
- Code-Strukturierung (Klassendesign, Modularisierung)

**Funktional:**
- Umgang mit Multi-File-Projekten (zunächst ausgeschlossen)
- Handling von Timeouts bei hängenden Programmen
- Truncation-Strategie für große Debug-Outputs

**Organisatorisch:**
- Finale LLM-Wahl nach praktischen Tests in Sprint 3/4
- Ressourcen-Allokation für 2-Personen-Team

---

## 10. Ergebnis

Der Architekturentwurf wurde erfolgreich erstellt. Trotz Reduktion des Projektumfangs aufgrund von Team-Ausfällen konnten alle notwendigen Komponenten definiert werden:

✓ Dreischichtige Systemarchitektur spezifiziert  
✓ LLM-Optionen evaluiert (Claude favorisiert)  
✓ Debugging-Tool-Integration konzipiert  
✓ Datenfluss definiert  
✓ Realistischer Scope festgelegt  

---

