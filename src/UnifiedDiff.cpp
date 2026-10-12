#include "UnifiedDiff.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace {

// Oberhalb dieser Größe (Zeilen alt x Zeilen neu im geänderten Mittelteil)
// wird kein LCS mehr berechnet, sondern der Block komplett ersetzt.
constexpr size_t MAX_LCS_CELLS = 4'000'000;

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

struct Op {
    char type;   // ' ' gleich, '-' entfernt, '+' hinzugefügt
    size_t a;    // Zeilenindex in alt (für ' ' und '-')
    size_t b;    // Zeilenindex in neu (für ' ' und '+')
};

std::vector<Op> diffOps(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;

    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }

    std::vector<Op> ops;
    for (size_t i = 0; i < prefix; ++i) ops.push_back({' ', i, i});

    const size_t n = a.size() - prefix - suffix;
    const size_t m = b.size() - prefix - suffix;

    if (n * m > MAX_LCS_CELLS) {
        for (size_t i = 0; i < n; ++i) ops.push_back({'-', prefix + i, prefix});
        for (size_t j = 0; j < m; ++j) ops.push_back({'+', prefix + n, prefix + j});
    } else {
        // LCS-Tabelle über den Mittelteil: lcs[i][j] = LCS von a[i..] und b[j..]
        std::vector<std::vector<unsigned>> lcs(n + 1, std::vector<unsigned>(m + 1, 0));
        for (size_t i = n; i-- > 0;) {
            for (size_t j = m; j-- > 0;) {
                lcs[i][j] = (a[prefix + i] == b[prefix + j])
                    ? lcs[i + 1][j + 1] + 1
                    : std::max(lcs[i + 1][j], lcs[i][j + 1]);
            }
        }

        size_t i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[prefix + i] == b[prefix + j]) {
                ops.push_back({' ', prefix + i, prefix + j});
                ++i; ++j;
            } else if (i < n && (j == m || lcs[i + 1][j] >= lcs[i][j + 1])) {
                ops.push_back({'-', prefix + i, prefix + j});
                ++i;
            } else {
                ops.push_back({'+', prefix + i, prefix + j});
                ++j;
            }
        }
    }

    for (size_t k = 0; k < suffix; ++k) {
        ops.push_back({' ', a.size() - suffix + k, b.size() - suffix + k});
    }
    return ops;
}

} // namespace

std::string unified_diff(const std::string& old_text,
                         const std::string& new_text,
                         const std::string& path,
                         int context) {
    const std::vector<std::string> a = splitLines(old_text);
    const std::vector<std::string> b = splitLines(new_text);
    const std::vector<Op> ops = diffOps(a, b);
    const size_t ctx = static_cast<size_t>(std::max(context, 0));

    std::vector<size_t> changes;
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].type != ' ') changes.push_back(k);
    if (changes.empty()) return "";

    std::ostringstream out;
    out << "--- a/" << path << "\n+++ b/" << path << "\n";

    size_t c = 0;
    while (c < changes.size()) {
        // Änderungen zusammenfassen, deren Kontext sich überlappt
        size_t first = changes[c];
        size_t last = changes[c];
        while (c + 1 < changes.size() && changes[c + 1] - last <= 2 * ctx + 1) {
            last = changes[++c];
        }
        ++c;

        const size_t begin = first > ctx ? first - ctx : 0;
        const size_t end = std::min(ops.size(), last + ctx + 1);

        size_t aCount = 0, bCount = 0;
        for (size_t k = begin; k < end; ++k) {
            if (ops[k].type != '+') ++aCount;
            if (ops[k].type != '-') ++bCount;
        }
        const size_t aStart = ops[begin].a;
        const size_t bStart = ops[begin].b;

        out << "@@ -" << (aCount ? aStart + 1 : aStart) << "," << aCount
            << " +" << (bCount ? bStart + 1 : bStart) << "," << bCount << " @@\n";
        for (size_t k = begin; k < end; ++k) {
            const Op& op = ops[k];
            out << op.type << (op.type == '+' ? b[op.b] : a[op.a]) << "\n";
        }
    }
    return out.str();
}
