// Tootega AIAD - engine milestone: the single-cylinder engine model (drawing = simulated physics) in a
// Hydra viewport (HdAurora, DXR), driven live by the coupled simulation (Chrono + 0D cycle + magneto).
// The user and the AI agent (Claude Code through MCP -> bridge) operate it through the same Command Bus.
#include "AiPanels.h"
#include "Bridge.h"
#include "ClaudeProcess.h"
#include "CommandBus.h"
#include "D3DContext.h"
#include "EngineCommands.h"
#include "DocCommands.h"
#include "DocumentScene.h"
#include "EngineSession.h"
#include "MainThreadQueue.h"
#include "Paths.h"
#include "Text.h"

#include "HydraViewport.h"
#include "engine/EngineSimulation.h"
#include "magneto/MagnetoField.h"
#include "magneto/MagnetoIgnition.h"

#include <windows.h>
#include <dbghelp.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <implot.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <misc/cpp/imgui_stdlib.h>
#include <map>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{

using namespace aiad;
using pxr::GfVec3d;
constexpr double kPi = std::numbers::pi;

app::D3DContext g_d3d;
cmd::CommandBus* g_bus = nullptr;  // the one path that mutates the model (UI, AI, scripts)

// Runs a command as the user; a failure is shown in the status line.
std::string g_lastError;
bool UserCommand(const std::string& name, const Json& args)
{
    const cmd::CommandOutput out = g_bus->Execute(name, args, "user");
    if (!out.ok)
        g_lastError = out.error;
    return out.ok;
}

std::vector<uint8_t> EncodePng(const render::ViewportImage& image)
{
    std::vector<uint8_t> png;
    if (image.rgba.empty())
        return png;
    stbi_write_png_to_func([](void* c, void* d, int n) { static_cast<std::vector<uint8_t>*>(c)->insert(static_cast<std::vector<uint8_t>*>(c)->end(), (uint8_t*)d, (uint8_t*)d + n); },
                           &png, image.width, image.height, 4, image.rgba.data(), image.width * 4);
    return png;
}

// Textures for the PNG images of the chat (viewport snapshots returned by tools), uploaded once.
class ChatTextures
{
public:
    ImTextureID Get(const std::string& base64, int& width, int& height)
    {
        const std::string key = std::to_string(std::hash<std::string>{}(base64));
        auto it = m_entries.find(key);
        if (it == m_entries.end())
        {
            if (m_entries.size() >= 48)
                return ImTextureID(0);
            Entry entry;
            const std::vector<uint8_t> png = Base64Decode(base64);
            int channels = 0;
            if (uint8_t* rgba = stbi_load_from_memory(png.data(), int(png.size()), &entry.width, &entry.height, &channels, 4))
            {
                entry.pixels.assign(rgba, rgba + size_t(entry.width) * entry.height * 4);
                stbi_image_free(rgba);
                entry.texture.Create(g_d3d, entry.width, entry.height);
            }
            it = m_entries.emplace(key, std::move(entry)).first;
        }
        width = it->second.width;
        height = it->second.height;
        return it->second.uploaded ? ImTextureID(it->second.texture.Srv().ptr) : ImTextureID(0);
    }
    void Upload(ID3D12GraphicsCommandList* list)
    {
        for (auto& [key, e] : m_entries)
            if (!e.uploaded && !e.pixels.empty())
            {
                e.texture.Upload(g_d3d, list, e.pixels.data());
                e.uploaded = true;
                e.pixels.clear();
            }
    }
    void Destroy()
    {
        for (auto& [key, e] : m_entries)
            if (e.width > 0)
                e.texture.Destroy(g_d3d);
        m_entries.clear();
    }

private:
    struct Entry
    {
        app::UploadTexture texture;
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        bool uploaded = false;
    };
    std::map<std::string, Entry> m_entries;
};

