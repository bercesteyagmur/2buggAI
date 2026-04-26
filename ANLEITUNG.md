# Buggy - Installations- und Nutzungsanleitung

## Übersicht

**Buggy** ist ein automatisches Debugging-Tool für C/C++-Programme. Es verwendet GDB und Valgrind zur Fehleranalyse und nutzt OpenAI für intelligente Lösungsvorschläge.

---

## Installation Schritt für Schritt

### 1. System vorbereiten (Ubuntu/Debian/WSL)

```bash
# System aktualisieren
sudo apt update && sudo apt upgrade -y

# Alle benötigten Pakete installieren
sudo apt install -y \
    build-essential \
    gdb \
    valgrind \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    git
```

**Was wird installiert:**
- `build-essential` - g++, gcc, make
- `gdb` - GNU Debugger
- `valgrind` - Memory Debugger
- `libcurl4-openssl-dev` - HTTP-Library
- `nlohmann-json3-dev` - JSON-Parser

### 2. Installation überprüfen

```bash
# Alle Tools prüfen
g++ --version          # Sollte Version 9+ zeigen
gdb --version          # GNU gdb
valgrind --version     # valgrind-3.x
curl-config --version  # libcurl
```

Falls `nlohmann-json3-dev` nicht verfügbar ist (ältere Ubuntu-Versionen):

```bash
git clone https://github.com/nlohmann/json.git /tmp/json
sudo cp -r /tmp/json/include/nlohmann /usr/local/include/
rm -rf /tmp/json
```

### 3. OpenAI API-Key einrichten

1. Erstellen Sie einen Account auf [platform.openai.com](https://platform.openai.com)
2. Generieren Sie einen API-Key unter "API Keys"
3. Setzen Sie den Key als Environment Variable:

```bash
# Permanent in .bashrc speichern
echo 'export OPENAI_API_KEY="sk-proj-IHR_KEY_HIER"' >> ~/.bashrc
source ~/.bashrc

# Prüfen
echo $OPENAI_API_KEY
```

**Optional:** Modell und URL anpassen:
```bash
export OPENAI_MODEL="gpt-4"
export OPENAI_BASE_URL="https://api.openai.com"
```

---

## Projekt kompilieren

### 1. Projekt herunterladen

```bash
# Repository klonen
git clone https://github.com/Dirk1306/2buggAI.git
cd 2buggAI

# Oder: Dateien manuell kopieren
```

### 2. Projektstruktur prüfen

```bash
ls -la
# Sollte enthalten:
# - *.cpp Dateien (main.cpp, argumentParser.cpp, etc.)
# - Makefile
# - includes/ Verzeichnis mit *.h Dateien
```

### 3. Include-Pfade anpassen

Die Source-Dateien enthalten absolute Pfade. Diese müssen angepasst werden:

```bash
# Automatisch alle Includes korrigieren
sed -i 's|#include "/home/krystian/projects/2buggAI/2_buggy_AI/includes/|#include "|g' *.cpp
```

### 4. Kompilieren

```bash
# Projekt bauen
make

# Bei Erfolg wird ./buggy erstellt
ls -lh buggy

# Test: Hilfe anzeigen
./buggy --help
```

### 5. Aufräumen und Neubauen

```bash
# Alles löschen und neu bauen
make clean
make
```

**Häufige Fehler:**

| Fehler | Lösung |
|--------|--------|
| `nlohmann/json.hpp not found` | `sudo apt install nlohmann-json3-dev` |
| `undefined reference to curl_*` | `sudo apt install libcurl4-openssl-dev` |
| `C++20 standard required` | `sudo apt install g++-10` oder neuer |

---

## Verwendung

### Basis-Syntax

```bash
./buggy <pfad> <fix-beschreibung> [optionen]
```

### Wichtigste Optionen

| Option | Beschreibung | Beispiel |
|--------|--------------|----------|
| `--gdb` | GDB für Crash-Analyse | `./buggy test.c "Crash" --gdb` |
| `--valgrind` | Valgrind für Memory-Leaks | `./buggy test.c "Leak" --valgrind` |
| `--verbose` | Detaillierte Ausgaben | `./buggy test.c "Debug" -v` |
| `--json-out <file>` | JSON-Report speichern | `./buggy test.c "Bug" --json-out report.json` |
| `--` | Argumente an Programm | `./buggy server.c "Test" --gdb -- --port 8080` |
| `--help` | Hilfe anzeigen | `./buggy --help` |

### Workflow

1. **Automatische Kompilierung:** Buggy kompiliert C/C++-Dateien automatisch mit `-g`
2. **Ausführung mit Tools:** GDB/Valgrind sammelt Debug-Informationen
3. **JSON-Report:** Alle Daten werden strukturiert zusammengestellt
4. **OpenAI-Analyse:** KI analysiert Fehler und gibt Lösungsvorschläge

---

## Erweiterte Nutzung

### Makefile anpassen

```makefile
# Debugging-Build mit AddressSanitizer
CXXFLAGS := -std=c++20 -O0 -g -fsanitize=address -Wall -Wextra -Iincludes
LDLIBS   := -lcurl -lpthread

# Dann kompilieren
make clean
make
```

## Schnellreferenz

```bash
# Installation (einmalig)
sudo apt install -y build-essential gdb valgrind libcurl4-openssl-dev nlohmann-json3-dev
export OPENAI_API_KEY="sk-proj-..."

# Projekt bauen
make

# Verwendung
./buggy <file.c> "Beschreibung" --gdb           # Crash analysieren
./buggy <file.c> "Beschreibung" --valgrind      # Memory Leaks
./buggy <file.c> "Beschreibung" --gdb --valgrind --json-out report.json  # Vollständig

# Mit Programm-Argumenten
./buggy <file.c> "Beschreibung" --gdb -- <args>

# Hilfe
./buggy --help
```

---

## Fehlerbehebung

| Problem | Lösung |
|---------|--------|
| `OPENAI_API_KEY not set` | `export OPENAI_API_KEY="sk-..."` in .bashrc |
| `OpenAI HTTP 401` | API-Key prüfen oder neu generieren |
| `nlohmann/json.hpp not found` | `sudo apt install nlohmann-json3-dev` |
| `make: command not found` | `sudo apt install build-essential` |
| Kompilierung schlägt fehl | Includes anpassen: `sed -i 's|#include "/home/.*includes/|#include "|g' *.cpp` |
| Valgrind zu langsam | Nur GDB verwenden: `--gdb` ohne `--valgrind` |

**Debugging von Buggy selbst:**
```bash
# Mit Verbose-Modus mehr Infos bekommen
./buggy test.c "Test" --verbose --gdb

# Buggy mit GDB debuggen
gdb ./buggy
(gdb) run test.c "Test" --gdb
```

---
