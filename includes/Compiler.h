//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_COMPILER_H
#define INC_2_BUGGY_AI_COMPILER_H

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

class Compiler {

private:
    std::string lastCompileOutput;

public:
    std::string compileIfNeeded(const std::string& targetPath);

    std::string compileMultiple(const std::vector<std::string>& sources,const std::vector<std::string>& includeDirs);

    const std::string& getLastCompileOutput() const;

    std::string compileJava(const std::vector<std::string>& sources);

    std::string compileMaven(const std::string& projectPath);
    std::string compileGradle(const std::string& projectPath);

};

#endif //INC_2_BUGGY_AI_COMPILER_H
