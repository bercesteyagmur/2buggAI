//
// Created by bercesteyagmur on 4/26/26.
//

#ifndef INC_2_BUGGY_AI_ERRORMATCHER_H
#define INC_2_BUGGY_AI_ERRORMATCHER_H


#include "ChecklistReader.h"
#include <vector>
#include <string>

class ErrorMatcher {
private:
    std::vector<ErrorCategory> checklist;

public:
    // constructor
    ErrorMatcher(const std::vector<ErrorCategory>& checklist);

    std::vector<std::string> match(const std::string& error_output, const std::string& language);
};


#endif //INC_2_BUGGY_AI_ERRORMATCHER_H
