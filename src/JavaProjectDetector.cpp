//
// Created by bercesteyagmur on 5/9/26.
//

#include "JavaProjectDetector.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace fs = std::filesystem;

JavaProjectType JavaProjectDetector::detect(const std::string& projectPath) {
    buildPath = projectPath;

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

    for (const auto& entry : fs::recursive_directory_iterator(projectPath))
    { if (!entry.is_regular_file()) {
        continue;
    } if (entry.path().filename() == "build.gradle")
    { buildPath =
entry.path().parent_path().string();

        std::cout << "Detected nested Gradle module: "
                  << buildPath
                  << "\n";

        return JavaProjectType::Gradle;
    } if (entry.path().filename() == "pom.xml")
    {
        buildPath =
entry.path().parent_path().string();

        std::cout << "Detected nested Maven module: "
                  << buildPath
                  << "\n";

        return JavaProjectType::Maven;
    }
    }


    return JavaProjectType::PlainJava;
}


std::string JavaProjectDetector::getBuildPath() const {
    return buildPath;
}