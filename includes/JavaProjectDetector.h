//
// Created by bercesteyagmur on 5/9/26.
//

#ifndef INC_2BUGGAI_INNOLAB_JAVAPROJECTDETECTOR_H
#define INC_2BUGGAI_INNOLAB_JAVAPROJECTDETECTOR_H


#include <string>
#include "JavaProjectType.h"

class JavaProjectDetector {
public:
    JavaProjectType detect(const std::string& projectPath);

    std::string getBuildPath() const;

private:
    std::string buildPath;
};


#endif //INC_2BUGGAI_INNOLAB_JAVAPROJECTDETECTOR_H