// Unhandled crash: write aiad_crash.dmp next to the executable (read it with the PDB in cdb/WinDbg).
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info)
{
    const std::filesystem::path dump = ExeDir() / "aiad_crash.dmp";
    HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &exception, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Folder next to the executable if present (deployed layout), otherwise the development folder.
std::string Resource(const char* relative, const char* development)
{
    const std::filesystem::path deployed = ExeDir() / relative;
    return std::filesystem::exists(deployed) ? deployed.string() : std::string(development);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;
    switch (msg)
    {
    case WM_SIZE:
        if (g_d3d.Device() && wParam != SIZE_MINIMIZED)
            g_d3d.Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

using app::EngineSession;
using app::OrbitCamera;


const char* PhaseName(double cycleDeg, ImVec4& colour)
{
    if (cycleDeg < 180.0)
        return colour = ImVec4(1.0f, 0.45f, 0.2f, 1), "Power";
    if (cycleDeg < 360.0)
        return colour = ImVec4(0.85f, 0.6f, 0.35f, 1), "Exhaust";
    if (cycleDeg < 540.0)
        return colour = ImVec4(0.35f, 0.65f, 1.0f, 1), "Intake";
    return colour = ImVec4(0.6f, 0.85f, 0.5f, 1), "Compression";
}

const char* BodyLabel(const std::string& body)
{
    static const std::map<std::string, const char*> labels = {{"Frame", "Frame (fixed)"},
                                                               {"Crankshaft", "Crankshaft (revolute, dynamic)"},
                                                               {"Rod", "Connecting rod (planar, dynamic)"},
                                                               {"Piston", "Piston (prismatic, dynamic)"},
                                                               {"Camshaft", "Camshaft (2:1 belt, kinematic)"},
                                                               {"IntakeValve", "Intake valve (cam follower)"},
                                                               {"ExhaustValve", "Exhaust valve (cam follower)"},
                                                               {"IntakeSpring", "Intake valve spring"},
                                                               {"ExhaustSpring", "Exhaust valve spring"}};
    auto it = labels.find(body);
    return it == labels.end() ? body.c_str() : it->second;
}

// ---- Panels ---------------------------------------------------------------------------------------

void SpecificationTree(EngineSession& session, int& selected)
{
    ImGui::Begin("Specification Tree");
    if (!session.scene)
    {
        ImGui::End();
        return;
    }
    const auto& parts = session.scene->Parts();
    const std::string product = session.document->Root().GetName() + " (product)";
    if (ImGui::TreeNodeEx(product.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const std::string& body : session.document->Bodies())
        {
            ImGui::PushID(body.c_str());
            const bool open = ImGui::TreeNodeEx(BodyLabel(body), body == "Frame" || body == "Crankshaft" || body == "Rod" || body == "Piston" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (open)
            {
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    if (parts[i].body != body || parts[i].role == "spark")
                        continue;
                    ImGui::PushID(int(i));
                    bool visible = parts[i].visible;
                    if (ImGui::Checkbox("##vis", &visible))
                        UserCommand("aiad_part_set_visible", {{"part", parts[i].name}, {"visible", visible}});
                    ImGui::SameLine();
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (selected == int(i))
                        flags |= ImGuiTreeNodeFlags_Selected;
                    ImGui::TreeNodeEx(parts[i].name.c_str(), flags);
                    if (ImGui::IsItemClicked())
                        selected = int(i);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::End();
}

void Properties(EngineSession& session, int selected)
{
    ImGui::Begin("Properties");
    if (!session.scene || selected < 0 || selected >= int(session.scene->Parts().size()))
    {
        ImGui::TextDisabled("Select a part in the viewport or in the specification tree.");
        ImGui::End();
        return;
    }
    const app::ScenePart& part = session.scene->Parts()[size_t(selected)];
    std::string error;
    const auto info = session.document->FindPart(part.path.GetString(), error);
    const auto material = session.document->FindMaterial(part.material);
    kernel::MassProperties mp;
    session.document->Mass(part.path, mp, error);
    ImGui::SeparatorText(part.name.c_str());
    if (ImGui::BeginTable("props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        auto row = [](const char* key, const char* fmt, auto... args) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(key);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, args...);
        };
        row("Body", "%s", BodyLabel(part.body));
        row("Material", "%s", part.material.c_str());
        row("Density", "%.0f kg/m^3", material ? material->density : 0.0);
        row("Volume", "%.2f cm^3", mp.volume * 1e6);
        row("Mass", "%.4f kg", mp.mass);
        row("Centre of mass (body)", "%.1f, %.1f, %.1f mm", mp.centroid.x * 1e3, mp.centroid.y * 1e3, mp.centroid.z * 1e3);
        row("Ixx, Iyy, Izz (CoM)", "%.3e, %.3e, %.3e kg m^2", mp.inertia[0], mp.inertia[1], mp.inertia[2]);
        if (!part.role.empty())
            row("Role in the study", "%s", part.role.c_str());
        ImGui::EndTable();
    }
    // The part's drawing: its feature history (edit and press Apply: one undoable command).
    if (info)
    {
        static std::string editing, editingPath;
        if (editingPath != part.path.GetString() || ImGui::Button("Reload"))
        {
            editing = info->features.dump(1);
            editingPath = part.path.GetString();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply features"))
        {
            const Json features = ParseJson(editing);
            if (features.is_discarded())
                g_lastError = "The features are not valid JSON.";
            else
                UserCommand("aiad_part_update", {{"part", part.path.GetString()}, {"features", features}});
        }
        if (!info->error.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1), "%s", info->error.c_str());
        ImGui::InputTextMultiline("##features", &editing, ImVec2(-1, std::max(120.0f, ImGui::GetContentRegionAvail().y - 8)));
    }
    ImGui::End();
}

void EnginePanel(EngineSession& session)
{
    ImGui::Begin("Engine");
    if (session.scene)
    {
        if (!session.studyError.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1), "Not simulated: %s", session.studyError.c_str());
        else
        {
            const auto& sd = session.study.simulation;
            const auto& g = sd.cylinder.geometry;
            ImGui::Text("Bore %.1f mm   Stroke %.1f mm   %.1f cm^3   CR %.1f   Rod %.1f mm", g.bore * 1e3, g.stroke * 1e3, g.DisplacedVolume() * 1e6,
                        g.compressionRatio, sd.rodLength * 1e3);
            ImGui::SeparatorText("Simulated physics, read from the drawing");
            ImGui::BulletText("Crankshaft + flywheel + magneto rotor: %.4f kg m^2 about the crank axis", sd.crankshaftInertia);
            ImGui::BulletText("Connecting rod: %.3f kg, CoM %.1f mm from the crank pin", sd.rodMass, sd.rodCentreOfMass * 1e3);
            ImGui::BulletText("Piston + rings + pin: %.3f kg", sd.pistonMass);
            ImGui::BulletText("Valve lift measured on the drawn lobes: %.2f / %.2f mm", *std::max_element(sd.intakeLiftTable.begin(), sd.intakeLiftTable.end()) * 1e3,
                              *std::max_element(sd.exhaustLiftTable.begin(), sd.exhaustLiftTable.end()) * 1e3);
            ImGui::BulletText("Magneto: points open %+.0f deg, magnet at crank %.0f deg", sd.ignition.pointsOpenDeg, sd.magnetMountDeg);
        }
        // The document's parameters: values or formulas. Enter applies (one undoable command that
        // regenerates the parts using them and the derived physics).
        ImGui::SeparatorText("Parameters (number or formula; Enter applies)");
        if (ImGui::BeginTable("##parameters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y))))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Definition", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableHeadersRow();
            for (const doc::Parameter& p : session.document->Parameters())
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.name.c_str());
                if (ImGui::IsItemHovered() && !p.description.empty())
                    ImGui::SetTooltip("%s", p.description.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(p.name.c_str());
                std::string text = p.value.is_string() ? p.value.get<std::string>() : std::format("{:g}", p.value.get<double>());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##v", &text, ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    char* end = nullptr;
                    const double number = std::strtod(text.c_str(), &end);
                    const Json value = end && *end == '\0' && !text.empty() ? Json(number) : Json(text);
                    UserCommand("aiad_parameters_set", {{"values", {{p.name, value}}}});
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.6g", session.document->Value(p.name));
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", p.unit.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
// Undo history and the audit trail: who (user or AI) did what.
void HistoryPanel()
{
    ImGui::Begin("History");
    ImGui::BeginDisabled(!g_bus->CanUndo());
    if (ImGui::Button("Undo (Ctrl+Z)"))
        g_bus->Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_bus->CanRedo());
    if (ImGui::Button("Redo (Ctrl+Y)"))
        g_bus->Redo();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Model changes");
    const auto history = g_bus->History();
    for (size_t i = 0; i < history.size(); ++i)
    {
        const bool done = i < g_bus->Depth();
        ImGui::TextColored(done ? (history[i].actor == "ai" ? ImVec4(1.0f, 0.6f, 0.25f, 1) : ImVec4(0.85f, 0.88f, 0.92f, 1)) : ImVec4(0.45f, 0.48f, 0.52f, 1),
                           "%s %s  [%s]", done ? "v" : "-", history[i].label.c_str(), history[i].actor.c_str());
    }
    if (history.empty())
        ImGui::TextDisabled("No change yet.");
    ImGui::SeparatorText("Audit trail (every command, newest first)");
    const auto& audit = g_bus->Audit();
    for (size_t k = 0; k < std::min<size_t>(audit.size(), 200); ++k)
    {
        const cmd::AuditEntry& e = audit[audit.size() - 1 - k];
        ImGui::TextColored(e.ok ? ImVec4(0.62f, 0.66f, 0.72f, 1) : ImVec4(0.95f, 0.36f, 0.32f, 1), "%s %s %s", e.actor == "ai" ? "AI  " : "user", e.command.c_str(),
                           e.ok ? "" : ("- " + e.error).c_str());
    }
    ImGui::End();
}

void SimulationPanel(EngineSession& session)
{
    ImGui::Begin("Simulation");
    if (session.scene)
    {
        if (ImGui::Button(session.running ? "  Stop  " : "  Start engine  "))
            UserCommand(session.running ? "aiad_sim_stop" : "aiad_sim_start", Json::object());
        ImGui::SameLine();
        float speed = float(session.timeScale);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("time scale", &speed, 0.002f, 1.0f, "%.3fx", ImGuiSliderFlags_Logarithmic))
            session.timeScale = speed;
        const auto& s = session.last;
        const double cycleDeg = std::fmod(std::fmod(s.crankDeg, 720.0) + 720.0, 720.0);
        ImVec4 colour;
        const char* phase = PhaseName(cycleDeg, colour);
        ImGui::Text("t = %.3f s   %.0f rpm   crank %.1f deg   ", s.time, s.rpm, cycleDeg);
        ImGui::SameLine();
        ImGui::TextColored(colour, "%s stroke", phase);
        if (s.time < session.study.simulation.starterDuration && session.running)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(starter engaged)");
        }
        const double omega = s.rpm * 2.0 * kPi / 60.0;
        ImGui::Text("Cylinder %.1f bar   gas torque %.1f N m", s.pressure / 1e5, s.gasTorque);
        ImGui::Text("Load %.2f kW   combustions %d", session.study.simulation.loadCoefficient * omega * omega * omega / 1e3, session.combustions);
        ImGui::Text("Valve lift: intake %.2f mm, exhaust %.2f mm", s.intakeLift * 1e3, s.exhaustLift * 1e3);
        ImGui::Text("Magneto secondary at the last points opening: %.1f kV", session.lastSecondaryKv);
    }
    ImGui::End();
}

void Plots(EngineSession& session)
{
    ImGui::Begin("Plots");
    if (!session.scene)
    {
        ImGui::End();
        return;
    }
    const auto& d = session.study.simulation;
    const double sparkAdvance = session.document->Value("spark_advance");
    const double cycleDeg = std::fmod(std::fmod(session.last.crankDeg, 720.0) + 720.0, 720.0);
    static std::vector<double> angles = [] {
        std::vector<double> a(720);
        for (int i = 0; i < 720; ++i)
            a[i] = i;
        return a;
    }();
    auto events = [&]() {
        const double tdc[] = {0.0, 360.0, 720.0}, bdc[] = {180.0, 540.0};
        ImPlot::PlotInfLines("TDC", tdc, 3, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.75f, 0.75f, 0.75f, 0.6f)));
        ImPlot::PlotInfLines("BDC", bdc, 2, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.5f, 0.5f, 0.5f, 0.5f)));
        const double spark = 720.0 - sparkAdvance;
        ImPlot::PlotInfLines("Spark", &spark, 1, ImPlotSpec(ImPlotProp_LineColor, ImVec4(1.0f, 0.85f, 0.3f, 1.0f)));
        ImPlot::TagX(d.intake.openDeg, ImVec4(0.35f, 0.65f, 1, 1), "IVO");
        ImPlot::TagX(d.intake.closeDeg, ImVec4(0.35f, 0.65f, 1, 1), "IVC");
        ImPlot::TagX(d.exhaust.openDeg, ImVec4(1, 0.55f, 0.25f, 1), "EVO");
        ImPlot::TagX(d.exhaust.closeDeg, ImVec4(1, 0.55f, 0.25f, 1), "EVC");
        ImPlot::PlotInfLines("Now", &cycleDeg, 1, ImPlotSpec(ImPlotProp_LineColor, ImVec4(1, 1, 1, 0.9f), ImPlotProp_LineWeight, 2.0f));
    };
    // ImPlot's own grid (plots inside ImGui tables do not draw their items with Dear ImGui 1.93).
    if (ImPlot::BeginSubplots("##plots", 2, 2, ImGui::GetContentRegionAvail(), ImPlotSubplotFlags_NoTitle))
    {
        const ImVec2 h(-1, -1);
        if (ImPlot::BeginPlot("Cylinder pressure", h))
        {
            ImPlot::SetupAxes("Crank angle [deg]", "Pressure [bar]", 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 720, ImPlotCond_Always);
            ImPlot::PlotLine("p", angles.data(), session.pressureByAngle.data(), 720,
                             ImPlotSpec(ImPlotProp_LineColor, ImVec4(1.0f, 0.62f, 0.2f, 1), ImPlotProp_LineWeight, 2.0f));
            events();
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Valve lift (measured on the drawn cam lobes)", h))
        {
            ImPlot::SetupAxes("Crank angle [deg]", "Lift [mm]");
            ImPlot::SetupAxesLimits(0, 720, 0, 10, ImPlotCond_Always);
            static std::vector<double> inMm, exMm;
            inMm.resize(720);
            exMm.resize(720);
            for (int i = 0; i < 720; ++i)
            {
                inMm[i] = (session.study.intakeLift.empty() ? 0.0 : session.study.intakeLift[i]) * 1e3;
                exMm[i] = (session.study.exhaustLift.empty() ? 0.0 : session.study.exhaustLift[i]) * 1e3;
            }
            ImPlot::PlotLine("Intake", angles.data(), inMm.data(), 720, ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.35f, 0.65f, 1, 1), ImPlotProp_LineWeight, 2.0f));
            ImPlot::PlotLine("Exhaust", angles.data(), exMm.data(), 720, ImPlotSpec(ImPlotProp_LineColor, ImVec4(1, 0.55f, 0.25f, 1), ImPlotProp_LineWeight, 2.0f));
            events();
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("p-V diagram (last cycle)", h))
        {
            ImPlot::SetupAxes("Volume [cm^3]", "Pressure [bar]");
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            ImPlot::SetupAxesLimits(20, 500, 0.3, 120, ImPlotCond_Once);
            if (!session.pvShownVolume.empty())
                ImPlot::PlotLine("cycle", session.pvShownVolume.data(), session.pvShownPressure.data(), int(session.pvShownVolume.size()),
                                 ImPlotSpec(ImPlotProp_LineColor, ImVec4(1.0f, 0.62f, 0.2f, 1), ImPlotProp_LineWeight, 2.0f));
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Speed and gas torque", h))
        {
            ImPlot::SetupAxes("Time [s]", "Speed [rpm]", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "Gas torque [N m]", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            if (!session.historyTime.empty())
            {
                std::vector<double> t(session.historyTime.begin(), session.historyTime.end());
                std::vector<double> rpm(session.historyRpm.begin(), session.historyRpm.end());
                std::vector<double> torque(session.historyTorque.begin(), session.historyTorque.end());
                ImPlot::PlotLine("rpm", t.data(), rpm.data(), int(t.size()), ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.4f, 0.85f, 0.5f, 1), ImPlotProp_LineWeight, 2.0f));
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::PlotLine("torque", t.data(), torque.data(), int(t.size()), ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.85f, 0.85f, 0.4f, 0.8f)));
            }
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
    ImGui::End();
}

// Axis triad in the lower-left corner of the viewport image.
void DrawCompass(const OrbitCamera& camera, ImVec2 imageMin, ImVec2 imageMax)
{
    GfVec3d right, up, back;
    camera.Axes(right, up, back);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(imageMin.x + 52, imageMax.y - 52);
    const float length = 36.0f;
    struct Axis
    {
        GfVec3d dir;
        ImU32 colour;
        const char* label;
    } axes[] = {{GfVec3d(1, 0, 0), IM_COL32(230, 80, 70, 255), "X"},
                {GfVec3d(0, 1, 0), IM_COL32(110, 200, 90, 255), "Y"},
                {GfVec3d(0, 0, 1), IM_COL32(80, 140, 240, 255), "Z"}};
    // Back-to-front so that the axis pointing at the viewer is drawn last.
    std::sort(std::begin(axes), std::end(axes), [&](const Axis& a, const Axis& b) { return GfDot(a.dir, back) < GfDot(b.dir, back); });
    draw->AddCircleFilled(centre, 44.0f, IM_COL32(20, 24, 32, 90));
    for (const Axis& a : axes)
    {
        const ImVec2 tip(centre.x + float(GfDot(a.dir, right)) * length, centre.y - float(GfDot(a.dir, up)) * length);
        draw->AddLine(centre, tip, a.colour, 2.5f);
        draw->AddText(ImVec2(tip.x - 4, tip.y - 7), a.colour, a.label);
    }
}

void BuildDefaultLayout(ImGuiID dockspace)
{
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
    // | tree, history | viewport (conversations open beside it) | AI hub |
    // |               | plots, engine...                        |        |
    ImGuiID centre = dockspace;
    const ImGuiID hub = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.19f, nullptr, &centre);
    const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.19f, nullptr, &centre);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.36f, nullptr, &centre);
    ImGuiID leftTop = left;
    const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.40f, nullptr, &leftTop);
    ImGui::DockBuilderDockWindow("Specification Tree", leftTop);
    ImGui::DockBuilderDockWindow("Properties", leftBottom);
    ImGui::DockBuilderDockWindow("History", leftBottom);
    ImGui::DockBuilderDockWindow("Viewport", centre);
    ImGui::DockBuilderDockWindow("Plots", bottom);
    ImGui::DockBuilderDockWindow("Engine", bottom);
    ImGui::DockBuilderDockWindow("Simulation", bottom);
    ImGui::DockBuilderDockWindow("AI Hub", hub);
    ImGui::DockBuilderFinish(dockspace);
}

