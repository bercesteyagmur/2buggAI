#pragma once
#include <string>
#include <vector>

// Einfacher zeilenbasierter Unified Diff (alt -> neu).
// Liefert einen leeren String, wenn sich die Zeilen nicht unterscheiden.
// Zeilenenden (\r\n vs. \n) werden beim Vergleich ignoriert.
std::string unified_diff(const std::string& old_text,
                         const std::string& new_text,
                         const std::string& path,
                         int context = 3);

// Eine Operation des Zeilendiffs. Jede Operation trägt die Position in alt UND neu, so
// dass sich Zeilennummern auch nach eingefügten oder gelöschten Zeilen zuordnen lassen.
struct DiffOp {
    char type;   // ' ' gleich, '-' entfernt, '+' hinzugefügt
    size_t a;    // 0-basierter Zeilenindex in alt (für ' ' und '-'; bei '+' die Einfügeposition)
    size_t b;    // 0-basierter Zeilenindex in neu (für ' ' und '+'; bei '-' die Position in neu)
};

// Zeilen eines Textes wie sie der Diff sieht (ohne \r, eine abschließende Leerzeile zählt nicht).
std::vector<std::string> diff_split_lines(const std::string& text);

// Der Zeilendiff (LCS), auf dem unified_diff beruht.
std::vector<DiffOp> diff_lines(const std::vector<std::string>& old_lines,
                               const std::vector<std::string>& new_lines);
