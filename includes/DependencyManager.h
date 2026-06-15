

#ifndef INC_2BUGGAI_INNOLAB_DEPENDENCYMANAGER_H
#define INC_2BUGGAI_INNOLAB_DEPENDENCYMANAGER_H

#include <string>
#include <vector>

class DependencyManager {

public:

    static bool installPythonDependencies(const std::string& projectPath);

    static bool installJavaDependencies(const std::string& projectPath);

    static bool hasRequirementsTxt(const std::string& projectPath);

    static bool hasPyProject(const std::string& projectPath);

    static std::vector<std::string> detectPythonImports(const std::string& projectPath);

    static bool installPythonPackages(const std::string& projectPath,const std::vector<std::string>& packages);

    static bool removeBrokenRequirement(const std::string& projectPath,const std::string& pipOutput);

    static std::string findRequirementsTxt(const std::string& projectPath);

};

#endif //INC_2BUGGAI_INNOLAB_DEPENDENCYMANAGER_H
