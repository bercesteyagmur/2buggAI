#include "StackPipeline.h"
#include "LanguageDetector.h"
#include "StackContext.h"
#include "StackToolchain.h"
#include "ToolOutputParser.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

void StackPipeline::printStack(const ProjectStack& stack) {
    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  STACK                                                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    if (!stack.config.error.empty()) {
        std::cout << "  Warning: " << StackDetector::configFileName() << ": " << stack.config.error << "\n";
    }
    if (!stack.composeFile.empty()) {
        std::cout << "  Compose file: " << stack.composeFile << " (" << stack.composeServices.size() << " services)\n";
    }
    for (const auto& c : stack.components) {
        std::cout << "  - " << c.name << "  [" << c.kind << " / " << c.language << "]  root: " << c.root;
        if (!c.service.empty()) std::cout << "  service: " << c.service;
        if (!c.dependsOn.empty()) {
            std::cout << "  depends on:";
            for (const auto& d : c.dependsOn) std::cout << " " << d;
        }
        std::cout << "\n";
    }
}

std::vector<CodeError> StackPipeline::parseErrors(const std::string& errorOutput, const ProjectStack& stack) {
    static const std::string head = "===== STEP: ";

    std::vector<CodeError> errors;
    std::string step, section;

    // One section = the output of one failed step. The step name tells which component
    // ran which tool, and with that which parser and which base folder to use.
    auto flush = [&]() {
        std::string componentName, tool;
        if (!section.empty() && StackToolchain::splitStepName(step, componentName, tool)) {
            const StackComponent* component = stack.find(componentName);
            const std::string root = component ? (fs::path(stack.projectRoot) / component->root).string()
                                               : stack.projectRoot;
            for (CodeError e : ToolOutputParser::parse(tool, section, root, stack.projectRoot)) {
                e.step = step;
                e.component = componentName;
                if (component) e.service = component->service;
                errors.push_back(e);
            }
        }
        section.clear();
    };

    std::istringstream stream(errorOutput);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(head, 0) == 0) {
            flush();
            step = line.substr(head.size(), line.find(" | ") - head.size());
            continue;
        }
        section += line + "\n";
    }
    flush();
    return errors;
}

int StackPipeline::run(const WebPipelineOptions& options, const std::vector<std::string>& files,
                       const ProjectStack& stack) {
    printStack(stack);

    // shared by the hooks below; the loop calls them long after this function set them up
    auto toolchain = std::make_shared<StackToolchain>(stack);

    // project-relative source files, used to find related files in other components
    std::vector<std::string> relativeFiles;
    for (const auto& f : files) {
        relativeFiles.push_back(fs::path(f).lexically_relative(options.targetPath).generic_string());
    }

    PipelineHooks hooks;

    // Checklist languages = languages of the source files, plus the language of every
    // component (a component may be detected by a marker file only).
    LanguageDetector detector;
    for (const auto& [lang, _] : detector.detectAll(files)) hooks.languages.insert(lang);
    for (const auto& c : stack.components) {
        hooks.languages.insert(c.language);
        if (c.language == "typescript") hooks.languages.insert("javascript");   // test runners, eslint
    }

    hooks.setup = [toolchain]() { return toolchain->setup(); };

    hooks.check = [toolchain](const std::string& changedFile) {
        std::vector<SetupStep> steps = toolchain->check(changedFile);
        if (!toolchain->skipped().empty()) {
            std::cout << "\nSkipped steps (tool not available on this machine):\n";
            for (const auto& s : toolchain->skipped()) std::cout << "  - " << s << "\n";
        }
        return steps;
    };

    hooks.parseErrors = [stack](const std::string& output) { return parseErrors(output, stack); };

    hooks.context = [stack, relativeFiles](const std::string&, const std::vector<std::string>& relevantFiles,
                                           const std::vector<CodeError>& errors) {
        return StackContext::select(stack, relativeFiles, relevantFiles, errors);
    };

    hooks.extendReport = [stack, toolchain](json& report) {
        json components = json::array();
        for (const auto& c : stack.components) {
            components.push_back({{"name", c.name}, {"service", c.service}, {"kind", c.kind},
                                  {"language", c.language}, {"root", c.root}, {"depends_on", c.dependsOn}});
        }
        report["stack"] = {{"components", components}, {"compose_file", stack.composeFile},
                           {"sql_dialect", stack.sqlDialect}, {"skipped_steps", toolchain->skipped()}};
    };

    return WebPipeline::runWithHooks(options, hooks);
}
