#include "Compiler.h"
#include "ShellQuote.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <array>
#include <regex>
#include <fstream>
#include <sstream>

static std::string detectJavaVersion(const std::string& content) {

    std::regex versionRegex(R"(VERSION_(\d+))");
    std::smatch match;

    if (std::regex_search(content, match, versionRegex)) {
        return match[1];
    }

    std::regex sourceRegex(
        R"(sourceCompatibility\s*=\s*['"]?(\d+))");

    if (std::regex_search(content, match, sourceRegex)) {
        return match[1];
    }

    std::regex mavenRegex(
        R"(<maven\.compiler\.source>(\d+)</maven\.compiler\.source>)");

    if (std::regex_search(content, match, mavenRegex)) {
        return match[1];
    }

    return "";
}

static std::string detectGradleVersion(const std::string& projectPath) {

    namespace fs = std::filesystem;

    for (const auto& entry :
         fs::recursive_directory_iterator(projectPath)) {

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().filename() ==
            "gradle-wrapper.properties") {

            std::ifstream file(entry.path());

            if (!file) {
                continue;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();

            std::string content =
                buffer.str();

            std::regex gradleRegex(
                R"(gradle-(\d+(\.\d+)*)-bin)");

            std::smatch match;

            if (std::regex_search(content,
                                  match,
                                  gradleRegex)) {

                return match[1];
                                  }
            }
         }

    // fallback heuristic
    for (const auto& entry :
         fs::recursive_directory_iterator(projectPath)) {

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().filename() ==
            "build.gradle") {

            std::ifstream file(entry.path());

            if (!file) {
                continue;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();

            std::string content =
                buffer.str();

            if (content.find("platform(")
                != std::string::npos) {

                return "7.6";
                }

            if (content.find("toolchain")
                != std::string::npos) {

                return "7.6";
                }
            }
         }

    return "4.4.1";
}

static std::string ensureGradleInstalled(
    const std::string& version) {

    std::string gradlePath =
        "/tmp/gradle-" + version;

    std::string executable =
        gradlePath + "/bin/gradle";

    if (std::filesystem::exists(executable)) {

        std::cout << "Gradle "
                  << version
                  << " already installed\n";

        return executable;
    }

    std::cout << "Installing Gradle "
              << version << "...\n";

    if (system("which wget > /dev/null 2>&1") != 0) {

        std::cout << "wget is not installed\n";

        return "gradle";
    }

    if (system("which unzip > /dev/null 2>&1") != 0) {

        std::cout << "unzip is not installed\n";

        return "gradle";
    }

    std::string zipName =
        "gradle-" + version + "-bin.zip";

    std::string downloadUrl =
        "https://services.gradle.org/distributions/"
        + zipName;

    std::string cmd =
        "cd /tmp && "
        "wget -q " + downloadUrl +
        " && unzip -o " + zipName;

    int status = system(cmd.c_str());

    if (status != 0) {

        std::cout << "Gradle installation failed\n";

        return "gradle";
    }

    return executable;
}

