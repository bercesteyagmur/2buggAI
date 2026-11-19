// Converts JSON files to PDF with the exporter that 2buggAI uses after every run.
//
//   build/json2pdf [--out <dir>] <file.json>...
//
// Writes <file>.pdf for every input and, for more than one input, <first>.combined.pdf.
// Without --out the PDFs are written next to the JSON files.
#include "PdfReportExporter.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    PdfExportConfig config;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) config.outputDir = argv[++i];
        else inputs.push_back(arg);
    }
    if (inputs.empty()) {
        std::cerr << "usage: " << argv[0] << " [--out <dir>] <file.json>...\n";
        return 2;
    }
    config.combined = inputs.size() > 1;

    PdfReportExporter exporter(config);
    int failed = 0;
    for (const auto& path : inputs) {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "cannot read " << path << "\n";
            ++failed;
            continue;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        if (!exporter.exportJson(buffer.str(), path)) ++failed;
    }
    if (config.combined && !exporter.finish()) ++failed;
    return failed == 0 ? 0 : 1;
}
