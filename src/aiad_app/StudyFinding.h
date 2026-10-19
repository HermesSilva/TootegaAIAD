// What a study found in the drawing, and how to correct it (shown to the user and the agent).
#pragma once

#include <string>

namespace aiad::app
{

struct StudyFinding
{
    bool error = true;   // false = a note (shown, does not stop the simulation)
    std::string problem, fix;
};

}  // namespace aiad::app