static std::string detectRequiredJavaHome(const std::string& projectPath) {

    namespace fs = std::filesystem;

    for (const auto& entry :
         fs::recursive_directory_iterator(projectPath)) {

        if (!entry.is_regular_file()) {
            continue;
        }

        std::string filename =
            entry.path().filename().string();

        if (filename != "build.gradle" &&
            filename != "pom.xml") {

            continue;
            }

        std::ifstream file(entry.path());

        if (!file) {
            continue;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        std::string content = buffer.str();

        std::string version =
            detectJavaVersion(content);

        if (!version.empty()) {

            return "/usr/lib/jvm/java-"
                   + version +
                   "-openjdk-amd64";
        }
         }

    return "";
}

static void ensureJavaInstalled(const std::string& javaHome,
                                const std::string& version) {

    if (javaHome.empty()) {
        return;
    }

    if (std::filesystem::exists(javaHome)) {

        std::cout << "Java " << version
                  << " already installed\n";

        return;
    }

    std::cout << "Installing Java "
              << version << "...\n";

    std::string installCmd;

    if (version == "17") {

        installCmd =
            "sudo apt update && "
            "sudo apt install -y openjdk-17-jdk";

    }
    else if (version == "11") {

        installCmd =
            "sudo apt update && "
            "sudo apt install -y openjdk-11-jdk";

    }
    else if (version == "8") {

        installCmd =
            "sudo apt update && "
            "sudo apt install -y openjdk-8-jdk";
    }

    else if (version == "21") {
        installCmd =
            "sudo apt update && "
            "sudo apt install -y openjdk-21-jdk";
    }

    std::cout << "Running install command:\n"
              << installCmd << "\n";

    system(installCmd.c_str());
}

std::string Compiler::compileIfNeeded(const std::string& targetPath) {
    namespace fs = std::filesystem;

    if (!fs::exists(targetPath)) {
        //throw std::runtime_error("File does not exist: " + targetPath);
        std::cout << "Compilation failed, continuing with error analysis...\n";
        return "";
    }

    std::string ext = fs::path(targetPath).extension();

    if (ext == ".c" || ext == ".cpp" || ext == ".cc") {
        std::string outFile = "/tmp/buggy_compiled_" + std::to_string(getpid());
        std::string compiler = (ext == ".c") ? "gcc" : "g++";
        std::string compileCmd = compiler + " -g -pthread -o " + outFile + " " + targetPath + " 2>&1";

        FILE* pipe = popen(compileCmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Compilation failed");
        }

        std::array<char, 256> buffer;
        std::string output;
        while (fgets(buffer.data(), buffer.size(), pipe)) {
            output += buffer.data();
            std::cout << buffer.data();
        }

        lastCompileOutput = output;

        int status = pclose(pipe);

        if (status != 0) {
           // throw std::runtime_error("Compilation failed");
            std::cout << "Compilation failed, continuing with error analysis...\n";
            return "";
        }

        return outFile;
    }

    return targetPath;
}


std::string Compiler::compileMultiple(
    const std::vector<std::string>& sources,
    const std::vector<std::string>& includeDirs
) {
    if (sources.empty()) return "";

    std::string outFile = "/tmp/buggy_multi_" + std::to_string(getpid());

    bool useCpp = false;

    for (const auto& file : sources) {
        std::string ext = std::filesystem::path(file).extension().string();
        if (ext == ".cpp" || ext == ".cc") {
            useCpp = true;
            break;
        }
    }

    std::string compiler = useCpp ? "g++" : "gcc";

    std::string cmd = compiler + " -g -pthread -o " + ShellQuote::quote(outFile);

    for (const auto& inc : includeDirs) {
        cmd += " -I" + ShellQuote::quote(inc);
    }

    for (const auto& src : sources) {
        cmd += " " + ShellQuote::quote(src);
    }

    cmd += " 2>&1";

    RunResult res = run_capture(cmd);

    lastCompileOutput = res.output;

    if (res.exit_code != 0) {
       // throw std::runtime_error("Multi-file compilation failed");
        std::cout << "Compilation failed, continuing with error analysis...\n";
        return "";
    }

    return outFile;
}

const std::string& Compiler::getLastCompileOutput() const {
    return lastCompileOutput;
}



std::string Compiler::compileJava(const std::vector<std::string>& sources) {
    namespace fs = std::filesystem;

    if (sources.empty()) {
        return "";
    }

    std::string outDir = "/tmp/buggy_java_classes_" + std::to_string(getpid());
    fs::create_directories(outDir);

    std::string cmd = "javac -g -d " + ShellQuote::quote(outDir);

    for (const auto& src : sources) {
        std::string ext = fs::path(src).extension().string();

        if (ext == ".java") {
            cmd += " " + ShellQuote::quote(src);
        }
    }

    cmd += " 2>&1";

    RunResult res = run_capture(cmd);
    lastCompileOutput = res.output;

    if (res.exit_code != 0) {
        std::cout << "Java compilation failed, continuing with error analysis...\n";
        return "";
    }

    // find the Java file that contains main()
    std::string mainClass;

    for (const auto& src : sources) {
        std::string ext = fs::path(src).extension().string();

        if (ext != ".java") {
            continue;
        }

        std::ifstream file(src);
        if (!file) {
            continue;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        if (content.find("public static void main") != std::string::npos) {
            mainClass = fs::path(src).stem().string();

            // package detection
            std::string keyword = "package ";
            size_t packagePos = content.find(keyword);

            if (packagePos != std::string::npos) {
                size_t start = packagePos + keyword.size();
                size_t end = content.find(";", start);

                if (end != std::string::npos) {
                    std::string packageName = content.substr(start, end - start);
                    mainClass = packageName + "." + mainClass;
                }
            }

            break;
        }
    }

    if (mainClass.empty()) {
        std::cout << "Java compilation succeeded, but no main method was found.\n";
        return "";
    }

    // Create small executable script:
    // /tmp/buggy_java_run_PID
    std::string runScript = "/tmp/buggy_java_run_" + std::to_string(getpid());

    std::ofstream script(runScript);

    script << "#!/bin/sh\n";
    script << "java -cp " << ShellQuote::quote(outDir) << " " << mainClass << " \"$@\"\n";
    script.close();

    fs::permissions(runScript,fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,fs::perm_options::add);

    return runScript;
}

std::string Compiler::compileMaven(const std::string& projectPath) {

    std::string javaHome = detectRequiredJavaHome(projectPath);

    if (javaHome.find("17") != std::string::npos) {

        ensureJavaInstalled(javaHome, "17");

    }
    else if (javaHome.find("11") != std::string::npos) {

        ensureJavaInstalled(javaHome, "11");

    }
    else if (javaHome.find("8") != std::string::npos) {

        ensureJavaInstalled(javaHome, "8");
    }

    std::string javaEnv;
    if (!javaHome.empty() && std::filesystem::exists(javaHome)) {
        javaEnv =
            "export JAVA_HOME=" + javaHome +
            " && export PATH=$JAVA_HOME/bin:$PATH && ";
    }

    std::string cmd =
    "cd " + ShellQuote::quote(projectPath) +
    " && " + javaEnv +
    "mvn clean install -DskipTests=false 2>&1";


    std::cout << "Executing Maven build:\n" << cmd << "\n\n";


    RunResult res = run_capture(cmd);
    lastCompileOutput = res.output;

    if (res.exit_code != 0) {
        std::cout << "Maven build failed, continuing with error analysis...\n";
        return "";
    }

    std::string findJarCmd =
        "ls " + ShellQuote::quote(projectPath + "/target") +
        "/*.jar 2>/dev/null | head -n 1";

    RunResult jarRes = run_capture(findJarCmd);

    std::string jarPath = jarRes.output;

    jarPath.erase(
        std::remove(jarPath.begin(), jarPath.end(), '\n'),
        jarPath.end()
    );

    if (jarPath.empty()) {
        std::cout << "Maven build succeeded, but no JAR file was found.\n";
        return "";
    }

    return jarPath;
}

std::string Compiler::compileGradle(const std::string& projectPath) {
    std::string cmd;

    std::string javaHome = detectRequiredJavaHome(projectPath);

    std::string gradleVersion =
    detectGradleVersion(projectPath);

    std::string gradleExecutable =
        ensureGradleInstalled(gradleVersion);

    std::cout << "Using Gradle version: "
              << gradleVersion
              << "\n";


    if (javaHome.find("17") != std::string::npos) {

        ensureJavaInstalled(javaHome, "17");

    }
    else if (javaHome.find("11") != std::string::npos) {

        ensureJavaInstalled(javaHome, "11");

    }
    else if (javaHome.find("8") != std::string::npos) {

        ensureJavaInstalled(javaHome, "8");
    }

    std::string javaEnv;
    if (!javaHome.empty() && std::filesystem::exists(javaHome)) {
        javaEnv =
            "export JAVA_HOME=" + javaHome +
            " && export PATH=$JAVA_HOME/bin:$PATH && ";
    }

    if (std::filesystem::exists(projectPath + "/gradlew")) {
        cmd = "cd " + ShellQuote::quote(projectPath) +
              " && " + javaEnv +
              "chmod +x gradlew && ./gradlew build --stacktrace 2>&1";
    } else {
        cmd = "cd " + ShellQuote::quote(projectPath) +
              " && " + javaEnv +
                  gradleExecutable +
     " build --stacktrace 2>&1";
    }

    std::cout << "Executing Gradle build:\n" << cmd << "\n\n";

    RunResult res = run_capture(cmd);
    lastCompileOutput = res.output;

    if (res.exit_code != 0) {
        std::cout << "Gradle build failed, continuing with error analysis...\n";
        return "";
    }

    std::string findJarCmd =
        "ls " + ShellQuote::quote(projectPath + "/build/libs") +
        "/*.jar 2>/dev/null | head -n 1";

    RunResult jarRes = run_capture(findJarCmd);

    std::string jarPath = jarRes.output;

    jarPath.erase(
        std::remove(jarPath.begin(), jarPath.end(), '\n'),
        jarPath.end()
    );

    if (jarPath.empty()) {
        std::cout << "Gradle build succeeded, but no JAR file was found.\n";
        return "";
    }

    return jarPath;
}
