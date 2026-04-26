#include "ShellQuote.h"

namespace ShellQuote {

std::string quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);

    out += '\'';
    for (char c : s) {
        if (c == '\'') {
            // beendet quote, escaped, startet quote neu
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

std::string buildCommand(const std::string& program,
                         const std::vector<std::string>& args) {
    std::string cmd = quote(program);
    for (const auto& a : args) {
        cmd += " ";
        cmd += quote(a);
    }
    return cmd;
}

} 
