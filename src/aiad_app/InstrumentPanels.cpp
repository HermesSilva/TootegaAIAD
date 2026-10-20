#include "InstrumentPanels.h"

#include "Session.h"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

namespace aiad::app
{

namespace
{

// The look of a bench instrument: a dark chassis, a black display with amber seven-segment digits.
constexpr ImU32 kChassis = IM_COL32(24, 26, 30, 255), kDisplay = IM_COL32(6, 7, 8, 255), kDigits = IM_COL32(255, 176, 0, 255),
                kGhost = IM_COL32(255, 176, 0, 22), kDim = IM_COL32(140, 146, 156, 255), kLabel = IM_COL32(196, 200, 208, 255);
constexpr double kRefresh = 0.2;   // the panels' readings, per wall second
const double kGates[] = {0.0, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0};
const double kWindows[] = {0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0};
const double kReferences[] = {0, 14400, 18000, 19800, 21600, 25200, 28800, 36000};

ImVec4 Colour(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

// A panel key: lit when on.
bool Key(const char* label, bool lit, ImU32 litColour = IM_COL32(255, 176, 0, 255))
{
    ImGui::PushStyleColor(ImGuiCol_Button, lit ? Colour(litColour & 0x60FFFFFF) : Colour(IM_COL32(46, 50, 56, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colour(IM_COL32(62, 67, 75, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, lit ? Colour(litColour) : Colour(kLabel));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return clicked;
}

// A small dropdown of numbers with their labels.
bool PickNumber(const char* id, double& value, const double* values, size_t count, const std::function<std::string(double)>& label, float width)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, label(value).c_str(), ImGuiComboFlags_HeightLarge))
    {
        for (size_t i = 0; i < count; ++i)
            if (ImGui::Selectable(label(values[i]).c_str(), values[i] == value))
                value = values[i], changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

std::string Seconds(double s)
{
    return s <= 0.0 ? std::string("live") : s < 1.0 ? std::format("{:g} ms", s * 1e3) : std::format("{:g} s", s);
}

// The seven-segment display: the dimmed 8s behind the digits, as on a real panel.
void SevenSegment(ImDrawList* draw, ImFont* font, float size, ImVec2 rightBottom, const std::string& text, ImU32 colour)
{
    std::string ghost = text;
    for (char& c : ghost)
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-')
            c = '8';
    const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0.0f, ghost.c_str());
    const ImVec2 at(rightBottom.x - extent.x, rightBottom.y - extent.y);
    draw->AddText(font, size, at, kGhost, ghost.c_str());
    const ImVec2 real = font->CalcTextSizeA(size, 1e9f, 0.0f, text.c_str());
    draw->AddText(font, size, ImVec2(rightBottom.x - real.x, at.y), colour, text.c_str());
}

std::string ProbeText(const LiveInstrument& li)
{
    std::string text;
    for (const InstrumentProbe& p : li.probes)
        if (!p.hidden)
            text += (text.empty() ? "" : ", ") + p.target + " · " + p.quantity;
    return text;
}

}  // namespace

void DrawInstrumentPanels(Session& session, const UiFonts& fonts, float scale, const RunCommand& command)
{
    std::vector<std::function<void()>> deferred;   // commands run once the lock is let go (they rebuild the bank)
    {
        const auto lock = session.Lock();
        InstrumentBank& bank = session.instruments;
        const double now = bank.Now(), wall = ImGui::GetTime();
        for (LiveInstrument& li : bank.Instruments())
        {
            if (!li.visible || !li.type)
                continue;
            if (!li.hold && (li.refreshedAt < 0.0 || wall - li.refreshedAt >= kRefresh || wall < li.refreshedAt))
            {
                li.shown = bank.Measure(li, now);
                li.refreshedAt = wall;
            }
            auto set = [&deferred, &command, name = li.info.name, type = li.info.type, definition = li.info.definition](const char* key, Json value) {
                Json d = definition;
                d[key] = std::move(value);
                deferred.push_back([command, name, type, d] { command("aiad_instrument_define", {{"name", name}, {"type", type}, {"definition", d}}); });
            };
            // A new panel takes the next place of a grid over the view (three across); afterwards it stays where it is put.
            const int slot = int(&li - bank.Instruments().data());
            const ImGuiViewport* screen = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(screen->WorkPos.x + (310.0f + 480.0f * float(slot % 3)) * scale, screen->WorkPos.y + (80.0f + 370.0f * float(slot / 3 % 2)) * scale),
                                    ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(470.0f * scale, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Colour(kChassis));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, Colour(IM_COL32(16, 17, 20, 255)));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, Colour(IM_COL32(30, 33, 38, 255)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, li.detached ? 0.0f : 8.0f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
            bool open = true;
            const std::string title = std::format("{}  {}###instrument:{}", li.type->model, li.info.name, li.info.name);
            const bool shownNow = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse);
            li.detached = ImGui::GetWindowViewport() != ImGui::GetMainViewport();
            if (shownNow)
            {
                const float base = ImGui::GetFontSize();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                // Header: what it is and where it is plugged; HOLD and RESET.
                ImGui::PushFont(fonts.bold, 0.0f);
                ImGui::TextColored(Colour(li.colour), "%s", li.type->title);
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextColored(Colour(kDim), "%s", ProbeText(li).c_str());
                const float keys = ImGui::CalcTextSize("HOLD").x + ImGui::CalcTextSize("RESET").x + 4.0f * ImGui::GetStyle().FramePadding.x +
                                   ImGui::GetStyle().ItemSpacing.x;
                ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - keys));
                if (Key("HOLD", li.hold))
                    li.hold = !li.hold;
                ImGui::SameLine();
                if (Key("RESET", false))
                {
                    bank.Reset(li.info.name);
                    li.refreshedAt = -1.0;
                }
                // The display.
                const Reading& main = li.shown.main;
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x, height = base * 4.6f;
                const ImVec2 max(min.x + width, min.y + height);
                draw->AddRectFilled(min, max, kDisplay, 6.0f * scale);
                draw->AddRect(min, max, IM_COL32(52, 56, 62, 255), 6.0f * scale, 0, 1.5f * scale);
                const std::string caption = main.label + (li.shown.status.empty() ? std::string() : "   " + li.shown.status) + (li.hold ? "   HOLD" : "");
                draw->AddText(fonts.regular, base * 0.85f, ImVec2(min.x + base * 0.6f, min.y + base * 0.4f), kDim, caption.c_str());
                const float unitWidth = ImGui::CalcTextSize(main.unit.c_str()).x * 1.2f;
                const ImVec2 digitsEnd(max.x - base * 0.8f - unitWidth, max.y - base * 0.55f);
                SevenSegment(draw, fonts.seven ? fonts.seven : fonts.mono, base * 2.7f, digitsEnd, main.Text(), main.valid ? kDigits : kGhost);
                draw->AddText(fonts.mono, base * 1.15f, ImVec2(max.x - base * 0.5f - unitWidth, max.y - base * 1.7f), kLabel, main.unit.c_str());
                ImGui::Dummy(ImVec2(width, height));
                // Secondary readouts, three per row.
                if (!li.shown.secondary.empty() && ImGui::BeginTable("##secondary", 3, ImGuiTableFlags_SizingStretchSame))
                {
                    for (const Reading& r : li.shown.secondary)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextColored(Colour(kDim), "%s", r.label.c_str());
                        ImGui::PushFont(fonts.mono, 0.0f);
                        ImGui::TextColored(Colour(r.valid ? kLabel : kDim), "%s %s", r.Text().c_str(), r.unit.c_str());
                        ImGui::PopFont();
                    }
                    ImGui::EndTable();
                }
                // Settings: unit, decimals, gate, window (and the timer's and the timegrapher's).
                ImGui::Spacing();
                const doc::Dimension shown = li.info.type == "oscillation" ? doc::Dimension::Frequency
                                             : li.info.type == "timer"     ? (li.mode == "count" ? doc::Dimension::Count : li.mode == "frequency" ? doc::Dimension::Frequency : doc::Dimension::Time)
                                             : li.probes.empty()           ? doc::Dimension::Ratio
                                                                           : li.probes.front().dimension;
                ImGui::SetNextItemWidth(base * 5.0f);
                if (ImGui::BeginCombo("##unit", doc::FindUnit(shown, li.unit).label[0] ? doc::FindUnit(shown, li.unit).label : "-"))
                {
                    for (const doc::Unit& u : doc::UnitsOf(shown))
                        if (ImGui::Selectable(u.label[0] ? u.label : u.name, li.unit == u.name))
                            set("unit", u.name);
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (Key("-", false) && li.digits > 0)
                    set("digits", li.digits - 1);
                ImGui::SameLine();
                ImGui::TextColored(Colour(kDim), "%d dec", li.digits);
                ImGui::SameLine();
                if (Key("+", false) && li.digits < 9)
                    set("digits", li.digits + 1);
                ImGui::SameLine();
                if (double g = li.gate; PickNumber("##gate", g, kGates, std::size(kGates), [](double s) { return "gate " + Seconds(s); }, base * 6.0f))
                    set("gate", g);
                ImGui::SameLine();
                if (double w = li.window; PickNumber("##window", w, kWindows, std::size(kWindows), [](double s) { return "chart " + Seconds(s); }, base * 6.0f))
                    set("window", w);
                if (li.info.type == "timer")
                {
                    ImGui::SetNextItemWidth(base * 6.0f);
                    if (ImGui::BeginCombo("##mode", li.mode.c_str()))
                    {
                        for (const char* m : {"interval", "frequency", "count", "stopwatch"})
                            if (ImGui::Selectable(m, li.mode == m))
                            {
                                Json d = li.info.definition;
                                d["mode"] = m;
                                d.erase("unit");
                                deferred.push_back([command, name = li.info.name, d] { command("aiad_instrument_define", {{"name", name}, {"type", "timer"}, {"definition", d}}); });
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(base * 5.0f);
                    if (ImGui::BeginCombo("##edge", li.edge.c_str()))
                    {
                        for (const char* e : {"rise", "fall", "both"})
                            if (ImGui::Selectable(e, li.edge == e))
                                set("edge", e);
                        ImGui::EndCombo();
                    }
                }
                if (li.info.type == "oscillation")
                {
                    if (double ref = li.reference;
                        PickNumber("##reference", ref, kReferences, std::size(kReferences),
                                   [](double v) { return v > 0.0 ? std::format("reference {:g} vph", v) : std::string("no reference"); }, base * 9.0f))
                        set("reference", ref);
                }
                // In slow motion, gate and chart cover less of the machine's time (as watched): say how much.
                if (bank.Slow() < 1.0)
                    ImGui::TextColored(Colour(kDim), "%s: gate %s, chart %s of machine time", std::format("{:g}x", bank.Slow()).c_str(),
                                       li.gate > 0.0 ? Seconds(bank.Gate(li)).c_str() : "live", Seconds(bank.Window(li)).c_str());
                // The chart: the history over the window (mean, and the band from min to max in each bucket).
                ImPlot::PushStyleColor(ImPlotCol_PlotBg, Colour(kDisplay));
                ImPlot::PushStyleColor(ImPlotCol_FrameBg, Colour(kChassis));
                // A dynamometer draws its curve: brake power and torque against the shaft's speed (every bucket of the history).
                if (li.info.type == "dynamometer" && li.probes.size() >= 5 &&
                    ImPlot::BeginPlot("##curve", ImVec2(-1.0f, base * 10.0f), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
                {
                    const InstrumentProbe &power = li.probes[0], &torque = li.probes[1], &speed = li.probes[2], &held = li.probes[3];
                    const doc::Unit& u = doc::FindUnit(doc::Dimension::Power, li.unit);
                    ImPlot::SetupAxes("rpm", u.label, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                    ImPlot::SetupAxis(ImAxis_Y2, "N·m", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
                    std::vector<double> rpm, kw, nm;
                    for (size_t k = 0; k < power.history.size() && k < torque.history.size() && k < speed.history.size() && k < held.history.size(); ++k)
                        if (DynoHeld(speed.history[k].mean, held.history[k].mean))
                        {
                            rpm.push_back(speed.history[k].mean * 60.0 / (2.0 * std::numbers::pi));
                            kw.push_back(power.history[k].mean * u.scale + u.offset);
                            nm.push_back(torque.history[k].mean);
                        }
                    if (!rpm.empty())
                    {
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                        const ImVec4 pc = Colour(IM_COL32(90, 200, 255, 255)), tc = Colour(IM_COL32(255, 176, 0, 255));   // power blue, torque amber
                        ImPlot::PlotScatter(std::format("Power [{}]", u.label).c_str(), rpm.data(), kw.data(), int(rpm.size()),
                                            ImPlotSpec(ImPlotProp_LineColor, pc, ImPlotProp_MarkerFillColor, pc, ImPlotProp_MarkerLineColor, pc, ImPlotProp_MarkerSize, 2.0f * scale));
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                        ImPlot::PlotScatter("Torque [N·m]", rpm.data(), nm.data(), int(rpm.size()),
                                            ImPlotSpec(ImPlotProp_LineColor, tc, ImPlotProp_MarkerFillColor, tc, ImPlotProp_MarkerLineColor, tc, ImPlotProp_MarkerSize, 2.0f * scale));
                    }
                    ImPlot::EndPlot();
                }
                if (ImPlot::BeginPlot("##chart", ImVec2(-1.0f, base * 8.0f), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | (std::count_if(li.probes.begin(), li.probes.end(), [](const InstrumentProbe& q) { return !q.hidden; }) > 1 ? 0 : ImPlotFlags_NoLegend)))
                {
                    const double window = bank.Window(li);
                    ImPlot::SetupAxes(window < 1.0 ? "machine ms" : "machine s", nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
                    const double xs = window < 1.0 ? 1e3 : 1.0;
                    ImPlot::SetupAxisLimits(ImAxis_X1, -window * xs, 0.0, ImPlotCond_Always);
                    for (size_t k = 0; k < li.probes.size(); ++k)
                    {
                        const InstrumentProbe& p = li.probes[k];
                        if (p.hidden)
                            continue;
                        const doc::Unit& u = doc::FindUnit(p.dimension, k == 0 && li.info.type != "oscillation" && li.info.type != "timer" ? li.unit : std::string());
                        std::vector<double> x, mean, lo, hi;
                        for (const InstrumentProbe::Bucket& b : p.history)
                            if (b.t >= now - window)
                            {
                                x.push_back((b.t - now) * xs);
                                mean.push_back(b.mean * u.scale + u.offset);
                                lo.push_back(b.min * u.scale + u.offset);
                                hi.push_back(b.max * u.scale + u.offset);
                            }
                        if (x.empty())
                            continue;
                        const ImU32 c = k == 0 ? li.colour : (li.colour ^ (0x5A3C1E * (k + 1))) | 0xFF000000;
                        const std::string label = std::format("{} [{}]", p.target, u.label);
                        ImPlot::PlotShaded(label.c_str(), x.data(), lo.data(), hi.data(), int(x.size()), ImPlotSpec(ImPlotProp_FillColor, Colour(c), ImPlotProp_FillAlpha, 0.18f));
                        ImPlot::PlotLine(label.c_str(), x.data(), mean.data(), int(x.size()), ImPlotSpec(ImPlotProp_LineColor, Colour(c), ImPlotProp_LineWeight, 1.6f * scale));
                    }
                    ImPlot::EndPlot();
                }
                ImPlot::PopStyleColor(2);
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (!open)
                deferred.push_back([command, name = li.info.name] { command("aiad_instrument_show", {{"name", name}, {"visible", false}}); });
        }
    }
    for (const auto& run : deferred)
        run();
}

void DrawInstrumentMarks(Session& session, const Project& project, const UiFonts& fonts)
{
    const auto lock = session.Lock();
    const sim::mb::Model& model = session.study.machine.mechanism;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float base = ImGui::GetFontSize();
    for (const LiveInstrument& li : session.instruments.Instruments())
    {
        if (!li.visible || !li.type)
            continue;
        for (const InstrumentProbe& p : li.probes)
        {
            // Where the probe reads, at rest: a joint's point, an outline's or a profile's centre, a spring's end.
            pxr::GfVec3d at(0.0);
            bool known = p.index >= 0;
            auto centre = [&](const std::vector<sim::mb::Vec3>& points) {
                for (const sim::mb::Vec3& q : points)
                    at += pxr::GfVec3d(q[0], q[1], q[2]) / double(std::max<size_t>(points.size(), 1));
            };
            using S = InstrumentProbe::Source;
            if (!known || p.hidden)
                continue;
            if (p.source == S::DynoPower || p.source == S::DynoTorque || p.source == S::DynoSpeed)
            {
                const auto& j = model.joints[size_t(session.study.machine.dynamometers[size_t(p.index)].joint)];
                at = pxr::GfVec3d(j.point[0], j.point[1], j.point[2]);
            }
            else if (p.source == S::JointPosition || p.source == S::JointSpeed || p.source == S::JointAcceleration)
                at = pxr::GfVec3d(model.joints[size_t(p.index)].point[0], model.joints[size_t(p.index)].point[1], model.joints[size_t(p.index)].point[2]);
            else if (p.source == S::TorsionTorque)
            {
                const auto& j = model.joints[size_t(model.torsions[size_t(p.index)].joint)];
                at = pxr::GfVec3d(j.point[0], j.point[1], j.point[2]);
            }
            else if (p.source == S::TouchForce && !model.touches[size_t(p.index)].outline0.empty())
                centre(model.touches[size_t(p.index)].outline0.front());
            else if (p.source == S::ContactForce)
                centre(model.contacts[size_t(p.index)].profile);
            else if (p.source == S::SpringForce)
                at = pxr::GfVec3d(model.springs[size_t(p.index)].point0[0], model.springs[size_t(p.index)].point0[1], model.springs[size_t(p.index)].point0[2]);
            else
                known = false;
            ImVec2 s;
            if (!known || !project(at, s))
                continue;
            // The tip, a short lead and the instrument's reading beside it.
            const ImVec2 label(s.x + base * 1.6f, s.y - base * 1.4f);
            draw->AddLine(s, ImVec2(label.x - base * 0.2f, label.y + base * 0.5f), li.colour, 1.5f);
            draw->AddCircleFilled(s, base * 0.32f, li.colour);
            draw->AddCircle(s, base * 0.32f, IM_COL32(10, 10, 12, 255), 0, 1.5f);
            const std::string text = std::format("{}  {} {}", li.type->model, li.shown.main.Text(), li.shown.main.unit);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            draw->AddRectFilled(ImVec2(label.x - 4, label.y - 2), ImVec2(label.x + size.x + 4, label.y + size.y + 2), IM_COL32(10, 11, 13, 210), 4.0f);
            draw->AddText(fonts.mono, base, label, li.colour, text.c_str());
        }
    }
}

void InstrumentsMenu(Session& session, const RunCommand& command)
{
    std::vector<std::pair<std::string, bool>> list;
    {
        const auto lock = session.Lock();
        for (const LiveInstrument& li : session.instruments.Instruments())
            list.push_back({li.info.name, li.visible});
    }
    if (list.empty())
        ImGui::TextDisabled("None yet: right-click a part in the view (or in the tree) > Measure");
    for (const auto& [name, visible] : list)
        if (ImGui::MenuItem(name.c_str(), nullptr, visible))
            command("aiad_instrument_show", {{"name", name}, {"visible", !visible}});
    if (!list.empty())
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Reset all readings"))
        {
            const auto lock = session.Lock();
            session.instruments.Reset();
        }
    }
}

void PlugInstrumentMenu(Session& session, const std::string& targetKind, const std::string& target, bool rotation, const RunCommand& command)
{
    if (!ImGui::BeginMenu("Measure with"))
        return;
    PlugInstrumentItems(session, targetKind, target, rotation, command);
    ImGui::EndMenu();
}

void MeasurePartMenu(Session& session, const std::string& part, const std::string& body, const RunCommand& command)
{
    if (!session.document)
        return;
    ImGui::TextDisabled("Measure %s", part.c_str());
    ImGui::Separator();
    bool any = false;
    // The joints that move its body: what it does (a hand on its arbor: its speed, its angle).
    for (const doc::JointInfo& j : session.document->Joints())
        if ((j.body0 == body || j.body1 == body) && (j.type == "revolute" || j.type == "cylindrical" || j.type == "prismatic"))
        {
            any = true;
            if (ImGui::BeginMenu(std::format("{} ({})", j.name, j.type).c_str()))
            {
                PlugInstrumentItems(session, "joint", j.name, j.type != "prismatic", command);
                ImGui::EndMenu();
            }
        }
    // The elements that use the part (a spring, a touch, a chamber's bore...).
    for (const doc::ElementInfo& e : session.document->Elements())
    {
        bool uses = false;
        for (const auto& [key, value] : e.definition.items())
            uses = uses || (value.is_string() && value.get<std::string>() == part);
        std::string kind, known;
        doc::Dimension dimension;
        if (!uses || doc::DefaultQuantity("meter", e.type).empty() || !doc::QuantityDimension(e.type, true, doc::DefaultQuantity("meter", e.type), dimension, known))
            continue;
        any = true;
        if (ImGui::BeginMenu(std::format("{} ({})", e.name, e.type).c_str()))
        {
            PlugInstrumentItems(session, e.type, e.name, true, command);
            ImGui::EndMenu();
        }
    }
    if (!any)
        ImGui::TextDisabled(session.document->BodyFixed(body) ? "Its body is fixed: nothing moves to measure."
                                                              : "No joint moves its body and no element uses it.");
}

void PlugInstrumentItems(Session& session, const std::string& targetKind, const std::string& target, bool rotation, const RunCommand& command)
{
    for (const doc::InstrumentType& t : doc::InstrumentTypes())
    {
        const std::string quantity = doc::DefaultQuantity(t.type, targetKind);
        doc::Dimension dimension;
        std::string known;
        if (quantity.empty() || !doc::QuantityDimension(targetKind, rotation, quantity, dimension, known))
            continue;
        if (!t.dimensions.empty() && std::find(t.dimensions.begin(), t.dimensions.end(), dimension) == t.dimensions.end())
            continue;
        if (ImGui::MenuItem(std::format("{}  {}", t.model, t.title).c_str()))
        {
            // A free name: "TC-1 Balance staff", "TC-1 Balance staff 2"...
            std::string name = std::format("{} {}", t.model, target);
            for (int n = 2; session.document && session.document->FindInstrument(name); ++n)
                name = std::format("{} {} {}", t.model, target, n);
            Json probe = {{targetKind == "joint" ? "joint" : "element", target}};
            Json definition = {{"probes", Json::array({probe})}};
            command("aiad_instrument_define", {{"name", name}, {"type", t.type}, {"definition", definition}});
        }
    }
}

}  // namespace aiad::app
