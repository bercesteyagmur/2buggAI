#include "ReportJson.h"
#include <nlohmann/json.hpp>

#include <string>
#include <algorithm>

using nlohmann::json;

// erste Zeile finden, die einen bestimmten Text enthält
static std::string first_line_containing(const std::string& text, const std::string& needle) {
    auto pos = text.find(needle);
    if (pos == std::string::npos) return "";

    auto start = text.rfind('\n', pos);
    start = (start == std::string::npos) ? 0 : start + 1;

    auto end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();

    return text.substr(start, end - start);
}

// Block zwischen zwei Markern ausschneiden 
static std::string extract_block(const std::string& text,
                                 const std::string& startNeedle,
                                 const std::string& endNeedle,
                                 size_t maxChars = 8000) {
    auto s = text.find(startNeedle);
    if (s == std::string::npos) return "";

    auto e = text.find(endNeedle, s + startNeedle.size());
    if (e == std::string::npos) e = text.size();

    std::string block = text.substr(s, e - s);
    if (block.size() > maxChars) block.resize(maxChars);
    return block;
}

// große Outputs für API kürzen 
static std::string truncate_middle(const std::string& s, size_t maxBytes = 200000) {
    if (s.size() <= maxBytes) return s;

    size_t keep = maxBytes / 2;
    return s.substr(0, keep) + "\n...<truncated>...\n" + s.substr(s.size() - keep);
}

std::string make_report_json(
    const std::string& targetPath,
    const std::string& fixDescription,
    bool recursive,
    bool verbose,
    const std::vector<std::string>& exts,
    const std::vector<std::string>& passthrough,
    const RunResult* gdb,
    const ValgrindResult* vg,
    const RunResult* plain_run, 
    const std::string& sourceCode
) {
    json j;

    j["meta"] = {
        {"format", "buggy-report-v1"}
    };

    j["config"] = {
        {"target_path", targetPath},
        {"fix_description", fixDescription},
        {"recursive", recursive},
        {"verbose", verbose},
        {"extensions", exts},
        {"passthrough_args", passthrough}
    };

    // Source Code
    if (!sourceCode.empty()) {
        // Kürze sehr große Dateien
        std::string content = sourceCode;
        size_t max_size = 50000;  // 50KB
        bool truncated = false;
        
        if (content.size() > max_size) {
            content = content.substr(0, max_size) + "\n...<source truncated>...";
            truncated = true;
        }
        
        j["source_code"] = {
            {"path", targetPath},
            {"content", content},
            {"size_bytes", sourceCode.size()},
            {"truncated", truncated}
        };
    } else {
        j["source_code"] = nullptr;
    }

    // GDB (optional) + Summary
    if (gdb) {
        // Signal/Stop-Reason Zeile (falls vorhanden)
        std::string signalLine = first_line_containing(gdb->output, "Program received signal");
        if (signalLine.empty()) {
            // fallback: irgendwas mit "SIG"
            signalLine = first_line_containing(gdb->output, "SIG");
        }

        // Output ggf. kürzen, damit API-Request nicht riesig wird
        const auto originalSize = gdb->output.size();
        std::string out = truncate_middle(gdb->output, 200000);

        j["gdb"] = {
            {"exit_code", gdb->exit_code},
            {"output", out},
            {"output_original_bytes", originalSize},
            {"output_truncated", out.size() != originalSize}
        };

        j["gdb_summary"] = {
            {"signal_line", signalLine}
        };
    } else {
        j["gdb"] = nullptr;
        j["gdb_summary"] = nullptr;
    }

    // Valgrind (optional) + Summary

    if (vg) {
        // Wichtige Valgrind-Zeilen/Blöcke extrahieren
        std::string errSummary = first_line_containing(vg->run.output, "ERROR SUMMARY:");
        std::string leakBlock  = extract_block(vg->run.output, "LEAK SUMMARY:", "ERROR SUMMARY:");

        // Output/XML ggf. kürzen
        const auto outOrig = vg->run.output.size();
        const auto xmlOrig = vg->xml.size();

        std::string out = truncate_middle(vg->run.output, 200000);
        std::string xml = truncate_middle(vg->xml, 300000);

        j["valgrind"] = {
            {"exit_code", vg->run.exit_code},
            {"output", out},
            {"output_original_bytes", outOrig},
            {"output_truncated", out.size() != outOrig},
            {"xml", xml},
            {"xml_original_bytes", xmlOrig},
            {"xml_truncated", xml.size() != xmlOrig}
        };

        j["valgrind_summary"] = {
            {"error_summary_line", errSummary},
            {"leak_summary_block", leakBlock}
        };
    } else {
        j["valgrind"] = nullptr;
        j["valgrind_summary"] = nullptr;
    }

        // Normaler Run (optional)
    if (plain_run) {
        const auto orig = plain_run->output.size();
        std::string out = truncate_middle(plain_run->output, 200000);

        j["run"] = {
            {"exit_code", plain_run->exit_code},
            {"output", out},
            {"output_original_bytes", orig},
            {"output_truncated", out.size() != orig}
        };
    } else {
        j["run"] = nullptr;
    }


    return j.dump(2);
}
