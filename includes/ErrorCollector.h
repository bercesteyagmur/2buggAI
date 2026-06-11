#pragma once

#include <string>
#include <vector>


class ErrorCollector {
    public:
        static std::vector<std::string> collectErrors(const std::string& compileOutput);
        std::vector<std::string> sortedErrors(const std::vector<std::string>& errors);
        static std::string difficultyOf(const std::string& error);

};