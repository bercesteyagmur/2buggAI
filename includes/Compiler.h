//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_COMPILER_H
#define INC_2_BUGGY_AI_COMPILER_H

#include <vector>
#include <string>

class Compiler {

public:
    std::string compileIfNeeded(const std::string& targetPath);

    std::string compileMultiple(
        const std::vector<std::string>& sources,
        const std::vector<std::string>& includeDirs
    );
};

#endif //INC_2_BUGGY_AI_COMPILER_H