void ApplyStyle(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 2.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 2.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 5);
    // ImPlot draws thick lines with the atlas' baked line UVs, which go stale with the dynamic font
    // atlas of Dear ImGui 1.92+ (the plot lines become invisible); use plain geometry instead.
    style.AntiAliasedLinesUseTex = false;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.24f, 0.38f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.33f, 0.52f, 0.7f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.42f, 0.66f, 0.8f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.46f, 0.9f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.41f, 0.62f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.34f, 0.52f, 1.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// The engine drawing: Dist/models next to the executable, else the repository's models folder.
std::string ModelPath()
{
    return Resource("models/Engine_SingleCylinder.usda", AIAD_MODELS_DIR "/Engine_SingleCylinder.usda");
}

// Opens the drawing and derives the study; the magneto table comes from the drawn magneto geometry.
bool LoadEngine(EngineSession& session, const std::string& magnetoCache, std::string& error)
{
    session.document = doc::Document::Open(ModelPath(), error);
    if (!session.document)
        return false;
    if (!session.Build())
    {
        error = session.studyError;
        return false;
    }
    session.magnetoTable = sim::magneto::CachedMagnetoTable(magnetoCache.c_str(), session.study.magneto, {}, 45.0, 1.0);
    session.Restart();
    return true;
}

