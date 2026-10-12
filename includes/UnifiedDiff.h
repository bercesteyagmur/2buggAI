#pragma once
#include <string>

// Einfacher zeilenbasierter Unified Diff (alt -> neu).
// Liefert einen leeren String, wenn sich die Zeilen nicht unterscheiden.
// Zeilenenden (\r\n vs. \n) werden beim Vergleich ignoriert.
std::string unified_diff(const std::string& old_text,
                         const std::string& new_text,
                         const std::string& path,
                         int context = 3);
