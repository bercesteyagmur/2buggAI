#pragma once
// Helpers shared by the unit tests.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "WebPipeline.h"

namespace support {

inline const std::string FIXTURE = "tests/fixtures/fullstack_sample";
inline const std::string OUTPUTS = "tests/fixtures/tool_output/";

inline std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

// Tool output sample; the placeholder path of the captures is mapped to the real fixture.
inline std::string toolOutput(const std::string& name) {
    const std::string fixture = std::filesystem::absolute(FIXTURE).lexically_normal().string();
    return replaceAll(readFile(OUTPUTS + name), "/work/fullstack_sample", fixture);
}

// Temporary project folder, removed at the end of the test.
class TempProject {
public:
    TempProject() {
        static int counter = 0;
        root_ = std::filesystem::temp_directory_path()
              / ("buggai_unit_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(root_);
    }
    ~TempProject() { std::error_code ec; std::filesystem::remove_all(root_, ec); }

    void write(const std::string& relative, const std::string& content = "") const {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << content;
    }
    std::string path() const { return root_.string(); }

private:
    std::filesystem::path root_;
};

template <typename T>
bool contains(const std::vector<T>& list, const T& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

inline bool contains(const std::vector<std::string>& list, const char* value) {
    return contains(list, std::string(value));
}

// First error for a file, or nullptr.
inline const CodeError* errorIn(const std::vector<CodeError>& errors, const std::string& file) {
    for (const auto& e : errors) {
        if (e.file == file) return &e;
    }
    return nullptr;
}

} // namespace support
