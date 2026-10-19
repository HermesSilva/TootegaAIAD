// Formulas of the document: numbers or expressions over named parameters, e.g. "bore / 2 + 0.003",
// "max(r1, r2)", "sin(30) * len" (trigonometric functions take degrees). Used by parameters (a
// parameter may be a formula of others) and by every numeric field of a feature.
#pragma once

#include <functional>
#include <set>
#include <string>

namespace aiad::doc
{

// Resolves a name to a value; returns false when unknown.
using NameLookup = std::function<bool(const std::string& name, double& value)>;

// Evaluates `text`. On failure returns false with a message naming the position and the problem.
bool Evaluate(const std::string& text, const NameLookup& lookup, double& value, std::string& error);

// Names referenced by `text` (dependency tracking); function names are not included.
std::set<std::string> ReferencedNames(const std::string& text);

}  // namespace aiad::doc
