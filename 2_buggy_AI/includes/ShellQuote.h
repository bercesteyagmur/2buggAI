#pragma once
#include <string>
#include <vector>

namespace ShellQuote {
    // quoted ein einzelnes Argument für /bin/sh (Single-Quote sicher)
    std::string quote(const std::string& s);

    // baut: '<program>' '<arg1>' '<arg2>' ...
    std::string buildCommand(const std::string& program,
                             const std::vector<std::string>& args);
}