// Verification mode (no window): open the drawing, run the engine for 2 s, render the posed scene.
int Snapshot(const char* pngPath, const std::string& magnetoCache, const std::string& environment)
{
    EngineSession session;
    std::string error;
    if (!LoadEngine(session, magnetoCache, error))
    {
        std::printf("cannot load the engine drawing: %s\n", error.c_str());
        return 1;
    }
    const auto& sd = session.study.simulation;
    std::printf("read from the drawing (%zu parts): crank+flywheel inertia %.4f kg m^2, rod %.3f kg (CoM %.1f mm), piston %.3f kg, "
                "max lift %.2f/%.2f mm\n",
                session.document->Parts().size(), sd.crankshaftInertia, sd.rodMass, sd.rodCentreOfMass * 1e3, sd.pistonMass,
                *std::max_element(sd.intakeLiftTable.begin(), sd.intakeLiftTable.end()) * 1e3,
                *std::max_element(sd.exhaustLiftTable.begin(), sd.exhaustLiftTable.end()) * 1e3);
    session.running = true;
    session.timeScale = 1.0;
    while (session.last.time < 2.0)
        session.Advance(0.05, 0.0);
    std::printf("engine at t = %.2f s: %.0f rpm, crank %.1f deg\n", session.last.time, session.last.rpm,
                std::fmod(session.last.crankDeg, 720.0));

    app::DocumentScene scene(*session.document, environment);
    std::printf("view of the drawing: %zu parts tessellated in %.2f s\n", scene.Parts().size(), scene.TessellationSeconds());
    scene.SetMotion(session.Motion());    render::HydraViewport viewport;
    viewport.SetStage(session.document->Stage());
    // A width whose rows are not 256-byte aligned on the GPU (1200 x 8 bytes): catches read-back stride bugs.
    viewport.Resize(1200, 675);
    viewport.SetCamera(OrbitCamera{}.CameraToWorld(), 35.0, 0.01, 20.0);
    // Interactive pattern: one pose update, one render and one read-back per frame while running.
    int valid = 0;
    session.timeScale = 0.02;
    // Frame-phase timing (mean and worst over the animated frames) [ms].
    const char* phaseNames[] = {"simulation", "pose", "camera", "render", "read-back"};
    double phaseSum[5] = {}, phaseMax[5] = {};
    auto clock = [] { return std::chrono::steady_clock::now(); };
    for (int f = 0; f < 60; ++f)
    {
        auto t0 = clock();
        session.Advance(1.0 / 60.0, f / 60.0);
        auto t1 = clock();
        scene.SetMotion(session.Motion());
        scene.SetSparkVisible(f / 60.0 < session.sparkUntil);
        auto t2 = clock();
        viewport.SetCamera(OrbitCamera{}.CameraToWorld(), 35.0, 0.01, 20.0);
        auto t3 = clock();
        // What the user does while it runs: hide/show a moving part, select, toggle the section.
        const int movingPart = int(std::find_if(scene.Parts().begin(), scene.Parts().end(),
                                                [](const app::ScenePart& p) { return p.body == "Crankshaft"; }) -
                                   scene.Parts().begin());
        if (f == 10)
            scene.SetVisible(movingPart, false);
        if (f == 20)
            scene.SetVisible(movingPart, true);
        if (f == 30)
            scene.SetHighlight(movingPart);
        if (f == 40)
            scene.SetHighlight(-1);
        if (f == 45 || f == 50)
            scene.SetSectionView(f == 50);
        viewport.Render();
        auto t4 = clock();
        const render::ViewportImage& img = viewport.ReadImage();
        auto t5 = clock();
        valid += img.width == 1200 && img.height == 675 && !img.rgba.empty();
        const double renderMs = std::chrono::duration<double, std::milli>(t4 - t3).count();
        if (renderMs > 50.0)
            std::printf("slow render frame %d: %.0f ms (spark %d, event %s)\n", f, renderMs, int(f / 60.0 < session.sparkUntil),
                        f == 10 ? "hide" : f == 20 ? "show" : f == 30 ? "highlight" : f == 40 ? "unhighlight" : f == 45 || f == 50 ? "section" : "-");
        const decltype(t0) marks[] = {t0, t1, t2, t3, t4, t5};
        for (int k = 0; k < 5; ++k)
        {
            const double ms = std::chrono::duration<double, std::milli>(marks[k + 1] - marks[k]).count();
            phaseSum[k] += ms;
            phaseMax[k] = std::max(phaseMax[k], ms);
        }
    }
    std::printf("animated frames with a valid image: %d of 60\n", valid);
    for (int k = 0; k < 5; ++k)
        std::printf("frame phase %-10s mean %6.2f ms, worst %7.2f ms\n", phaseNames[k], phaseSum[k] / 60.0, phaseMax[k]);
    {
        // What the user sees while the engine runs (no accumulation).
        const render::ViewportImage& frame = viewport.ReadImage();
        const std::string animated = std::filesystem::path(pngPath).replace_filename("snapshot_animated.png").string();
        stbi_write_png(animated.c_str(), frame.width, frame.height, 4, frame.rgba.data(), frame.width * 4);
    }
    if (valid != 60)
        return 1;
    int frames = 0;
    for (; frames < 256 && !(frames > 8 && viewport.IsConverged()); ++frames)
        viewport.Render();
    const render::ViewportImage& image = viewport.ReadImage();
    const bool written = stbi_write_png(pngPath, image.width, image.height, 4, image.rgba.data(), image.width * 4) != 0;
    std::printf("rendered %d frames, %s %s\n", frames, written ? "wrote" : "FAILED to write", pngPath);
    return written ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(WriteCrashDump);
    // Runtime layout next to the executable (Dist): usd/ with the plugInfo folders; when running from
    // the build tree, the development folders under deps/ are used instead. Static OpenUSD needs its
    // plugInfo folders before the first stage is opened.
    _putenv_s("PXR_PLUGINPATH_NAME", Resource("usd", AIAD_USD_PLUGIN_DIR).c_str());
    render::RegisterPlugins({Resource("usd/hdUsdBrep", AIAD_HDUSDBREP_PLUGIN_DIR),
                             Resource("usd/omniSolid/resources", AIAD_OMNISOLID_RESOURCES_DIR),
                             Resource("usd/hdAurora/resources", AIAD_HDAURORA_RESOURCES_DIR)});
    // The kernel reads the omniSolid schema folder from this variable (CRT copy, set before any stage).
    _putenv_s("OMNISOLID_PLUGIN_PATH", Resource("usd/omniSolid/resources", AIAD_OMNISOLID_RESOURCES_DIR).c_str());
    kernel::Initialize();
    const std::string magnetoCache = (ExeDir() / "magneto_table.txt").string();
    const std::string environment = (ExeDir() / "studio_env.hdr").string();
    app::WriteStudioEnvironment(environment);
    if (argc == 3 && std::string(argv[1]) == "--snapshot")
        return Snapshot(argv[2], magnetoCache, environment);

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"TootegaAIAD", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Tootega AIAD - Engine_SingleCylinder", WS_OVERLAPPEDWINDOW, 60, 40,
                              int(1700 * scale), int(1000 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_d3d.Create(hwnd))
    {
        std::fprintf(stderr, "DirectX 12 device creation failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    const bool uiTest = argc == 3 && std::string(argv[1]) == "--ui-test";
    // v3: conversations are tabs opened from the hub (no fixed chat panel). The UI test always starts from the default layout.
    static const std::string iniPath = (ExeDir() / "aiad_layout_v3.ini").string();
    const bool firstRun = uiTest || !std::filesystem::exists(iniPath);
    io.IniFilename = uiTest ? nullptr : iniPath.c_str();
    ApplyStyle(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_InitInfo init = {};
    init.Device = g_d3d.Device();
    init.CommandQueue = g_d3d.Queue();
    init.NumFramesInFlight = app::kFramesInFlight;
    init.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init.SrvDescriptorHeap = g_d3d.SrvHeap();
    init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) { g_d3d.AllocSrv(c, g); };
    init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE g) { g_d3d.FreeSrv(c, g); };
    ImGui_ImplDX12_Init(&init);

    // Fonts: Segoe UI (accents, symbols merged from Segoe UI Symbol), bold, Cascadia Mono / Consolas.
    app::UiFonts fonts;
    fonts.size = 15.0f;
    {
        const std::filesystem::path dir = L"C:\\Windows\\Fonts";
        auto exists = [&](const wchar_t* f) { return std::filesystem::exists(dir / f); };
        if (exists(L"segoeui.ttf"))
        {
            fonts.regular = io.Fonts->AddFontFromFileTTF((dir / "segoeui.ttf").string().c_str(), fonts.size);
            if (exists(L"seguisym.ttf"))
            {
                ImFontConfig merge;
                merge.MergeMode = true;
                io.Fonts->AddFontFromFileTTF((dir / "seguisym.ttf").string().c_str(), fonts.size, &merge);
            }
            fonts.bold = exists(L"seguisb.ttf") ? io.Fonts->AddFontFromFileTTF((dir / "seguisb.ttf").string().c_str(), fonts.size) : fonts.regular;
            const wchar_t* mono = exists(L"CascadiaMono.ttf") ? L"CascadiaMono.ttf" : L"consola.ttf";
            fonts.mono = io.Fonts->AddFontFromFileTTF((dir / mono).string().c_str(), fonts.size - 1.0f);
        }
        if (!fonts.regular)
            fonts.regular = fonts.bold = fonts.mono = io.Fonts->AddFontDefault();
        io.FontDefault = fonts.regular;
    }

    // Command Bus + engine commands; loopback bridge for the agent's MCP server.
    MainThreadQueue mainQueue;
    cmd::CommandBus bus;
    g_bus = &bus;
    app::SimulationJobs simulationJobs;
    app::AppContext appContext;
    bridge::BridgeServer bridgeServer;
    ChatTextures chatTextures;

    // The drawing: opened at once; its study is derived from it. The magneto field table (from the drawn
    // magneto geometry) is computed in the background (cached; about 70 s the first time).
    EngineSession session;
    std::string loadError;
    session.document = doc::Document::Open(ModelPath(), loadError);
    if (session.document)
        session.Build();
    bool documentChanged = false;
    if (session.document)
        session.document->OnChanged = [&] { documentChanged = true; };
    std::atomic<bool> magnetoReady = false;
    std::thread magnetoThread([&] {
        if (session.document && session.studyError.empty())
            session.magnetoTable = sim::magneto::CachedMagnetoTable(magnetoCache.c_str(), session.study.magneto, {}, 45.0, 1.0);
        magnetoReady = true;
    });

    std::unique_ptr<render::HydraViewport> viewport;
    app::UploadTexture texture;
    OrbitCamera camera;
    int selected = -1;
    bool sceneReady = false, layoutBuilt = !firstRun;
    const bool autoRun = argc == 2 && std::string(argv[1]) == "--run";
    const auto start = std::chrono::steady_clock::now();
    double previousTime = 0.0;

    appContext.session = &session;
    appContext.camera = &camera;
    appContext.selected = &selected;

    appContext.snapshot = [&] { return viewport ? EncodePng(viewport->ReadImage()) : std::vector<uint8_t>(); };
    doc::RegisterDocumentCommands(bus, [&] { return session.document.get(); });
    app::RegisterEngineCommands(bus, appContext, simulationJobs);

    std::string bridgeError;
    auto bridgeHandler = [&](const Json& request) -> Json {
        const std::string method = request.value("method", "");
        if (method == "catalog")
            return {{"ok", true}, {"result", bus.CatalogJson()}};
        if (method != "execute")
            return {{"ok", false}, {"error", "unknown bridge method '" + method + "' (catalog | execute)"}};
        const cmd::CommandOutput out = bus.Execute(request.value("name", ""), request.value("args", Json::object()), "ai");
        Json images = Json::array();
        for (const cmd::Image& image : out.images)
            images.push_back({{"mimeType", image.mimeType}, {"data", Base64Encode(image.bytes.data(), image.bytes.size())}});
        return {{"ok", out.ok}, {"text", out.text}, {"result", out.result}, {"error", out.error}, {"images", images}};
    };
    // The agent works where the model is: its folder is the workspace (also for aiad-mcp discovery).
    const std::filesystem::path modelFolder = std::filesystem::absolute(std::filesystem::path(Widen(ModelPath()))).parent_path();
    if (!bridgeServer.Start(mainQueue, bridgeHandler, modelFolder, bridgeError))
        std::fprintf(stderr, "agent bridge: %s\n", bridgeError.c_str());

    app::AiHost aiHost;
    aiHost.bus = &bus;
    aiHost.workspace = Narrow(modelFolder.wstring());
    aiHost.bridgeUrl = bridgeServer.Url();
    aiHost.bridgeToken = bridgeServer.Token();
    aiHost.mcpExe = ExeDir() / "aiad-mcp.exe";
    aiHost.selectedPart = [&] { return session.scene && selected >= 0 ? session.scene->Parts()[size_t(selected)].name : std::string(); };
    aiHost.partNames = [&] {
        std::vector<std::string> names;
        if (session.scene)
            for (const app::ScenePart& p : session.scene->Parts())
                if (p.role != "spark")
                    names.push_back(p.name);
        return names;
    };
    aiHost.viewportPng = appContext.snapshot;
    aiHost.texture = [&](const std::string& base64, int& w, int& h) { return chatTextures.Get(base64, w, h); };
    app::AiPanels ai(aiHost, fonts);

    // The drawing changed (user, AI or undo): the view and the simulation follow it, right after the
    // command (so the next command already sees the derived physics) and once per frame (undo/redo).
    auto syncDocument = [&] {
        if (!documentChanged || !sceneReady)
            return;
        documentChanged = false;
        const bool wasRunning = session.running;
        session.scene->Refresh();
        session.Build();
        session.running = wasRunning && session.studyError.empty();
        if (selected >= int(session.scene->Parts().size()))
            selected = -1;
    };
    bus.OnExecuted = [&](const cmd::AuditEntry&) { syncDocument(); };

    // --ui-test: drive the real app (bridge + MCP + commands + chat fixture), capture the window.
    int uiFrame = 0, uiFailures = 0, uiCaptureFrames = -1;  // -1 = agent phase, then frames since the chat fixture
    std::thread uiTestThread;
    std::atomic<bool> uiTestDone = false;
    std::vector<std::string> uiTestLog;
    std::mutex uiTestMutex;

    for (bool done = false; !done;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            done = done || msg.message == WM_QUIT;
        }
        if (done)
            break;
        // Minimized or occluded: no drawing, but the agent's bridge calls and the CLI stream are still
        // served (an agent working while the window is hidden must not hang). The UI test always draws.
        if (!uiTest && (g_d3d.TestOccluded() || IsIconic(hwnd)))
        {
            mainQueue.Drain();
            ai.Pump();
            Sleep(10);
            continue;
        }
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double wallDt = std::min(now - previousTime, 0.1);
        previousTime = now;

        if (!sceneReady && magnetoReady && session.document)
        {
            magnetoThread.join();
            session.Restart();
            session.scene = std::make_unique<app::DocumentScene>(*session.document, environment);
            viewport = std::make_unique<render::HydraViewport>();
            viewport->SetStage(session.document->Stage());
            sceneReady = true;
            session.running = autoRun && session.studyError.empty();  // --run: start the engine as soon as the model is built
        }
        syncDocument();

        mainQueue.Drain();  // bridge requests from the agent run here, on the main thread
        ai.Pump();
        session.Advance(wallDt, now);

        if (uiTest && sceneReady && ++uiFrame == 8)
        {
            // The agent's path end to end: aiad-mcp.exe (stdio MCP) -> bridge -> Command Bus -> model.
            uiTestThread = std::thread([&] {
                auto log = [&](bool ok, const std::string& what) {
                    std::lock_guard lock(uiTestMutex);
                    std::printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
                    std::fflush(stdout);
                    uiFailures += ok ? 0 : 1;
                };
                ai::ClaudeProcess mcp;
                std::string error;
                if (!mcp.Start({ExeDir() / "aiad-mcp.exe", ExeDir(), {}, {{"AIAD_BRIDGE_URL", bridgeServer.Url()}, {"AIAD_BRIDGE_TOKEN", bridgeServer.Token()}}},
                               error))
                {
                    log(false, "start aiad-mcp.exe: " + error);
                    uiTestDone = true;
                    return;
                }
                int id = 0;
                auto rpc = [&](const char* method, const Json& params, int seconds = 30) -> Json {
                    mcp.WriteLine({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", method}, {"params", params}});
                    const auto t0 = std::chrono::steady_clock::now();
                    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds))
                    {
                        std::vector<Json> events;
                        bool exited = false;
                        unsigned long code = 0;
                        mcp.Poll(events, exited, code);
                        for (const Json& e : events)
                            if (e.value("id", -1) == id)
                                return e;
                        Sleep(5);
                    }
                    return Json();
                };
                auto call = [&](const char* tool, const Json& args) { return rpc("tools/call", {{"name", tool}, {"arguments", args}}, 60)["result"]; };
                auto text = [](const Json& r) { return r.is_object() && r.contains("content") ? r["content"][0].value("text", "") : std::string(); };

                const Json init = rpc("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()}, {"clientInfo", {{"name", "ui-test"}}}});
                log(init["result"]["serverInfo"]["name"] == "aiad", "MCP initialize");
                const Json tools = rpc("tools/list", Json::object())["result"]["tools"];
                log(tools.is_array() && tools.size() == bus.Catalog().size(), std::format("MCP tools/list = Command Bus catalog ({} tools)", tools.size()));
                log(text(call("aiad_parameters_get", Json::object())).find("| bore | 0.08 | 0.08 | m |") != std::string::npos, "the drawing's parameters: bore 0.08 m");
                const Json bad = call("aiad_parameters_set", {{"values", {{"bore", 0.3}}}});
                log(bad.value("isError", false) && text(bad).find("outside its range") != std::string::npos, "out-of-range bore refused: " + text(bad));
                const Json broken = call("aiad_parameters_set", {{"values", {{"rod_length", "stroke * unknown"}}}});
                log(broken.value("isError", false) && text(broken).find("unknown name") != std::string::npos, "formula with an unknown name refused");
                const Json set = call("aiad_parameters_set", {{"values", {{"bore", 0.084}}}});
                log(!set.value("isError", true) && set.contains("structuredContent") && set["structuredContent"]["regenerated"].size() > 20,
                    std::format("bore 84 mm regenerates the parts that depend on it ({} parts)", set.contains("structuredContent") ? set["structuredContent"]["regenerated"].size() : 0));
                const Json piston = call("aiad_part_get", {{"part", "Piston"}});
                log(!piston.value("isError", true) && text(piston).find("piston_r") != std::string::npos, "the AI reads a part's feature history");
                const Json derived = call("aiad_engine_get_derived", Json::object());
                const double displacement = derived.contains("structuredContent") ? derived["structuredContent"].value("displacement_cm3", 0.0) : 0.0;
                log(std::abs(displacement - 332.5) < 0.1, std::format("derived displacement {:.1f} cm^3 (pi/4 x 84^2 x 60 = 332.5)", displacement));
                const Json snapshot = call("aiad_viewport_snapshot", Json::object());
                const bool image = snapshot.contains("content") && snapshot["content"].size() == 2 && snapshot["content"][1]["type"] == "image";
                log(image, "viewport snapshot returns a PNG");
                const Json hide = call("aiad_part_set_visible", {{"part", "cam cover"}, {"visible", false}});
                log(!hide.value("isError", true), "hide by name: " + text(hide));
                const Json run = call("aiad_sim_run", {{"seconds", 2.5}});
                const int job = run.contains("structuredContent") ? run["structuredContent"].value("job", 0) : 0;
                Json status;
                for (int i = 0; i < 600 && job > 0; ++i)
                {
                    status = call("aiad_sim_job_status", {{"job", job}});
                    if (status.contains("structuredContent") && status["structuredContent"].value("state", "") != "running")
                        break;
                    Sleep(250);
                }
                const Json result = status.contains("structuredContent") ? status["structuredContent"].value("result", Json::object()) : Json::object();
                log(result.value("runs_on_its_own", false), "simulation job: " + text(status));
                const Json undo1 = call("aiad_undo", Json::object()), undo2 = call("aiad_undo", Json::object());
                log(text(call("aiad_parameters_get", Json::object())).find("| bore | 0.08 | 0.08 | m |") != std::string::npos, "two undos: bore back to 80 mm");
                const Json redo = call("aiad_redo", Json::object());
                log(text(call("aiad_history", Json::object())).find("Set bore = 0.084") != std::string::npos, "history lists the AI change");
                call("aiad_undo", Json::object());
                if (snapshot.contains("content") && snapshot["content"].size() == 2)
                {
                    std::lock_guard lock(uiTestMutex);
                    uiTestLog.push_back("#snapshot " + snapshot["content"][1].value("data", ""));
                }
                std::printf("stage: stopping aiad-mcp\n"), std::fflush(stdout);
                mcp.Stop();
                std::printf("stage: agent path done\n"), std::fflush(stdout);
                uiTestDone = true;
            });
        }
        if (uiTest && uiTestDone && uiCaptureFrames < 0)
        {
            uiTestThread.join();
            std::string snapshotBase64;
            {
                std::lock_guard lock(uiTestMutex);
                for (const std::string& line : uiTestLog)
                    if (line.starts_with("#snapshot "))
                        snapshotBase64 = line.substr(10);
            }
            // A conversation as the CLI streams it, rendered by the chat (no tokens spent).
            const std::vector<Json> fixture = {
                {{"type", "system"}, {"subtype", "init"}, {"session_id", "ui-test"}, {"model", "claude-opus-5-5[1m]"},
                 {"mcp_servers", {{{"name", "aiad"}, {"status", "connected"}}}}},
                {{"type", "assistant"}, {"message", {{"id", "m1"}, {"role", "assistant"}, {"usage", {{"input_tokens", 12}, {"cache_read_input_tokens", 182000}, {"cache_creation_input_tokens", 3100}}},
                   {"content", {{{"type", "text"}, {"text", "I'll read the current design first, then change the **bore** and check the result."}},
                                {{"type", "tool_use"}, {"id", "t1"}, {"name", "mcp__aiad__aiad_parameters_get"}, {"input", Json::object()}}}}}}},
                {{"type", "user"}, {"message", {{"role", "user"}, {"content", {{{"type", "tool_result"}, {"tool_use_id", "t1"},
                   {"content", "| Parameter | Definition | Value | Unit |\n|---|---|---|---|\n| bore | 0.08 | 0.08 | m |\n| stroke | 0.06 | 0.06 | m |"}}}}}}},
                {{"type", "assistant"}, {"message", {{"id", "m2"}, {"role", "assistant"},
                   {"content", {{{"type", "tool_use"}, {"id", "t2"}, {"name", "mcp__aiad__aiad_parameters_set"}, {"input", {{"values", {{"bore", 0.084}}}}}}}}}}},
                {{"type", "user"}, {"message", {{"role", "user"}, {"content", {{{"type", "tool_result"}, {"tool_use_id", "t2"},
                   {"content", "Set bore = 0.084. Regenerated 31 part(s): Piston, Cylinder liner, Cylinder barrel, ..."}}}}}}},
                {{"type", "assistant"}, {"message", {{"id", "m3"}, {"role", "assistant"},
                   {"content", {{{"type", "tool_use"}, {"id", "t3"}, {"name", "mcp__aiad__aiad_viewport_snapshot"}, {"input", Json::object()}}}}}}},
                {{"type", "user"}, {"message", {{"role", "user"}, {"content", {{{"type", "tool_result"}, {"tool_use_id", "t3"},
                   {"content", {{{"type", "text"}, {"text", "Viewport snapshot."}}, {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", "image/png"}, {"data", snapshotBase64}}}}}}}}}}}},
                {{"type", "assistant"}, {"message", {{"id", "m4"}, {"role", "assistant"},
                   {"content", {{{"type", "text"}, {"text", "Done. The bore is now **84 mm**:\n\n| | Before | After |\n|---|---|---|\n| Displacement | 301.6 cm^3 | 332.5 cm^3 |\n| Bore | 80 mm | 84 mm |\n\n"
                                                               "- The piston, rings, liner and head were regenerated from the new bore.\n- Run `aiad_sim_run` to see the effect on power.\n\nYou can undo it with **Ctrl+Z**."}}}}}}},
                {{"type", "result"}, {"subtype", "success"}, {"total_cost_usd", 0.412}, {"usage", {{"input_tokens", 40}, {"output_tokens", 820}, {"cache_read_input_tokens", 546000}, {"cache_creation_input_tokens", 9300}}}}};
            // No conversation is open until the user asks for one; "+ New session" opens its tab.
            const bool noneAtStart = ai.ChatCount() == 0;
            ai.OpenNewSession();
            std::printf("%s no conversation at start, + New session opens one tab\n", noneAtStart && ai.ChatCount() == 1 ? "ok   " : "FAIL ");
            uiFailures += noneAtStart && ai.ChatCount() == 1 ? 0 : 1;
            const bool inModelFolder = ai.Workspace() == Narrow(modelFolder.wstring()) && ai.Session() && ai.Session()->Options().cwd == modelFolder;
            std::printf("%s the agent works in the model's folder (%s)\n", inModelFolder ? "ok   " : "FAIL ", ai.Workspace().c_str());
            uiFailures += inModelFolder ? 0 : 1;
            ai.LoadFixture(fixture, "Increase the bore to 84 mm and show me the result.");
            std::printf("stage: chat fixture loaded\n"), std::fflush(stdout);
            uiCaptureFrames = 0;  // capture phase
        }

        // Undo / redo (not while typing).
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            bus.Undo();
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            bus.Redo();

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
        if (!layoutBuilt)
        {
            BuildDefaultLayout(dockspace);
            layoutBuilt = true;
        }
        ai.PrepareDocking();

        // ---- Viewport ----
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (!sceneReady)
        {
            ImGui::TextUnformatted("  Opening the drawing and computing the magneto field table (MFEM, first run only, about 70 s)...");
        }
        else
        {
            // Toolbar: standard views, fit all, section (below the tab bar, inside the content area).
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6, ImGui::GetCursorPosY() + 4));
            auto view = [&](const char* label, double yaw, double pitch) {
                if (ImGui::Button(label))
                {
                    camera.yaw = yaw;
                    camera.pitch = pitch;
                }
                ImGui::SameLine();
            };
            view("Iso", -45.0, 35.264);
            view("Front", -90.0, 0.0);
            view("Back", 90.0, 0.0);
            view("Right", 0.0, 0.0);
            view("Left", 180.0, 0.0);
            view("Top", -90.0, 89.9);
            if (ImGui::Button("Fit all"))
                camera.Fit(session.scene->WorldBounds());
            ImGui::SameLine();
            bool section = session.scene->SectionView();
            if (ImGui::Checkbox("Section", &section))
                session.scene->SetSectionView(section);
            ImGui::SameLine();
            ImGui::TextDisabled("  Space+LMB drag rotate |MMB pan | MMB+RMB rotate | wheel zoom | LMB select | Alt+LMB rotate");

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > 32 && avail.y > 32)
            {
                const int w = int(avail.x), h = int(avail.y);
                if (w != texture.Width() || h != texture.Height())
                {
                    g_d3d.WaitIdle();
                    texture.Destroy(g_d3d);
                    texture.Create(g_d3d, w, h);
                    viewport->Resize(w, h);
                }
                session.scene->SetMotion(session.Motion());
                session.scene->SetSparkVisible(now < session.sparkUntil);
                session.scene->SetHighlight(selected);
                viewport->SetCamera(camera.CameraToWorld(), camera.fovDeg, 0.005, 20.0);
                viewport->Render();
                const ImVec2 imageMin = ImGui::GetCursorScreenPos();
                ImGui::Image(ImTextureRef(ImTextureID(texture.Srv().ptr)), ImVec2(float(w), float(h)));
                const ImVec2 imageMax(imageMin.x + w, imageMin.y + h);
                DrawCompass(camera, imageMin, imageMax);
                if (ImGui::IsItemHovered())
                {
                    const bool mmb = ImGui::IsMouseDown(ImGuiMouseButton_Middle), rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                    const bool altLmb = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    // Space held (not while typing) + left button dragged: orbits the view.
                    const bool space = !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space);
                    const bool spaceLmb = space && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    if ((mmb && rmb) || altLmb || spaceLmb)
                    {
                        camera.yaw -= io.MouseDelta.x * 0.3;
                        camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * 0.3, -89.9, 89.9);
                    }
                    else if (mmb)
                        camera.Pan(io.MouseDelta.x, io.MouseDelta.y, h);
                    camera.distance = std::clamp(camera.distance * std::pow(0.88, io.MouseWheel), 0.05, 10.0);
                    // Left click (no drag, no Alt): select the part under the cursor.
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt && !space &&
                        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0)
                    {
                        GfVec3d origin, direction;
                        camera.Ray((io.MousePos.x - imageMin.x) / w, (io.MousePos.y - imageMin.y) / h, double(w) / h, origin, direction);
                        selected = session.scene->Pick(origin, direction).part;
                    }
                }
                if (selected >= 0)
                {
                    const char* name = session.scene->Parts()[size_t(selected)].name.c_str();
                    ImGui::GetWindowDrawList()->AddText(ImVec2(imageMin.x + 10, imageMax.y - 112), IM_COL32(255, 160, 40, 255), name);
                }
            }
        }
        ImGui::End();

        SpecificationTree(session, selected);
        Properties(session, selected);
        HistoryPanel();
        EnginePanel(session);
        SimulationPanel(session);
        Plots(session);
        ai.DrawHub();
        ai.DrawChats();
        if (!g_lastError.empty())
        {
            ImGui::OpenPopup("Command refused");
            if (ImGui::BeginPopupModal("Command refused", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::PushTextWrapPos(520);
                ImGui::TextUnformatted(g_lastError.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    g_lastError.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        ImGui::Render();
        const float clear[4] = {0.08f, 0.09f, 0.11f, 1.0f};
        ID3D12GraphicsCommandList* list = g_d3d.BeginFrame(clear);
        if (sceneReady && texture.Width() > 0)
        {
            const render::ViewportImage& image = viewport->ReadImage();
            if (image.width == texture.Width() && image.height == texture.Height())
                texture.Upload(g_d3d, list, image.rgba.data());
        }
        chatTextures.Upload(list);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        if (uiTest && uiCaptureFrames >= 0 && (++uiCaptureFrames == 60 || uiCaptureFrames == 120))
        {
            // Captures after the chat settled (60 frames: textures uploaded, layout stable): the chat,
            // then the same window with a question from the agent in its modal.
            const bool modal = uiCaptureFrames == 120;
            const std::string png = modal ? std::filesystem::path(argv[2]).replace_filename("ui_test_modal.png").string() : std::string(argv[2]);
            int w = 0, h = 0;
            const std::vector<uint8_t> pixels = g_d3d.EndFrameWithCapture(w, h);
            const bool written = stbi_write_png(png.c_str(), w, h, 4, pixels.data(), w * 4) != 0;
            std::printf("ui capture %dx%d %s %s\n", w, h, written ? "written to" : "FAILED", png.c_str());
            uiFailures += written ? 0 : 1;
            if (!modal)
                ai.Session()->Feed({{"type", "control_request"},
                                    {"request_id", "q1"},
                                    {"request",
                                     {{"subtype", "can_use_tool"},
                                      {"tool_name", "AskUserQuestion"},
                                      {"input",
                                       {{"questions",
                                         {{{"question", "Qual diâmetro você prefere para a próxima simulação?"},
                                           {"header", "Diâmetro"},
                                           {"multiSelect", false},
                                           {"options",
                                            {{{"label", "84 mm (Recomendado)"}, {"description", "+10% de cilindrada; as válvulas ainda cabem"}},
                                             {{"label", "88 mm"}, {"description", "+21%; exige válvulas maiores"}}}}},
                                          {{"question", "Quais saídas devo gerar?"},
                                           {"header", "Saídas"},
                                           {"multiSelect", true},
                                           {"options", {{{"label", "Snapshot"}, {"description", ""}}, {{"label", "Curva de potência"}, {"description", ""}}}}}}}}}}}});
            else
            {
                const bool asked = !ai.Session()->Pending().empty() && ai.Session()->Pending().front().questions.size() == 2;
                std::printf("%s question modal shows the agent's 2 questions\n", asked ? "ok   " : "FAIL ");
                uiFailures += asked ? 0 : 1;
                std::printf("ui-test: %s (%d failure(s))\n", uiFailures ? "FAIL" : "ok", uiFailures);
                done = true;
            }
            std::fflush(stdout);
        }
        else
            g_d3d.EndFrame();
    }

    if (uiTestThread.joinable())
        uiTestThread.join();
    if (magnetoThread.joinable())
        magnetoThread.join();
    bridgeServer.Stop();
    g_d3d.WaitIdle();
    chatTextures.Destroy();
    viewport.reset();
    texture.Destroy(g_d3d);
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    g_d3d.Destroy();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return uiTest ? (uiFailures ? 1 : 0) : 0;
}
