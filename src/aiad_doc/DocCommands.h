// Document commands on the Command Bus: parameters, materials, parts and their features. The same
// commands for the user's panels, the AI (MCP) and command scripts; every change is one undoable step.
#pragma once

#include "CommandBus.h"
#include "Document.h"

#include <functional>

namespace aiad::doc
{

// `document` returns the open document (nullptr while none is open).
void RegisterDocumentCommands(cmd::CommandBus& bus, std::function<Document*()> document);

// Runs a command script (JSON list of {"command": name, "args": {...}}) through the bus, as a script
// actor. Stops at the first failure and reports the step.
bool RunScript(cmd::CommandBus& bus, const Json& script, std::string& error, int* steps = nullptr);

}  // namespace aiad::doc
