#include "ValgrindRunner.h"
#include "ShellQuote.h"
#include <fstream>
#include <unistd.h>     
#include <string>

static std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

ValgrindResult run_valgrind(const std::string& program, const std::vector<std::string>& args) {
    const std::string xmlFile = "valgrind_" + std::to_string(getpid()) + ".xml";

    std::string cmd =
        "valgrind "
        "--leak-check=full --show-leak-kinds=all --track-origins=yes "
        "--num-callers=30 "
        "--error-exitcode=42 "
        "--xml=yes --xml-file=" + ShellQuote::quote(xmlFile) + " " +
        ShellQuote::quote(program);

    for (const auto& a : args) cmd += " " + ShellQuote::quote(a);

    ValgrindResult vr;
    vr.run = run_capture(cmd);
    vr.xml = read_all(xmlFile);
    return vr;
}
