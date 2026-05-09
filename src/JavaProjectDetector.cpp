//
// Created by bercesteyagmur on 5/9/26.
//

#include "JavaProjectDetector.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

JavaProjectType JavaProjectDetector::detect(const std::string& projectPath) {
    bool hasPom = fs::exists(projectPath + "/pom.xml");
    bool hasGradle = fs::exists(projectPath + "/build.gradle");

    if (hasPom) {
        std::ifstream file(projectPath + "/pom.xml");
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            if (content.find("spring-boot") != std::string::npos) {
                return JavaProjectType::SpringBootMaven;
            }
        }
        return JavaProjectType::Maven;
    }

    if (hasGradle) {
        std::ifstream file(projectPath + "/build.gradle");
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            if (content.find("spring-boot") != std::string::npos) {
                return JavaProjectType::SpringBootGradle;
            }
        }
        return JavaProjectType::Gradle;
    }

    return JavaProjectType::PlainJava;
}