# 🧠 KI-unterstützter Debugging-Prototyp

## 📘 Projektübersicht
Dieses Projekt entwickelt einen **Prototyp für KI-unterstützte Debugging-Sessions** in verschiedenen Programmiersprachen.  
Der Ansatz nutzt **Large Language Models (LLMs)** – darunter ChatGPT, Claude und Mistral – um Entwickler*innen bei der **Fehleranalyse, Fehlerbeschreibung und Lokalisierung von Bugs** zu unterstützen, ohne den ursprünglichen Code unnötig zu verändern.

Der Fokus liegt nicht auf der vollständigen Automatisierung des Debuggings, sondern auf einer **verständlichen, erklärenden Unterstützung** während des Entwicklungsprozesses.  
Zu den adressierten Fehlertypen gehören:

- 🧩 **Syntaktische Fehler**  
- 🧠 **Semantische und logische Fehler**  
- 🛠️ **Design- und Konzeptfehler**  
- 🎨 **UI-bezogene Probleme**  
- 🪲 **Heisenbugs** (schwer reproduzierbare Fehler)  
- 🧪 **Schrödinger-Bugs** (Fehler, die erst erkennbar werden, wenn man den Code liest)

---

## 🎯 Projektziele
- Entwicklung eines funktionalen Prototyps zur **KI-basierten Fehlerlokalisierung und Analyse**
- Kombination von LLMs mit klassischen Debugging-Tools (z. B. **Valgrind**, **gdb**)
- Automatisierte Erkennung und Beschreibung von Fehlern und Problemstellen im Code
- Bereitstellung von **erklärenden Fehlermeldungen** und Vorschlägen für mögliche Fixes
- Evaluation der Effektivität und Zuverlässigkeit von LLMs im Debugging-Kontext

---

## 🧩 Systemkomponenten
| Komponente | Beschreibung |
|-----------|--------------|
| **LLM-Schnittstelle** | Anbindung an ChatGPT, Claude und Mistral zur Fehleranalyse |
| **Debugging-Layer** | Integration von Tools wie Valgrind oder gdb |
| **Analyse-Pipeline** | Verarbeitung von Compiler-Fehlern, Logs, Stacktraces etc. |
| **Benutzeroberfläche (CLI/Web)** | Ausgabe der Erklärungen, Fehlerlokalisierung und Fix-Vorschläge |

---

## 🚀 Technologie-Stack
- **Programmiersprache:** C++ (Hauptimplementierung)
- **KI-Modelle:** OpenAI ChatGPT, Anthropic Claude, Mistral
- **Debugging-Tools:** Valgrind, gdb
- **Versionsverwaltung:** Git & GitHub
- **Dokumentation:** Markdown

---

## 🗓️ Sprint-Plan
| Sprint | Schwerpunkt | Ergebnis |
|--------|-------------|----------|
| **1** | Projektstart, Tool-Recherche, Setup | Repo + Dokumentationsstruktur |
| **2** | Architektur & Schnittstellenplanung | Systemkonzept |
| **3** | Prototyping I – LLM-Integration | Erster funktionsfähiger Prototyp |
| **4** | Prototyping II – komplexe Fehlertypen | Erweiterter Prototyp |
| **5** | Testing & Optimierung | Verbessertes System |
| **6** | Abschlussphase | Finaler Prototyp + Präsentation |

---

## 👥 Team
| Name |
|------|
| Nikola Cvetkovic |
| Krystian Piotr Kedzior |
| Berceste Yagmur Aslan |

---

## 📊 Aufwandsschätzung
- **Teamgröße:** 3 Personen  
- **Laufzeit:** 6 Sprints  
- **Gesamtaufwand:** ca. **225 Personenstunden**

---

## 🧠 Betreuer
**Prof. (FH) Dr. DI Mehnen**

---

## 💬 Kontakt
Bei Fragen oder Problemen gerne ein Issue im Repository eröffnen oder das Team über GitHub kontaktieren.

