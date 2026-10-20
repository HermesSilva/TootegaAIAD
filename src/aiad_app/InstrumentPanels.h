// The measuring instruments on screen: one panel each, drawn as a bench instrument (seven-segment display, secondary
// readouts, statistics, HOLD and RESET, unit, decimals, gate and window, a chart of its history), the marks of their
// probes in the view, and the menus that plug them in and show or hide them. Every change is a Command Bus command.
#pragma once

#include "Json.h"
#include "Markdown.h"

#include <imgui.h>
#include <pxr/base/gf/vec3d.h>

#include <functional>
#include <string>

namespace aiad::app
{

struct Session;

using RunCommand = std::function<bool(const std::string& name, const Json& args)>;
using Project = std::function<bool(const pxr::GfVec3d& model, ImVec2& screen)>;

// The panels of the visible instruments.
void DrawInstrumentPanels(Session& session, const UiFonts& fonts, float scale, const RunCommand& command);
// Each visible instrument's probes, marked where they read (a dot with the instrument's colour and its reading).
void DrawInstrumentMarks(Session& session, const Project& project, const UiFonts& fonts);
// View > Instruments: show or hide each one.
void InstrumentsMenu(Session& session, const RunCommand& command);
// A context menu on a joint or an element: "Measure with" every instrument that can read it.
void PlugInstrumentMenu(Session& session, const std::string& targetKind, const std::string& target, bool rotation, const RunCommand& command);
// The items of that menu (inside a menu the caller opened).
void PlugInstrumentItems(Session& session, const std::string& targetKind, const std::string& target, bool rotation, const RunCommand& command);
// The context menu of a part (right click on it in the view or in the tree): the joints that move its body and the elements
// that use it, each with the instruments that can read it.
void MeasurePartMenu(Session& session, const std::string& part, const std::string& body, const RunCommand& command);

}  // namespace aiad::app
