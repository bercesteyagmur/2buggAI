#pragma once
#include <string>
#include <vector>

// Helpers for the fix loop: what to tell the model about earlier attempts, and whether a fix
// really removed an error or only changed its message.
namespace FixHistory {

// The lines of a build / program output that carry an error message (exceptions, "error",
// "Caused by", ...), without ANSI colours and Maven noise. Falls back to the last lines.
std::string errorExcerpt(const std::string& output, size_t maxLines = 12, size_t maxLineLength = 220);

// One entry of the attempt history that is sent with the next fix request.
//   outcome:     what happened, e.g. "error still present" or "not applied: Verifier rejected"
//   outputAfter: program / build output after the attempt (empty when it was not applied)
std::string describeAttempt(int number, const std::string& file, const std::string& fixExplanation,
                            const std::string& outcome, const std::string& outputAfter);

struct StackFrame {
    std::string file;  // file name as it appears in the trace
    int line = 0;
};

// Frames of project files in Java stack traces and Python tracebacks, innermost first.
// A frame belongs to the project when its file name matches the file name of one of
// projectFiles (JDK / site-packages frames are dropped).
std::vector<StackFrame> projectFrames(const std::string& output, const std::vector<std::string>& projectFiles);

// Where the program crashes, seen from outside the file a fix changed: the innermost project
// frame whose file is not changedFile, as "File.java:24". Empty when the output has no such
// frame (no crash, compiler errors, ...).
std::string crashSite(const std::string& output, const std::vector<std::string>& projectFiles,
                      const std::string& changedFile);

// What the failure looks like, to tell "no effect" from "progress" when an error category is still
// there after a fix: the error lines of the output without line numbers, column numbers and memory
// addresses (moving code around must not look like progress).
std::string failureFingerprint(const std::string& output);

// A fix made error categories disappear, but only turned them into others: at least one category
// vanished, at least one new one appeared, and the program still crashes at the same place
// outside the changed file.
bool onlyTransformed(const std::vector<std::string>& errorsBefore, const std::vector<std::string>& errorsAfter,
                     const std::string& outputBefore, const std::string& outputAfter,
                     const std::vector<std::string>& projectFiles, const std::string& changedFile);

} // namespace FixHistory
