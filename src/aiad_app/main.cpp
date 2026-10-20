// Tootega AIAD: a document (the drawing = the simulated physics) in a Hydra viewport (HdAurora, DXR),
// driven live by the generic simulation of the machine it draws (Chrono mechanism + domain elements: 0D
// gas chambers, magnetos). The user and the AI agent (Claude Code through MCP -> bridge) operate it
// through the same Command Bus.
#include "AiPanels.h"
#include "Bridge.h"
#include "ClaudeProcess.h"
#include "CommandBus.h"
#include "D3DContext.h"
#include "AppCommands.h"
#include "DocCommands.h"
#include "DocumentScene.h"
#include "InstrumentPanels.h"
#include "Session.h"
#include "MainThreadQueue.h"
#include "Paths.h"
#include "Text.h"

#include "HydraViewport.h"
#include "magneto/MagnetoField.h"

#include <windows.h>
#include <dbghelp.h>
#include <shobjidl.h>

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
#include <set>

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
const app::RunCommand RunUserCommand = [](const std::string& name, const Json& args) { return UserCommand(name, args); };

// A font embedded in the executable (aiad.rc): its bytes, or null.
const void* EmbeddedFont(const wchar_t* name, int& size)
{
    HRSRC found = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    size = found ? int(SizeofResource(nullptr, found)) : 0;
    return loaded ? LockResource(loaded) : nullptr;
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

// Closing must never hang: once the window is closed, a watchdog gives the shutdown (threads, the agent's CLI, the
// renderer) a few seconds, then writes aiad_hang.dmp (every thread's stack, to find what waited) and ends the process.
void StartShutdownWatchdog()
{
    std::thread([] {
        Sleep(5000);
        const std::filesystem::path dump = ExeDir() / "aiad_hang.dmp";
        HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MINIDUMP_TYPE(MiniDumpWithThreadInfo), nullptr, nullptr, nullptr);
            CloseHandle(file);
        }
        std::fprintf(stderr, "shutdown took more than 5 s: %s written, process ended\n", dump.string().c_str());
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

// Folder next to the executable if present (deployed layout), otherwise the development folder.
std::string Resource(const char* relative, const char* development)
{
    const std::filesystem::path deployed = ExeDir() / relative;
    return std::filesystem::exists(deployed) ? deployed.string() : std::string(development);
}

bool g_closeRequested = false;  // window close button / Alt+F4 / File > Exit

// Windows' Open / Save As dialogs for documents. Returns the chosen path (UTF-8), empty if cancelled.
std::string DocumentDialog(HWND owner, bool save, const std::string& current)
{
    const COMDLG_FILTERSPEC filters[] = {{L"AIAD document (*.usda)", L"*.usda"}, {L"USD (*.usda;*.usdc;*.usd)", L"*.usda;*.usdc;*.usd"}};
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))))
        return {};
    dialog->SetFileTypes(save ? 1 : 2, filters);
    dialog->SetDefaultExtension(L"usda");
    if (!current.empty())
    {
        const std::filesystem::path path = Widen(current);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))))
        {
            dialog->SetFolder(folder);
            folder->Release();
        }
        if (save)
            dialog->SetFileName(path.filename().c_str());
    }
    std::string chosen;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
    {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
        {
            chosen = Narrow(name);
            CoTaskMemFree(name);
        }
        item->Release();
    }
    dialog->Release();
    return chosen;
}

// What the user arranged, kept between runs in <exe dir>\.aiad\aiadconfig.json: the panels (Dear ImGui's
// settings: docking, sizes, positions, tabs) and the main window's placement. None in tests.
class AppConfig
{
public:
    static constexpr int kLayoutVersion = 4;   // bump when the default panels change: old layouts are dropped

    explicit AppConfig(std::filesystem::path file) : m_file(std::move(file))
    {
        if (m_file.empty())
            return;
        const Json data = ParseJson(ReadTextFile(m_file));
        if (data.is_object())
            m_data = data;
    }
    // Dear ImGui settings (its ini text); empty = build the default layout.
    std::string Layout() const
    {
        const auto layout = m_data.find("layout");
        const auto version = m_data.find("layout_version");
        if (layout == m_data.end() || !layout->is_string() || version == m_data.end() || *version != kLayoutVersion)
            return {};
        return layout->get<std::string>();
    }
    void SetLayout(std::string ini)
    {
        m_data["layout_version"] = kLayoutVersion;
        m_data["layout"] = std::move(ini);
    }
    // Main window: normal-state rectangle and whether it was maximized.
    bool Window(RECT& rect, bool& maximized) const
    {
        const auto w = m_data.find("window");
        if (w == m_data.end() || !w->is_object() || !w->contains("rect") || !(*w)["rect"].is_array() || (*w)["rect"].size() != 4)
            return false;
        const Json& r = (*w)["rect"];
        for (const Json& v : r)
            if (!v.is_number_integer())
                return false;
        rect = {r[0].get<LONG>(), r[1].get<LONG>(), r[2].get<LONG>(), r[3].get<LONG>()};
        maximized = !w->contains("maximized") || !(*w)["maximized"].is_boolean() || (*w)["maximized"].get<bool>();
        return rect.right - rect.left > 200 && rect.bottom - rect.top > 150;
    }
    void SetWindow(const RECT& rect, bool maximized)
    {
        m_data["window"] = {{"rect", {rect.left, rect.top, rect.right, rect.bottom}}, {"maximized", maximized}};
    }
    // Where the last AI conversation window was (see AiPanels::ChatPlacement).
    Json ChatWindow() const
    {
        const auto c = m_data.find("chat_window");
        return c == m_data.end() ? Json() : *c;
    }
    void SetChatWindow(const Json& placement)
    {
        if (!placement.is_null())
            m_data["chat_window"] = placement;
    }
    void Save() const
    {
        if (m_file.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(m_file.parent_path(), ec);
        WriteTextFile(m_file, m_data.dump(2));
    }

private:
    std::filesystem::path m_file;
    Json m_data = Json::object();
};

// File > Open Recent: newest first, kept in %APPDATA%\Tootega\AIAD\recent_documents.json (none in tests).
class RecentDocuments
{
public:
    explicit RecentDocuments(std::filesystem::path file) : m_file(std::move(file))
    {
        if (m_file.empty())
            return;
        const Json list = ParseJson(ReadTextFile(m_file));
        if (list.is_array())
            for (const Json& item : list)
                if (item.is_string())
                    m_items.push_back(item.get<std::string>());
    }
    const std::vector<std::string>& Items() const { return m_items; }
    void Add(const std::string& path)
    {
        const std::string full = Narrow(std::filesystem::absolute(Widen(path)).wstring());
        std::erase_if(m_items, [&](const std::string& p) { return _stricmp(p.c_str(), full.c_str()) == 0; });
        m_items.insert(m_items.begin(), full);
        if (m_items.size() > 10)
            m_items.resize(10);
        Save();
    }
    void Remove(const std::string& path)
    {
        std::erase(m_items, path);
        Save();
    }
    void Clear()
    {
        m_items.clear();
        Save();
    }

private:
    void Save() const
    {
        if (!m_file.empty())
            WriteTextFile(m_file, Json(m_items).dump(2));
    }
    std::filesystem::path m_file;
    std::vector<std::string> m_items;
};

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
    case WM_CLOSE:
        g_closeRequested = true;  // the main loop asks about unsaved changes first
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

using app::Session;
using app::OrbitCamera;


// ---- Panels ---------------------------------------------------------------------------------------

void SpecificationTree(Session& session, int& selected)
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
            const std::string label = body + (session.document->BodyFixed(body) ? " (fixed)" : "");
            const bool open = ImGui::TreeNodeEx(label.c_str(), 0);
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
                    if (ImGui::BeginPopupContextItem("measure"))   // right click: what can be measured on it
                    {
                        selected = int(i);
                        app::MeasurePartMenu(session, parts[i].name, parts[i].body, RunUserCommand);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // The mechanism: joints and the elements tied to them.
        const std::vector<doc::JointInfo> joints = session.document->Joints();
        if (!joints.empty() && ImGui::TreeNodeEx(std::format("Joints ({})", joints.size()).c_str(), 0))
        {
            for (const doc::JointInfo& j : joints)
            {
                ImGui::BulletText("%s: %s, %s - %s%s", j.name.c_str(), j.type.c_str(), j.body0.c_str(), j.body1.empty() ? "ground" : j.body1.c_str(),
                                  j.motor.is_null() ? "" : ", motor");
                // Right click: plug a measuring instrument into it.
                const std::string popup = "plug:joint:" + j.name;
                ImGui::OpenPopupOnItemClick(popup.c_str(), ImGuiPopupFlags_MouseButtonRight);
                if (ImGui::BeginPopup(popup.c_str()))
                {
                    app::PlugInstrumentMenu(session, "joint", j.name, j.type != "prismatic", RunUserCommand);
                    ImGui::EndPopup();
                }
            }
            ImGui::TreePop();
        }
        const std::vector<doc::ElementInfo> elements = session.document->Elements();
        if (!elements.empty() && ImGui::TreeNodeEx(std::format("Elements ({})", elements.size()).c_str(), 0))
        {
            for (const doc::ElementInfo& e : elements)
            {
                ImGui::BulletText("%s: %s", e.name.c_str(), e.type.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", e.definition.dump(1).c_str());
                const std::string popup = "plug:element:" + e.name;
                ImGui::OpenPopupOnItemClick(popup.c_str(), ImGuiPopupFlags_MouseButtonRight);
                if (ImGui::BeginPopup(popup.c_str()))
                {
                    app::PlugInstrumentMenu(session, e.type, e.name, true, RunUserCommand);
                    ImGui::EndPopup();
                }
            }
            ImGui::TreePop();
        }
        // The measuring instruments: shown or hidden.
        const std::vector<doc::InstrumentInfo> instruments = session.document->Instruments();
        if (!instruments.empty() && ImGui::TreeNodeEx(std::format("Instruments ({})", instruments.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const doc::InstrumentInfo& i : instruments)
            {
                ImGui::PushID(i.name.c_str());
                bool visible = i.definition.value("visible", true);
                if (ImGui::Checkbox("##shown", &visible))
                    UserCommand("aiad_instrument_show", {{"name", i.name}, {"visible", visible}});
                ImGui::SameLine();
                const doc::InstrumentType* t = doc::FindInstrumentType(i.type);
                ImGui::Text("%s  %s", t ? t->model : "", i.name.c_str());
                if (ImGui::BeginPopupContextItem("instrument"))
                {
                    if (ImGui::MenuItem("Delete"))
                        UserCommand("aiad_instrument_delete", {{"name", i.name}});
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    ImGui::End();
}

// ---- Adjusters: parts set by hand (a regulator, a screw, a lever), moved with a control on the part ----

// The adjuster that moves `body`, if any.
const app::MechanismStudy::Adjuster* AdjusterOf(const Session& session, const std::string& body)
{
    for (const app::MechanismStudy::Adjuster& a : session.study.adjusters)
        if (a.body == body)
            return &a;
    return nullptr;
}

// Being dragged (the view or the Properties panel): the adjuster's joint and where it is now.
struct AdjustDrag
{
    int joint = -1;
    double value = 0.0;
};
AdjustDrag g_adjustDrag;

// While dragged the machine shows it at once; let go, the drawing changes (one undoable aiad_adjust).
void AdjustTo(Session& session, const app::MechanismStudy::Adjuster& a, double value, bool release)
{
    value = std::clamp(value, a.lower, a.upper);
    if (!release)
    {
        if (g_adjustDrag.joint != a.joint || g_adjustDrag.value != value)
            session.PreviewAdjust(a.joint, value);
        g_adjustDrag = {a.joint, value};
        return;
    }
    g_adjustDrag = {};
    if (value != a.value)
        UserCommand("aiad_adjust", {{"joint", a.name}, {"value", value}});
}

void Properties(Session& session, int selected)
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
        row("Body", "%s%s", part.body.c_str(), session.document->BodyFixed(part.body) ? " (fixed)" : "");
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
    // Set by hand (an adjuster): the same control as on the part in the view.
    if (const app::MechanismStudy::Adjuster* a = AdjusterOf(session, part.body))
    {
        ImGui::SeparatorText("Set by hand");
        const double scale = a->rotation ? 180.0 / kPi : 1e3;
        float value = float((g_adjustDrag.joint == a->joint ? g_adjustDrag.value : a->value) * scale);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##adjust", &value, float(a->lower * scale), float(a->upper * scale), a->rotation ? "%.2f deg" : "%.3f mm"))
            AdjustTo(session, *a, value / scale, false);
        if (ImGui::IsItemDeactivatedAfterEdit())
            AdjustTo(session, *a, value / scale, true);
        ImGui::TextDisabled("%s: %s %s; held where it is set while the machine runs.", a->name.c_str(), a->body.c_str(), a->rotation ? "turns about its joint" : "slides along its joint");
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

// What the simulator found wrong in the drawing, each with its fix (the study refuses to run it).
void StudyFindingsView(const Session& session)
{
    const auto& findings = session.study.findings;
    ImGui::PushTextWrapPos(0.0f);
    if (findings.empty())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1), "Not simulated: %s", session.studyError.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1), "Not simulated: the simulator found %d problem(s) in the drawing.", session.study.Errors());
    for (size_t i = 0; i < findings.size(); ++i)
    {
        ImGui::Spacing();
        ImGui::TextColored(findings[i].error ? ImVec4(0.95f, 0.55f, 0.45f, 1) : ImVec4(0.98f, 0.75f, 0.25f, 1), "%zu. %s%s", i + 1,
                           findings[i].error ? "" : "(note) ", findings[i].problem.c_str());
        ImGui::Indent();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "Fix: %s", findings[i].fix.c_str());
        ImGui::Unindent();
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

// The machine the simulation is built from: bodies (mass from the parts), joints, elements, what was
// measured on the drawing, notes.
void MachineSummary(const Session& session)
{
    const app::MechanismStudy& m = session.study;
    const sim::mb::Model& model = m.machine.mechanism;
    ImGui::Text("%zu bodies, %zu joints, %zu couplings, %zu contacts, %zu springs, %zu chambers, %zu magnetos; %d degree(s) of freedom", model.bodies.size(),
                model.joints.size(), model.couplings.size(), model.contacts.size(), model.springs.size(), m.machine.chambers.size(), m.machine.magnetos.size(),
                m.mobility);
    ImGui::PushTextWrapPos(0.0f);
    for (const app::StudyFinding& f : m.findings)
    {
        ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.25f, 1), "Note: %s", f.problem.c_str());
        ImGui::TextDisabled("%s", f.fix.c_str());
    }
    if (!m.measured.empty())
    {
        ImGui::SeparatorText("Measured on the drawing");
        for (const std::string& line : m.measured)
            ImGui::BulletText("%s", line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::SeparatorText("Bodies (mass from the parts and their materials)");
    for (const sim::mb::Body& b : model.bodies)
        if (b.fixed)
            ImGui::BulletText("%s: fixed", b.name.c_str());
        else
            ImGui::BulletText("%s: %.3g kg, centre of mass (%.2f, %.2f, %.2f) mm", b.name.c_str(), b.mass, b.centroid[0] * 1e3, b.centroid[1] * 1e3,
                              b.centroid[2] * 1e3);
}

void StudyPanel(Session& session)
{
    ImGui::Begin("Study");
    if (session.scene)
    {
        if (!session.studyError.empty())
            StudyFindingsView(session);
        else
            MachineSummary(session);
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

void SimulationPanel(Session& session)
{
    ImGui::Begin("Simulation");
    if (session.scene && !session.studyError.empty())
        StudyFindingsView(session);
    else if (session.scene && session.machine)
    {
        if (ImGui::Button(session.running ? "  Stop  " : "  Run  "))
            UserCommand(session.running ? "aiad_sim_stop" : "aiad_sim_start", Json::object());
        ImGui::SameLine();
        float speed = float(session.timeScale);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("time scale", &speed, 0.002f, 1.0f, "%.3fx", ImGuiSliderFlags_Logarithmic))
            session.timeScale = speed;
        if (session.running && session.achievedScale > 0.0 && session.achievedScale < 0.9 * session.timeScale)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.25f, 1), "running at %.3fx (the computer cannot keep up with %.3fx)", session.achievedScale.load(),
                               session.timeScale.load());
        }
        const auto lock = session.Lock();
        if (!session.machine)
        {
            ImGui::End();
            return;
        }
        const sim::machine::Machine& machine = *session.machine;
        const sim::mb::Multibody& mb = machine.Mechanism();
        const sim::mb::Model& model = session.study.machine.mechanism;
        ImGui::Text("t = %.4f s   %d degree(s) of freedom", machine.Time(), session.study.mobility);
        if (ImGui::BeginTable("##joints", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Joint");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("Speed");
            ImGui::TableHeadersRow();
            static const char* types[] = {"revolute", "prismatic", "cylindrical", "spherical", "planar", "fixed"};
            for (size_t j = 0; j < model.joints.size(); ++j)
            {
                const sim::mb::Joint& joint = model.joints[j];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(joint.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(types[int(joint.type)]);
                if (joint.motor.mode == sim::mb::Motor::Mode::Torque && (joint.motor.duration <= 0.0 || machine.Time() < joint.motor.duration))
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(motor on)");
                }
                const double c = mb.Coordinate(int(j)), rate = mb.Rate(int(j));
                ImGui::TableNextColumn();
                if (joint.type == sim::mb::JointType::Prismatic)
                {
                    ImGui::Text("%.2f mm", c * 1e3);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f m/s", rate);
                }
                else if (joint.type == sim::mb::JointType::Revolute || joint.type == sim::mb::JointType::Cylindrical)
                {
                    ImGui::Text("%.1f deg", c * 180.0 / kPi);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f rpm", rate * 30.0 / kPi);
                }
                else
                {
                    ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                }
            }
            ImGui::EndTable();
        }
        for (size_t c = 0; c < session.study.machine.chambers.size(); ++c)
        {
            const sim::machine::ChamberState& s = machine.Chamber(c);
            ImGui::Text("Chamber %s: %.2f bar, %.1f cm^3, %.0f K, valves %.2f / %.2f mm, %d combustions%s", session.study.machine.chambers[c].name.c_str(),
                        s.pressure / 1e5, s.volume * 1e6, s.temperature, s.intakeLift * 1e3, s.exhaustLift * 1e3, s.combustions, s.combustion ? "  (burning)" : "");
        }
        for (size_t g = 0; g < session.study.machine.magnetos.size(); ++g)
            ImGui::Text("Magneto %s: magnet %+.0f deg from the core, %d sparks, secondary %.1f kV at the last points opening",
                        session.study.machine.magnetos[g].name.c_str(), machine.Magneto(g).angleDeg, machine.Magneto(g).sparks, machine.Magneto(g).lastSecondaryKv);
    }
    else if (session.scene && session.NeedsTables())
        ImGui::TextDisabled("Computing the magneto field table (MFEM, first run of this geometry only)...");
    else if (session.scene)
        ImGui::TextDisabled("The machine is measured on the drawing once its parts are displayed...");
    ImGui::End();
}

// Time histories of the running machine: any channel against time or against a turning joint's angle
// (e.g. pressure over the crank cycle), and pressure-volume loops of the chambers.
void Plots(Session& session)
{
    ImGui::Begin("Plots");
    const auto lock = session.Lock();
    if (!session.machine || session.channels.empty())
    {
        ImGui::TextDisabled("Run the machine to plot it.");
        ImGui::End();
        return;
    }
    static std::set<std::string> shown;
    static std::string axis = "Time";
    static int cycle = 1;   // 0: 360 deg, 1: 720 deg
    const sim::mb::Model& model = session.study.machine.mechanism;
    if (shown.empty())
        shown.insert(session.channels.front().name);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("X axis", axis.c_str()))
    {
        if (ImGui::Selectable("Time", axis == "Time"))
            axis = "Time";
        for (const sim::mb::Joint& j : model.joints)
            if (j.type == sim::mb::JointType::Revolute || j.type == sim::mb::JointType::Cylindrical)
                if (ImGui::Selectable((j.name + " angle").c_str(), axis == j.name + " angle"))
                    axis = j.name + " angle";
        ImGui::EndCombo();
    }
    if (axis != "Time")
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("cycle", &cycle, "360 deg\0" "720 deg\0");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("channels", std::format("{} shown", shown.size()).c_str()))
    {
        for (const app::Channel& c : session.channels)
        {
            bool on = shown.contains(c.name);
            if (ImGui::Checkbox(std::format("{} [{}]", c.name, c.unit).c_str(), &on))
                on ? (void)shown.insert(c.name) : (void)shown.erase(c.name);
        }
        ImGui::EndCombo();
    }
    const int period = cycle == 0 ? 360 : 720;
    // The X values: time, or the joint's angle wrapped to the cycle, rebuilt from its speed history.
    const size_t n = session.historyTime.size();
    std::vector<double> x(session.historyTime.begin(), session.historyTime.end());
    if (axis != "Time")
    {
        const std::string speed = axis.substr(0, axis.size() - 6) + " speed";
        const auto it = std::find_if(session.channels.begin(), session.channels.end(), [&](const app::Channel& c) { return c.name == speed; });
        const auto joint = std::find_if(model.joints.begin(), model.joints.end(), [&](const sim::mb::Joint& j) { return j.name + " angle" == axis; });
        if (it != session.channels.end() && joint != model.joints.end() && n > 0)
        {
            // Integrate the sampled speed back from the current angle.
            double angle = session.machine->Mechanism().Coordinate(int(joint - model.joints.begin())) * 180.0 / kPi;
            for (size_t i = n; i-- > 0;)
            {
                x[i] = std::fmod(std::fmod(angle, double(period)) + period, double(period));
                if (i > 0)
                    angle -= it->values[i] * 6.0 * (session.historyTime[i] - session.historyTime[i - 1]);   // rpm -> deg/s
            }
        }
    }
    const bool loops = !session.study.machine.chambers.empty();
    static float columns[2] = {0.65f, 0.35f};   // ImPlot writes the user's resizing back
    if (ImPlot::BeginSubplots("##plots", 1, loops ? 2 : 1, ImGui::GetContentRegionAvail(), ImPlotSubplotFlags_NoTitle, nullptr, loops ? columns : nullptr))
    {
        if (ImPlot::BeginPlot("Channels", ImVec2(-1, -1)))
        {
            ImPlot::SetupAxes(axis == "Time" ? "Time [s]" : (axis + " [deg]").c_str(), nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            if (axis != "Time")
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, period, ImPlotCond_Always);
            for (const app::Channel& c : session.channels)
                if (shown.contains(c.name) && c.values.size() == n)
                {
                    std::vector<double> y(c.values.begin(), c.values.end());
                    const std::string label = std::format("{} [{}]", c.name, c.unit);
                    if (axis == "Time")
                        ImPlot::PlotLine(label.c_str(), x.data(), y.data(), int(n), ImPlotSpec(ImPlotProp_LineWeight, 2.0f));
                    else
                        ImPlot::PlotScatter(label.c_str(), x.data(), y.data(), int(n), ImPlotSpec(ImPlotProp_MarkerSize, 1.5f));
                }
            ImPlot::EndPlot();
        }
        if (loops && ImPlot::BeginPlot("p-V", ImVec2(-1, -1)))
        {
            ImPlot::SetupAxes("Volume [cm^3]", "Pressure [bar]");
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            ImPlot::SetupAxesLimits(10, 1000, 0.2, 150, ImPlotCond_Once);
            for (const sim::machine::Chamber& c : session.study.machine.chambers)
            {
                const auto p = std::find_if(session.channels.begin(), session.channels.end(), [&](const app::Channel& ch) { return ch.name == c.name + " pressure"; });
                const auto v = std::find_if(session.channels.begin(), session.channels.end(), [&](const app::Channel& ch) { return ch.name == c.name + " volume"; });
                if (p == session.channels.end() || v == session.channels.end())
                    continue;
                std::vector<double> pv(p->values.begin(), p->values.end()), vv(v->values.begin(), v->values.end());
                ImPlot::PlotLine(c.name.c_str(), vv.data(), pv.data(), int(pv.size()), ImPlotSpec(ImPlotProp_LineColor, ImVec4(1.0f, 0.62f, 0.2f, 1), ImPlotProp_LineWeight, 2.0f));
            }
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
    ImGui::End();
}

// Axis triad in the lower-left corner of the viewport image.
// The control on the part in the view, as in real life: a lever turning on an arc over its range (a joint that turns), or
// a slider along its track (a joint that slides). True while the pointer is on it or dragging it (the view does not pan).
bool AdjusterControl(Session& session, const OrbitCamera& camera, int selected, ImVec2 imageMin, ImVec2 imageMax)
{
    if (!session.scene || selected < 0 || selected >= int(session.scene->Parts().size()))
        return false;
    const app::ScenePart& part = session.scene->Parts()[size_t(selected)];
    const app::MechanismStudy::Adjuster* a = AdjusterOf(session, part.body);
    if (!a)
        return false;
    const sim::mb::Joint& joint = session.study.machine.mechanism.joints[size_t(a->joint)];
    const GfVec3d point(joint.point[0], joint.point[1], joint.point[2]), axis = GfVec3d(joint.axis[0], joint.axis[1], joint.axis[2]).GetNormalized();
    // Model point -> image point.
    const pxr::GfMatrix4d toWorld = session.scene->ModelToWorld(), toCamera = camera.CameraToWorld().GetInverse();
    const double w = imageMax.x - imageMin.x, h = imageMax.y - imageMin.y, t = std::tan(camera.fovDeg * kPi / 360.0);
    auto project = [&](const GfVec3d& model, ImVec2& out) {
        const GfVec3d c = toCamera.Transform(toWorld.Transform(model));
        if (c[2] > -1e-9)
            return false;
        out = ImVec2(float(imageMin.x + (c[0] / (-c[2] * t * w / h) + 1.0) * 0.5 * w), float(imageMin.y + (1.0 - c[1] / (-c[2] * t)) * 0.5 * h));
        return true;
    };
    // Where the part is at rest (model frame): its centre of mass.
    kernel::MassProperties mp;
    std::string error;
    session.document->Mass(part.path, mp, error);
    const kernel::Vec3 o = session.document->BodyOrigin(part.body);
    const GfVec3d rest(o.x + mp.centroid.x, o.y + mp.centroid.y, o.z + mp.centroid.z);
    const double shown = g_adjustDrag.joint == a->joint ? g_adjustDrag.value : a->value;
    // The control's point at a setting: on the arc through the part (rotation), on the track through it (sliding).
    GfVec3d radial = rest - point;
    radial -= axis * pxr::GfDot(radial, axis);
    const double reach = std::max(radial.GetLength(), 1e-4);
    const GfVec3d e1 = radial.GetLength() > 1e-9 ? radial / radial.GetLength() : pxr::GfCross(axis, GfVec3d(1, 0, 0)).GetNormalized();
    auto at = [&](double value) {
        if (!a->rotation)
            return rest + axis * value;
        const pxr::GfRotation turn(axis, value * 180.0 / kPi);
        return point + axis * pxr::GfDot(rest - point, axis) + turn.TransformDir(e1) * reach * 1.15;
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 track = IM_COL32(255, 170, 40, 150), knobColour = IM_COL32(255, 150, 20, 255);
    std::vector<ImVec2> path;
    for (int i = 0; i <= 64; ++i)
        if (ImVec2 p; project(at(a->lower + (a->upper - a->lower) * i / 64.0), p))
            path.push_back(p);
    if (path.size() < 2)
        return false;
    draw->AddPolyline(path.data(), int(path.size()), track, ImDrawFlags_None, 3.0f);
    ImVec2 knob, pivot;
    if (!project(at(shown), knob))
        return false;
    if (a->rotation && project(point + axis * pxr::GfDot(rest - point, axis), pivot))
    {
        draw->AddLine(pivot, knob, knobColour, 3.0f);   // the lever, from the pivot
        draw->AddCircleFilled(pivot, 4.0f, knobColour);
    }
    draw->AddCircleFilled(knob, 9.0f, knobColour);
    draw->AddCircle(knob, 9.0f, IM_COL32(40, 20, 0, 255), 0, 2.0f);
    const std::string label = a->rotation ? std::format("{} {:+.2f} deg", a->name, shown * 180.0 / kPi) : std::format("{} {:+.3f} mm", a->name, shown * 1e3);
    draw->AddText(ImVec2(knob.x + 12, knob.y - 8), IM_COL32(255, 200, 120, 255), label.c_str());

    // Dragging: the setting whose control point is nearest the pointer.
    const ImGuiIO& io = ImGui::GetIO();
    const bool overKnob = std::hypot(io.MousePos.x - knob.x, io.MousePos.y - knob.y) < 14.0;
    static bool dragging = false;
    if (overKnob && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered())
        dragging = true;
    if (dragging)
    {
        double best = shown, nearest = 1e300;
        for (int i = 0; i <= 400; ++i)
        {
            const double v = a->lower + (a->upper - a->lower) * i / 400.0;
            if (ImVec2 p; project(at(v), p))
                if (const double d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y); d < nearest)
                    nearest = d, best = v;
        }
        const bool release = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        AdjustTo(session, *a, best, release);
        if (release)
            dragging = false;
        return true;
    }
    if (overKnob)
        ImGui::SetTooltip("Drag to set %s (range %.4g .. %.4g %s)", a->name.c_str(), a->rotation ? a->lower * 180.0 / kPi : a->lower * 1e3,
                          a->rotation ? a->upper * 180.0 / kPi : a->upper * 1e3, a->rotation ? "deg" : "mm");
    return overKnob;
}

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
    ImGui::DockBuilderDockWindow("Study", bottom);
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

// The engine drawing used by the verification modes (--snapshot, --ui-test): Dist/models next to the
// executable, else the repository's models folder. The interactive app opens nothing by itself.
std::string EngineModelPath()
{
    return Resource("models/Engine_SingleCylinder.usda", AIAD_MODELS_DIR "/Engine_SingleCylinder.usda");
}

// Opens a document and prepares its machine: the study and the magneto field tables (computed here, or
// read from the cache next to the executable).
bool LoadDocument(Session& session, const std::string& path, const std::string& cacheFolder, std::string& error)
{
    session.document = doc::Document::Open(path, error);
    if (!session.document)
        return false;
    if (!session.Build())
    {
        error = session.studyError;
        return false;
    }
    session.SetTables(Session::ComputeTables(session.MagnetoGeometry(), cacheFolder), session.MagnetoGeometry());
    session.Restart();
    return true;
}

// Verification of any document (no window): its machine runs `seconds` with everything it is made of and
// the measured result is printed (exit 0 = it ran).
int SimulateDocument(const char* documentPath, double seconds, const std::string& cacheFolder)
{
    Session session;
    std::string error;
    if (!LoadDocument(session, documentPath, cacheFolder, error))
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    for (const std::string& line : session.study.measured)
        std::printf("measured: %s\n", line.c_str());
    std::string text;
    const auto t0 = std::chrono::steady_clock::now();
    const Json result = app::RunMachine(session.study, session.magnetoTables, seconds, text);
    std::printf("%s\n(%.1f s of computing)\n", text.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return result.value("runs_on_its_own", true) ? 0 : 1;
}

// Verification of any document (no window): a mechanism runs 0.5 s, then the posed drawing is rendered to
// a PNG.
int SnapshotDocument(const char* pngPath, const char* documentPath, const std::string& environment, const std::string& cacheFolder)
{
    Session session;
    std::string error;
    if (!LoadDocument(session, documentPath, cacheFolder, error) && !session.document)
    {
        std::printf("cannot open %s: %s\n", documentPath, error.c_str());
        return 1;
    }
    if (session.machine)
    {
        session.running = true;
        session.timeScale = 1.0;
        while (session.Time() < 0.5)
            session.Advance(0.05, 0.0);
        std::printf("machine: %zu bodies, %zu joints, %d degree(s) of freedom, simulated %.3f s\n", session.study.machine.mechanism.bodies.size(),
                    session.study.machine.mechanism.joints.size(), session.study.mobility, session.Time());
    }
    else
        std::printf("%s\nthe drawing at rest\n", session.studyError.c_str());
    app::DocumentScene scene(*session.document, environment);
    scene.Update(1e9);
    scene.SetMotion(session.Motion());
    render::HydraViewport viewport;
    viewport.SetStage(session.document->Stage());
    viewport.Resize(1200, 675);
    OrbitCamera camera;
    camera.yaw = -60.0;
    camera.pitch = 25.0;
    camera.Fit(scene.WorldBounds());
    viewport.SetCamera(camera.CameraToWorld(), camera.fovDeg, 0.001, 50.0);
    int frames = 0;
    for (; frames < 256 && !(frames > 8 && viewport.IsConverged()); ++frames)
        viewport.Render();
    const render::ViewportImage& image = viewport.ReadImage();
    const bool written = stbi_write_png(pngPath, image.width, image.height, 4, image.rgba.data(), image.width * 4) != 0;
    std::printf("rendered %d frames, %s %s\n", frames, written ? "wrote" : "FAILED to write", pngPath);
    return written ? 0 : 1;
}

// Verification mode (no window): the engine drawing runs 2 s with everything it is made of, then its view
// is animated and rendered.
int Snapshot(const char* pngPath, const std::string& cacheFolder, const std::string& environment)
{
    Session session;
    std::string error;
    if (!LoadDocument(session, EngineModelPath(), cacheFolder, error))
    {
        std::printf("cannot load the engine drawing: %s\n", error.c_str());
        return 1;
    }
    std::printf("read from the drawing (%zu parts):\n", session.document->Parts().size());
    for (const std::string& line : session.study.measured)
        std::printf("  %s\n", line.c_str());
    session.running = true;
    session.timeScale = 1.0;
    while (session.Time() < 2.0)
        session.Advance(0.05, 0.0);
    const sim::mb::Multibody& mb = session.machine->Mechanism();
    for (size_t j = 0; j < session.study.machine.mechanism.joints.size(); ++j)
        if (session.study.machine.mechanism.joints[j].type == sim::mb::JointType::Revolute)
            std::printf("at t = %.2f s: '%s' %.0f rpm\n", session.Time(), session.study.machine.mechanism.joints[j].name.c_str(), mb.Rate(int(j)) * 30.0 / kPi);
    for (size_t c = 0; c < session.study.machine.chambers.size(); ++c)
        std::printf("chamber '%s': %d combustions, peak %.1f bar\n", session.study.machine.chambers[c].name.c_str(), session.machine->Chamber(c).combustions,
                    session.machine->Chamber(c).peakPressure / 1e5);

    app::DocumentScene scene(*session.document, environment);
    scene.Update(1e9);
    std::printf("view of the drawing: %zu parts tessellated in %.2f s\n", scene.Parts().size(), scene.TessellationSeconds());
    scene.SetMotion(session.Motion());
    render::HydraViewport viewport;
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
    // A part of the first moving body (hidden, shown, highlighted while it moves).
    int movingPart = 0;
    for (size_t i = 0; i < scene.Parts().size(); ++i)
        if (!session.document->BodyFixed(scene.Parts()[i].body))
        {
            movingPart = int(i);
            break;
        }
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
    const std::string magnetoCache = ExeDir().string();   // magneto field tables (cached per drawn geometry)
    const std::string environment = (ExeDir() / "studio_env.hdr").string();
    app::WriteStudioEnvironment(environment);
    if (argc == 3 && std::string(argv[1]) == "--snapshot")
        return Snapshot(argv[2], magnetoCache, environment);
    if (argc == 4 && std::string(argv[1]) == "--snapshot")
        return SnapshotDocument(argv[2], argv[3], environment, magnetoCache);
    if (argc == 4 && std::string(argv[1]) == "--simulate")
        return SimulateDocument(argv[2], std::atof(argv[3]), magnetoCache);
    if (argc == 3 && std::string(argv[1]) == "--check")
    {
        // The simulator's critique of a drawing, without a window (exit 0 = it can be simulated).
        std::string error;
        const std::unique_ptr<doc::Document> document = doc::Document::Open(argv[2], error);
        if (!document)
        {
            std::printf("cannot open %s: %s\n", argv[2], error.c_str());
            return 2;
        }
        app::MechanismStudy study;
        if (!app::DeriveMechanismStudy(*document, study))
        {
            std::printf("The document has no joints: it is a drawing, not a mechanism yet.\n");
            return 1;
        }
        for (const std::string& line : study.measured)
            std::printf("measured: %s\n", line.c_str());
        std::printf("%s\n", study.findings.empty() ? "No problem found: the drawing is the machine the simulator runs." : study.Report().c_str());
        return study.Runnable() ? 0 : 1;
    }

    // aiad.exe [--run] [document.usda]: opens the document given, if any (--run starts its engine);
    // --ui-test <png>: verification with a real window.
    const bool uiTest = argc == 3 && std::string(argv[1]) == "--ui-test";
    bool autoRun = false;
    std::string startDocument;
    for (int i = 1; i < argc && !uiTest; ++i)
    {
        if (std::string(argv[i]) == "--run")
            autoRun = true;
        else
            startDocument = argv[i];
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // file dialogs

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"TootegaAIAD", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Tootega AIAD", WS_OVERLAPPEDWINDOW, 60, 40,
                              int(1700 * scale), int(1000 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_d3d.Create(hwnd))
    {
        std::fprintf(stderr, "DirectX 12 device creation failed\n");
        return 1;
    }
    // The window where the user left it (still on a monitor), maximized the first time.
    AppConfig config(uiTest ? std::filesystem::path() : ExeDir() / ".aiad" / "aiadconfig.json");
    RECT savedRect{};
    bool savedMaximized = true;
    if (config.Window(savedRect, savedMaximized) && MonitorFromRect(&savedRect, MONITOR_DEFAULTTONULL))
    {
        WINDOWPLACEMENT placement{sizeof(placement)};
        placement.showCmd = savedMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        placement.rcNormalPosition = savedRect;
        SetWindowPlacement(hwnd, &placement);
    }
    else
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    auto rememberWindow = [&] {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(hwnd, &placement))
            config.SetWindow(placement.rcNormalPosition, placement.showCmd == SW_SHOWMAXIMIZED);
    };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Panels dragged out of the main window become windows of their own (another monitor: an instrument bench); where
    // they are and their size are saved with the layout.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoTaskBarIcon = true;   // they belong to the main window (one icon in the task bar)
    // Panels as the user left them (aiadconfig.json; the layout file of earlier versions is taken over once).
    // The UI test always starts from the default layout.
    io.IniFilename = nullptr;
    std::string layout = config.Layout();
    if (layout.empty() && !uiTest)
        if (const std::filesystem::path old = ExeDir() / "aiad_layout_v3.ini"; std::filesystem::exists(old))
            layout = ReadTextFile(old);
    if (!layout.empty())
        ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
    const bool firstRun = uiTest || layout.empty();
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
        // The instruments' seven-segment digits (DSEG7, embedded).
        int bytes = 0;
        if (const void* data = EmbeddedFont(L"DSEG7", bytes))
        {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            fonts.seven = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), bytes, fonts.size * 2.0f, &config);
        }
    }

    // Command Bus + engine commands; loopback bridge for the agent's MCP server.
    MainThreadQueue mainQueue;
    cmd::CommandBus bus;
    g_bus = &bus;
    app::SimulationJobs simulationJobs;
    app::AppContext appContext;
    bridge::BridgeServer bridgeServer;
    ChatTextures chatTextures;

    // The open document (none at start: File > New / Open, or a path on the command line). Its machine is
    // derived from it; the magneto field tables (from the drawn magneto geometry) are computed in the
    // background (cached; about 70 s the first time).
    Session session;
    session.threaded = true;   // the machine runs on its own thread; the UI only reads it
    bool documentChanged = false;
    uint64_t changeCount = 0, savedChangeCount = 0;  // unsaved changes = the counts differ
    std::atomic<bool> magnetoReady = true, magnetoCancel = false;
    std::atomic<double> worstFrame = 0.0;   // longest time between two frames [s] (--ui-test: the UI never waits)
    std::thread magnetoThread;
    std::vector<std::vector<sim::magneto::MagnetoSample>> computedTables;
    std::vector<sim::magneto::MagnetoGeometry> computedGeometry;
    // Field tables for the drawn magnetos, off the main thread; the frame loop hands them to the session.
    auto stopTables = [&] {   // closing the document or the app: never wait for MFEM
        magnetoCancel = true;
        if (magnetoThread.joinable())
            magnetoThread.join();
        magnetoCancel = false;
        magnetoReady = true;
    };
    auto startTables = [&] {
        if (magnetoThread.joinable())
            magnetoThread.join();
        if (!session.NeedsTables())
            return;
        magnetoReady = false;
        magnetoThread = std::thread([&, geometry = session.MagnetoGeometry()] {
            computedTables = Session::ComputeTables(geometry, magnetoCache, &magnetoCancel);
            computedGeometry = geometry;
            magnetoReady = true;
        });
    };

    std::unique_ptr<render::HydraViewport> viewport;
    app::UploadTexture texture;
    OrbitCamera camera;
    int selected = -1;
    bool sceneReady = false, fitPending = false, layoutBuilt = !firstRun;
    // Loading a document: the view of its parts (slices), then the machine study; shown with a progress bar.
    struct Loading
    {
        bool active = false, measuring = false;
        double viewStarted = -1.0;   // when the parts' display started [s since start]
        size_t parts = 0;            // parts to display then
    } loading;
    RecentDocuments recent(uiTest ? std::filesystem::path() : AppDataDir() / "recent_documents.json");
    const auto start = std::chrono::steady_clock::now();
    double previousTime = 0.0;

    appContext.session = &session;
    appContext.camera = &camera;
    appContext.selected = &selected;

    appContext.snapshot = [&] { return viewport ? EncodePng(viewport->ReadImage()) : std::vector<uint8_t>(); };
    doc::RegisterDocumentCommands(bus, [&] { return session.document.get(); });
    app::RegisterAppCommands(bus, appContext, simulationJobs);

    std::string bridgeError;
    auto reply = [](const cmd::CommandOutput& out) {
        Json images = Json::array();
        for (const cmd::Image& image : out.images)
            images.push_back({{"mimeType", image.mimeType}, {"data", Base64Encode(image.bytes.data(), image.bytes.size())}});
        return Json{{"ok", out.ok}, {"text", out.text}, {"result", out.result}, {"error", out.error}, {"images", images}};
    };
    // Runs on the main thread; a command's background part (pure computation, no document) runs after it
    // on the bridge's connection thread, so the UI does not wait for it.
    auto bridgeHandler = [&](const Json& request) -> bridge::BridgeServer::Reply {
        const std::string method = request.value("method", "");
        if (method == "catalog")
            return Json{{"ok", true}, {"result", bus.CatalogJson()}};
        if (method != "execute")
            return Json{{"ok", false}, {"error", "unknown bridge method '" + method + "' (catalog | execute)"}};
        cmd::CommandOutput out = bus.ExecuteDeferred(request.value("name", ""), request.value("args", Json::object()), "ai");
        if (!out.background)
            return reply(out);
        bridge::BridgeServer::Reply r(Json::object());
        r.later = [out, reply]() mutable {
            out.background(out);
            out.background = nullptr;
            return reply(out);
        };
        return r;
    };
    // The agent works where the document is: its folder is the workspace (also for aiad-mcp discovery);
    // without a saved document, the default folder (Documents\Tootega AIAD).
    app::AiHost aiHost;
    aiHost.bus = &bus;
    if (!bridgeServer.Start(mainQueue, bridgeHandler, std::filesystem::path(), bridgeError))
        std::fprintf(stderr, "agent bridge: %s\n", bridgeError.c_str());
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
    ai.SetChatPlacement(config.ChatWindow());
    bridgeServer.SetWorkspace(Widen(ai.Workspace()));

    // The drawing changed (user, AI or undo): right after the command (and once per frame, for undo/redo)
    // the view takes the new part list; the changed parts' display, the machine study and the field
    // tables follow in the frame loop, a slice per frame, so that the UI never waits for them.
    auto syncDocument = [&] {
        if (!documentChanged || !sceneReady)
            return;
        documentChanged = false;
        session.scene->Refresh();
        session.studyStale = true;
        if (selected >= int(session.scene->Parts().size()))
            selected = -1;
    };

    // ---- Document lifecycle (File menu) ----
    // The agent and aiad-mcp discovery follow the document's folder (the default one when unsaved).
    auto followDocumentFolder = [&] {
        const std::string path = session.document ? session.document->Path() : std::string();
        ai.SetWorkspace(path.empty() ? std::string() : Narrow(std::filesystem::absolute(Widen(path)).parent_path().wstring()));
        bridgeServer.SetWorkspace(Widen(ai.Workspace()));
    };
    auto closeDocument = [&] {
        stopTables();
        g_d3d.WaitIdle();
        session.scene.reset();  // the scene refers to the document: before it
        viewport.reset();
        texture.Destroy(g_d3d);
        session.Clear();
        selected = -1;
        sceneReady = false;
        loading = Loading{};
        documentChanged = false;
        changeCount = savedChangeCount = 0;
        bus.ClearHistory();  // undo steps of the closed document must not run against the next one
        followDocumentFolder();
    };
    // Takes an opened or new document: derives its study and starts the magneto table in the background;
    // the frame loop builds its view when that is done.
    auto adoptDocument = [&](std::unique_ptr<doc::Document> document) {
        session.document = std::move(document);
        session.document->OnChanged = [&] {
            documentChanged = true;
            ++changeCount;
        };
        // Instruments observe the machine: changing one rebuilds them, not the machine.
        session.document->OnInstrumentsChanged = [&] {
            ++changeCount;
            session.RebuildInstruments();
        };
        // The view is saved with the document (by the user or the agent) and found again when it is opened.
        session.document->OnSaving = [&] {
            session.document->SetView(Json{{"yaw", camera.yaw},
                                           {"pitch", camera.pitch},
                                           {"distance", camera.distance},
                                           {"fov", camera.fovDeg},
                                           {"target", {camera.target[0], camera.target[1], camera.target[2]}}}
                                          .dump());
        };
        session.studyStale = true;   // derived once its view is written (frame loop)
        loading = Loading{};
        loading.active = true;
        followDocumentFolder();
    };
    auto openDocument = [&](const std::string& path) {
        std::string error;
        std::unique_ptr<doc::Document> document = doc::Document::Open(path, error);
        if (!document)
        {
            g_lastError = "Cannot open " + path + ": " + error;
            return false;
        }
        closeDocument();
        adoptDocument(std::move(document));
        recent.Add(path);
        return true;
    };
    auto newDocument = [&] {
        closeDocument();
        adoptDocument(doc::Document::Create("Untitled"));
    };
    // Save through the Command Bus (the same command the agent uses); without a file yet, Save As.
    auto saveDocumentAs = [&](std::string path) {
        if (path.empty())
            path = DocumentDialog(hwnd, true, session.document->Path().empty() ? Narrow((std::filesystem::path(Widen(ai.Workspace())) / "Untitled.usda").wstring())
                                                                                 : session.document->Path());
        return !path.empty() && UserCommand("aiad_doc_save", {{"path", path}});
    };
    auto saveDocument = [&] { return session.document->Path().empty() ? saveDocumentAs("") : UserCommand("aiad_doc_save", Json::object()); };
    auto documentTitle = [&] {
        if (!session.document)
            return std::string();
        const std::string path = session.document->Path();
        return path.empty() ? std::string("Untitled") : Narrow(std::filesystem::path(Widen(path)).filename().wstring());
    };
    auto unsavedChanges = [&] { return session.document && changeCount != savedChangeCount; };

    bus.OnExecuted = [&](const cmd::AuditEntry& entry) {
        syncDocument();
        // Saved (by the user or the agent): no unsaved changes; the file joins the recent list and
        // the agent follows it if it went to another folder.
        if (entry.ok && entry.command == "aiad_doc_save" && session.document)
        {
            savedChangeCount = changeCount;
            recent.Add(session.document->Path());
            followDocumentFolder();
        }
    };

    // Actions that would discard unsaved changes ask first: Save / Don't save / Cancel.
    std::function<void()> pendingAction;
    bool askToSave = false;
    auto guarded = [&](std::function<void()> action) {
        if (unsavedChanges())
        {
            pendingAction = std::move(action);
            askToSave = true;
        }
        else
            action();
    };
    std::vector<std::function<void()>> fileActions;  // run between frames (see the frame loop)
    auto fileNew = [&] { guarded(newDocument); };
    auto fileOpen = [&] {
        guarded([&] {
            if (const std::string path = DocumentDialog(hwnd, false, session.document ? session.document->Path() : Narrow((std::filesystem::path(Widen(ai.Workspace())) / "x").wstring()));
                !path.empty())
                openDocument(path);
        });
    };
    auto fileSave = [&] { saveDocument(); };
    auto fileSaveAs = [&] { saveDocumentAs(""); };
    std::string windowTitle;
    bool quit = false, showAbout = false;
    if (!startDocument.empty() && !openDocument(startDocument))
        std::fprintf(stderr, "%s\n", g_lastError.c_str());

    // --ui-test: drive the real app (bridge + MCP + commands + chat fixture), capture the window.
    int uiFrame = 0, uiFailures = 0, uiCaptureFrames = -1;  // -1 = agent phase, then frames since the chat fixture
    bool uiFilesChecked = false;
    Json uiChatBefore;
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
        if (g_closeRequested)
        {
            g_closeRequested = false;
            guarded([&] { quit = true; });
        }
        if (done || quit)
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
        worstFrame = std::max(worstFrame.load(), now - previousTime);
        const double wallDt = std::min(now - previousTime, 0.1);
        previousTime = now;

        // --ui-test, file part: nothing is open at start; New, Save As and Open through the same
        // functions as the File menu (then the agent phase works on the opened engine).
        if (uiTest && uiFrame == 0 && !session.document && !uiFilesChecked)
        {
            uiFilesChecked = true;
            auto check = [&](bool ok, const char* what) {
                std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
                uiFailures += ok ? 0 : 1;
            };
            check(!session.document, "no document at start (nothing opened automatically)");
            newDocument();
            check(session.document && session.document->Parts().empty() && session.document->Path().empty() && documentTitle() == "Untitled",
                  "File > New: an empty untitled document");
            // Any machine, made only of commands (no code knows it): a fixed base and an arm turned by a motor.
            {
                auto run = [&](const char* name, const Json& args) { return bus.Execute(name, args, "user"); };
                auto says = [](const cmd::CommandOutput& o, const char* text) { return o.text.find(text) != std::string::npos; };
                auto box = [](double x, double y, double z, double sx, double sy, double sz) {
                    return Json::array({{{"op", "box"}, {"corner", Json::array({x, y, z})}, {"size", Json::array({sx, sy, sz})}}});
                };
                check(!run("aiad_mechanism_check", Json::object()).result.value("mechanism", true), "mechanism: a drawing without joints is not one yet");
                const bool drawn = run("aiad_material_define", {{"name", "Steel"}, {"density", 7850}}).ok &&
                                   run("aiad_body_define", {{"body", "Base"}, {"origin", Json::array({0, 0, 0})}, {"fixed", true}}).ok &&
                                   run("aiad_part_create", {{"name", "Base block"}, {"body", "Base"}, {"material", "Steel"}, {"features", box(-0.05, -0.05, -0.05, 0.1, 0.1, 0.1)}}).ok &&
                                   run("aiad_body_define", {{"body", "Arm"}, {"origin", Json::array({0, 0, 0})}}).ok &&
                                   run("aiad_part_create", {{"name", "Arm bar"}, {"body", "Arm"}, {"material", "Steel"}, {"features", box(0, -0.01, 0.05, 0.2, 0.02, 0.02)}}).ok;
                const Json motor = {{"mode", "speed"}, {"value", 2.0}};
                const bool misplaced = run("aiad_joint_define", {{"name", "Pivot"}, {"type", "revolute"}, {"body0", "Arm"}, {"body1", "Base"},
                                                                 {"point", Json::array({0.5, 0, 0.05})}, {"axis", Json::array({0, 0, 1})}, {"motor", motor}}).ok;
                const cmd::CommandOutput bad = run("aiad_mechanism_check", Json::object());
                check(drawn && misplaced && !bad.result.value("runnable", true) && says(bad, "outside the parts of 'Arm'") && says(bad, "Fix: ") &&
                          !run("aiad_mechanism_run", {{"seconds", 0.1}}).ok,
                      "mechanism: a joint away from the parts it joins is criticised (with the fix) and does not run");
                run("aiad_joint_define", {{"name", "Pivot"}, {"type", "revolute"}, {"body0", "Arm"}, {"body1", "Base"},
                                          {"point", Json::array({0, 0, 0.05})}, {"axis", Json::array({0, 0, 1})}, {"motor", motor}});
                const cmd::CommandOutput good = run("aiad_mechanism_check", Json::object());
                const cmd::CommandOutput ran = run("aiad_mechanism_run", {{"seconds", 0.5}});
                const double angle = ran.ok ? ran.result["joints"][0].value("coordinate", 0.0) : 0.0;
                const double turned = ran.ok ? ran.result["bodies"][0].value("rotation_rad", 0.0) : 0.0;
                check(good.result.value("runnable", false) && good.result.value("degrees_of_freedom", -1) == 0 && std::abs(angle - 1.0) < 0.01 &&
                          std::abs(turned - 1.0) < 0.01,
                      "mechanism: the arm, driven at 2 rad/s for 0.5 s, turned 1 rad (generic solver, no machine code)");
            }
            const std::string saved = (std::filesystem::temp_directory_path() / "aiad_ui_test_new.usda").string();
            std::error_code ec;
            std::filesystem::remove(saved, ec);
            check(saveDocumentAs(saved) && std::filesystem::exists(saved) && session.document->Path() == saved && !unsavedChanges(),
                  "File > Save As writes it; no unsaved changes");
            check(openDocument(EngineModelPath()) && session.document->Parts().size() > 50 && documentTitle() == "Engine_SingleCylinder.usda" &&
                      !bus.CanUndo() && recent.Items().size() == 2,
                  "File > Open: the engine drawing (new undo history, listed in Open Recent)");
            std::fflush(stdout);
        }
        if (magnetoReady && magnetoThread.joinable())
        {
            magnetoThread.join();
            if (computedTables.size() == computedGeometry.size())
                session.SetTables(std::move(computedTables), std::move(computedGeometry));
            const bool wasRunning = session.running;
            session.Restart();
            session.running = (wasRunning || autoRun) && session.machine != nullptr;
            autoRun = autoRun && !session.machine;   // --run: start the machine as soon as it is built
            startTables();   // the drawing changed while they were computed
        }
        if (!sceneReady && magnetoReady && session.document)
        {
            session.scene = std::make_unique<app::DocumentScene>(*session.document, environment);
            viewport = std::make_unique<render::HydraViewport>();
            viewport->SetStage(session.document->Stage());
            sceneReady = true;
            fitPending = true;
            loading.viewStarted = now;
            loading.parts = session.scene->PendingCount();
        }
        syncDocument();
        // A slice of the view (changed parts), then the machine study and its field tables.
        if (sceneReady && session.scene->Pending())
            session.scene->Update(10.0);
        else if (sceneReady)
        {
            if (fitPending)
            {
                camera = OrbitCamera{};
                // The view it was saved with; else frame what is drawn, once it is.
                const Json view = ParseJson(session.document->View());
                if (view.is_object() && view.contains("target") && view["target"].is_array() && view["target"].size() == 3)
                {
                    camera.yaw = view.value("yaw", camera.yaw);
                    camera.pitch = view.value("pitch", camera.pitch);
                    camera.distance = view.value("distance", camera.distance);
                    camera.fovDeg = view.value("fov", camera.fovDeg);
                    camera.target = GfVec3d(view["target"][0].get<double>(), view["target"][1].get<double>(), view["target"][2].get<double>());
                }
                else
                    camera.Fit(session.scene->WorldBounds());
                fitPending = false;
            }
            if (session.studyStale && loading.active && !loading.measuring)
                loading.measuring = true;   // one frame to show it: the study blocks this thread while it measures
            else if (session.studyStale)
            {
                loading.active = false;
                const bool wasRunning = session.running;
                session.Build();
                session.running = (wasRunning || autoRun) && session.machine != nullptr;
                autoRun = autoRun && !session.machine;
                if (magnetoReady)
                    startTables();
                if (selected >= int(session.scene->Parts().size()))
                    selected = -1;
            }
        }

        // Bridge requests from the agent run here, on the main thread, once the view has caught up with the
        // last change (its slices warm the caches the next command reads; the bridge waits on its own thread).
        if (!sceneReady || !session.scene->Pending())
            mainQueue.Drain();
        ai.Pump();
        session.Advance(wallDt, now);

        if (uiTest && sceneReady && !session.scene->Pending() && !session.studyStale && magnetoReady && !magnetoThread.joinable() && ++uiFrame == 8)
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
                // The machine is read from the drawing: the chamber's bore measured on the regenerated liner.
                const Json derived = call("aiad_mechanism_check", Json::object());
                log(text(derived).find("bore 84.00 mm") != std::string::npos && derived.contains("structuredContent") &&
                        derived["structuredContent"].value("runnable", false),
                    "the machine is measured on the drawing: chamber bore 84.00 mm, it can run");
                // The simulator criticises a drawing that would not work: with 12 mm of valve lash the 8 mm lobes
                // never reach the tappets; refused, with the fix.
                const Json lash = call("aiad_parameters_set", {{"values", {{"valve_lash", 0.012}}}});
                const Json check = call("aiad_mechanism_check", Json::object());
                const std::string critique = text(check);
                log(!lash.value("isError", true) && check.contains("structuredContent") && !check["structuredContent"].value("runnable", true) &&
                        critique.find("never reaches the face") != std::string::npos && critique.find("Fix: ") != std::string::npos,
                    "valve lash larger than the lobe lift: the simulator says why and how to fix it");
                const Json refused = call("aiad_sim_run", {{"seconds", 1.0}});
                log(refused.value("isError", false) && text(refused).find("never reaches") != std::string::npos, "and refuses to simulate it");
                const Json undoLash = call("aiad_undo", Json::object());
                const Json fixedCheck = call("aiad_mechanism_check", Json::object());
                log(fixedCheck.contains("structuredContent") && fixedCheck["structuredContent"].value("runnable", false),
                    "undo (" + text(undoLash) + "): the engine drawing runs again");
                // A run the agent asks for computes off the main thread: the UI keeps drawing meanwhile.
                worstFrame = 0.0;
                const auto runStart = std::chrono::steady_clock::now();
                const Json quick = call("aiad_mechanism_run", {{"seconds", 4.0}});
                const double runSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
                log(!quick.value("isError", true) && runSeconds > 1.0 && worstFrame < 0.25,
                    std::format("aiad_mechanism_run took {:.1f} s and the UI kept drawing (longest frame {:.0f} ms)", runSeconds, worstFrame * 1e3));
                // The live simulation runs on its own thread: at real time the UI keeps drawing.
                session.timeScale = 1.0;
                worstFrame = 0.0;
                const Json live = call("aiad_sim_start", Json::object());
                Sleep(2500);
                const Json liveStatus = call("aiad_sim_status", Json::object());
                const double liveFrame = worstFrame;
                call("aiad_sim_stop", Json::object());
                session.timeScale = 0.02;
                const double liveTime = liveStatus.contains("structuredContent") ? liveStatus["structuredContent"].value("time_s", 0.0) : 0.0;
                log(!live.value("isError", true) && liveTime > 0.3 && liveFrame < 0.12,
                    std::format("live simulation at 1.0x on its own thread: {:.2f} s simulated in 2.5 s, longest frame {:.0f} ms", liveTime, liveFrame * 1e3));
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
                const double displacement = result.contains("chambers") && !result["chambers"].empty() ? result["chambers"][0].value("displacement_cm3", 0.0) : 0.0;
                log(result.value("runs_on_its_own", false), "simulation job: " + (job > 0 ? text(status) : text(run)));
                log(std::abs(displacement - 332.5) < 1.0, std::format("measured displacement {:.1f} cm^3 (pi/4 x 84^2 x 60 = 332.5)", displacement));
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
            const std::filesystem::path modelFolder = std::filesystem::absolute(Widen(EngineModelPath())).parent_path();
            const bool inModelFolder = ai.Workspace() == Narrow(modelFolder.wstring()) && ai.Session() && ai.Session()->Options().cwd == modelFolder;
            std::printf("%s the agent works in the model's folder (%s)\n", inModelFolder ? "ok   " : "FAIL ", ai.Workspace().c_str());
            uiFailures += inModelFolder ? 0 : 1;
            ai.LoadFixture(fixture, "Increase the bore to 84 mm and show me the result.");
            std::printf("stage: chat fixture loaded\n"), std::fflush(stdout);
            uiCaptureFrames = 0;  // capture phase
        }

        // Shortcuts (not while typing): Ctrl+N/O/S, Ctrl+Shift+S, Ctrl+Z/Y.
        if (!io.WantTextInput && io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_N, false))
                fileActions.push_back(fileNew);
            if (ImGui::IsKeyPressed(ImGuiKey_O, false))
                fileActions.push_back(fileOpen);
            if (ImGui::IsKeyPressed(ImGuiKey_S, false) && session.document)
                fileActions.push_back(io.KeyShift ? std::function<void()>(fileSaveAs) : std::function<void()>(fileSave));
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
                bus.Undo();
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                bus.Redo();
        }
        // File actions chosen in the previous frame run here, between frames: closing a document
        // releases the viewport texture, which a frame being drawn may still reference.
        for (std::vector<std::function<void()>> actions = std::exchange(fileActions, {}); const auto& action : actions)
            action();
        // Window title: the document and its unsaved-changes mark.
        if (const std::string title = session.document ? std::format("Tootega AIAD - {}{}", documentTitle(), unsavedChanges() ? " *" : "") : "Tootega AIAD";
            title != windowTitle)
        {
            windowTitle = title;
            SetWindowTextW(hwnd, Widen(title).c_str());
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // ---- Main menu (above the dockspace) ----
        if (ImGui::BeginMainMenuBar())
        {
            const bool open = session.document != nullptr;
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N"))
                    fileActions.push_back(fileNew);
                if (ImGui::MenuItem("Open...", "Ctrl+O"))
                    fileActions.push_back(fileOpen);
                if (ImGui::BeginMenu("Open Recent", !recent.Items().empty()))
                {
                    for (const std::string& path : recent.Items())
                        if (ImGui::MenuItem(path.c_str()))
                            fileActions.push_back([&, path] {
                                guarded([&, path] {
                                    if (!openDocument(path) && !std::filesystem::exists(Widen(path)))
                                        recent.Remove(path);  // gone: drop it from the list
                                });
                            });
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Recent List"))
                        recent.Clear();
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S", false, open))
                    fileActions.push_back(fileSave);
                if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, open))
                    fileActions.push_back(fileSaveAs);
                ImGui::Separator();
                if (ImGui::MenuItem("Close", nullptr, false, open))
                    fileActions.push_back([&] { guarded(closeDocument); });
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4"))
                    g_closeRequested = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                const auto history = bus.History();
                const std::string undo = bus.CanUndo() ? "Undo " + history[bus.Depth() - 1].label : std::string("Undo");
                const std::string redo = bus.CanRedo() ? "Redo " + history[bus.Depth()].label : std::string("Redo");
                if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, bus.CanUndo()))
                    bus.Undo();
                if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, bus.CanRedo()))
                    bus.Redo();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Fit All", nullptr, false, sceneReady))
                    camera.Fit(session.scene->WorldBounds());
                bool section = sceneReady && session.scene->SectionView();
                if (ImGui::MenuItem("Section View", nullptr, &section, sceneReady))
                    session.scene->SetSectionView(section);
                if (ImGui::BeginMenu("Instruments", session.document != nullptr))
                {
                    app::InstrumentsMenu(session, RunUserCommand);
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout"))
                    layoutBuilt = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help"))
            {
                if (ImGui::MenuItem("About Tootega AIAD"))
                    showAbout = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
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
        if (!session.document)
        {
            // Start page: nothing is open until the user asks for it.
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 24, ImGui::GetCursorPosY() + 20));
            ImGui::BeginGroup();
            ImGui::PushFont(fonts.bold, fonts.size * 1.3f);
            ImGui::TextUnformatted("No document open");
            ImGui::PopFont();
            ImGui::Spacing();
            if (ImGui::Button("New document"))
                fileActions.push_back(fileNew);
            ImGui::SameLine();
            if (ImGui::Button("Open..."))
                fileActions.push_back(fileOpen);
            if (!recent.Items().empty())
            {
                ImGui::SeparatorText("Recent");
                for (const std::string& path : recent.Items())
                    if (ImGui::Selectable(path.c_str()))
                        fileActions.push_back([&, path] {
                            if (!openDocument(path) && !std::filesystem::exists(Widen(path)))
                                recent.Remove(path);
                        });
            }
            ImGui::EndGroup();
        }
        else if (!sceneReady)
        {
            ImGui::Text("  Opening %s%s...", documentTitle().c_str(),
                        session.NeedsTables() ? " and computing the magneto field table (MFEM, first run of this geometry only, about 70 s)" : "");
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
            ImGui::TextDisabled("  LMB click select | LMB drag or MMB pan | Alt+LMB, Space+LMB or MMB+RMB rotate | wheel zoom");

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
                session.scene->SetSparkVisible(session.SparkVisible(now));
                session.scene->SetHighlight(selected);
                // Clipping planes that follow the camera: a watch seen from a few millimetres, an engine from a metre.
                viewport->SetCamera(camera.CameraToWorld(), camera.fovDeg, std::max(1e-6, camera.distance * 0.01), std::max(20.0, camera.distance * 100.0));
                viewport->Render();
                const ImVec2 imageMin = ImGui::GetCursorScreenPos();
                ImGui::Image(ImTextureRef(ImTextureID(texture.Srv().ptr)), ImVec2(float(w), float(h)));
                const ImVec2 imageMax(imageMin.x + w, imageMin.y + h);
                DrawCompass(camera, imageMin, imageMax);
                // A part set by hand: its control on it, which takes the pointer before the camera does.
                const bool onControl = AdjusterControl(session, camera, selected, imageMin, imageMax);
                // Where the visible instruments' probes read.
                {
                    const pxr::GfMatrix4d toWorld = session.scene->ModelToWorld(), toCamera = camera.CameraToWorld().GetInverse();
                    const double tanHalf = std::tan(camera.fovDeg * kPi / 360.0), aspect = double(w) / double(h);
                    app::DrawInstrumentMarks(session, [&](const GfVec3d& model, ImVec2& out) {
                        const GfVec3d c = toCamera.Transform(toWorld.Transform(model));
                        if (c[2] > -1e-9)
                            return false;
                        out = ImVec2(float(imageMin.x + (c[0] / (-c[2] * tanHalf * aspect) + 1.0) * 0.5 * w), float(imageMin.y + (1.0 - c[1] / (-c[2] * tanHalf)) * 0.5 * h));
                        return true;
                    }, fonts);
                }
                if (ImGui::IsItemHovered() && !onControl)
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
                    // Middle button, or left button dragged (past ImGui's drag threshold: a click still selects): pans,
                    // moving the view's centre across and up and down with the pointer.
                    else if (mmb || (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(imageMin, imageMax) &&
                                     io.MouseClickedPos[0].x >= imageMin.x && io.MouseClickedPos[0].x < imageMax.x &&
                                     io.MouseClickedPos[0].y >= imageMin.y && io.MouseClickedPos[0].y < imageMax.y))
                        camera.Pan(io.MouseDelta.x, io.MouseDelta.y, h);
                    // The wheel zooms on the point under the cursor (it stays under it), from 0.1 mm (a watch's pivots) to 50 m.
                    if (io.MouseWheel != 0.0f)
                    {
                        const double before = camera.distance;
                        camera.distance = std::clamp(camera.distance * std::pow(0.85, io.MouseWheel), 1e-4, 50.0);
                        GfVec3d origin, direction, right, up, back;
                        camera.Ray((io.MousePos.x - imageMin.x) / w, (io.MousePos.y - imageMin.y) / h, double(w) / h, origin, direction);
                        camera.Axes(right, up, back);
                        // The cursor's point on the plane through the target, facing the camera.
                        const double along = pxr::GfDot(camera.target - origin, -back) / std::max(1e-12, pxr::GfDot(direction, -back));
                        const GfVec3d pointed = origin + direction * along;
                        camera.target += (pointed - camera.target) * (1.0 - camera.distance / before);
                    }
                    // Left click (no drag past the threshold, no Alt): select the part under the cursor.
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt && !space &&
                        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0)
                    {
                        GfVec3d origin, direction;
                        camera.Ray((io.MousePos.x - imageMin.x) / w, (io.MousePos.y - imageMin.y) / h, double(w) / h, origin, direction);
                        selected = session.scene->Pick(origin, direction).part;
                    }
                    // Right click (no drag; middle + right turns the view): the part under the cursor, and what can be measured on it.
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !mmb && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0 &&
                        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0)
                    {
                        GfVec3d origin, direction;
                        camera.Ray((io.MousePos.x - imageMin.x) / w, (io.MousePos.y - imageMin.y) / h, double(w) / h, origin, direction);
                        if (const int part = session.scene->Pick(origin, direction).part; part >= 0)
                        {
                            selected = part;
                            ImGui::OpenPopup("##measure-part");
                        }
                    }
                }
                if (ImGui::BeginPopup("##measure-part"))
                {
                    if (selected >= 0 && selected < int(session.scene->Parts().size()))
                        app::MeasurePartMenu(session, session.scene->Parts()[size_t(selected)].name, session.scene->Parts()[size_t(selected)].body,
                                             RunUserCommand);
                    ImGui::EndPopup();
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
        StudyPanel(session);
        SimulationPanel(session);
        Plots(session);
        if (session.document)
            app::DrawInstrumentPanels(session, fonts, scale, RunUserCommand);
        ai.DrawHub();
        ai.DrawChats();

        // New / Open / Close / Exit with unsaved changes: Save, Don't Save or Cancel.
        if (askToSave)
        {
            ImGui::OpenPopup("Unsaved changes");
            askToSave = false;
        }
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Save the changes to %s?", documentTitle().c_str());
            ImGui::TextDisabled("They are lost otherwise.");
            ImGui::Spacing();
            if (ImGui::Button("Save") || ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                ImGui::CloseCurrentPopup();
                fileActions.push_back([&, action = std::move(pendingAction)] {
                    if (saveDocument())
                        action();
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save"))
            {
                ImGui::CloseCurrentPopup();
                fileActions.push_back(std::move(pendingAction));
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                ImGui::CloseCurrentPopup();
                pendingAction = nullptr;
            }
            ImGui::EndPopup();
        }
        if (showAbout)
        {
            ImGui::OpenPopup("About Tootega AIAD");
            showAbout = false;
        }
        if (ImGui::BeginPopupModal("About Tootega AIAD", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushFont(fonts.bold, fonts.size * 1.2f);
            ImGui::TextUnformatted("Tootega AIAD - Artificial Intelligence-Aided Design");
            ImGui::PopFont();
            ImGui::TextUnformatted("CAD/CAE where the user and an AI operate the same exact B-rep model.");
            ImGui::TextDisabled("OpenUSD 25.11 - USD BRep - Hydra/Aurora (DXR) - MFEM + CUDA - Project Chrono");
            ImGui::TextDisabled("Tootega Pesquisa e Inovação -github.com/HermesSilva/TootegaAIAD");
            if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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

        // Loading: a bar over the window, with the time left estimated from the parts already displayed.
        if (loading.active && session.document)
        {
            const ImGuiViewport* main = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + main->WorkSize.x * 0.5f, main->WorkPos.y + main->WorkSize.y * 0.35f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(520 * scale, 0));
            ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);
            ImGui::Text("Loading %s", documentTitle().c_str());
            const size_t total = std::max<size_t>(loading.parts, 1), left = sceneReady ? session.scene->PendingCount() : total;
            const size_t done = total - std::min(left, total);
            std::string status;
            if (!sceneReady)
                status = "reading the document";
            else if (left > 0)
            {
                const double elapsed = now - loading.viewStarted;
                status = std::format("displaying parts: {} of {}", done, total);
                if (done > 0 && elapsed > 0.5)
                    status += std::format(", about {:.0f} s left", elapsed / double(done) * double(left));
            }
            else
                status = "measuring the machine on the drawing (masses, contacts, springs)";
            ImGui::ProgressBar(loading.measuring || left == 0 ? -1.0f * float(now) : float(done) / float(total), ImVec2(-1, 0));
            ImGui::TextDisabled("%s", status.c_str());
            ImGui::End();
        }

        ImGui::Render();
        // Panels moved, docked, resized or closed: saved a few seconds after they settle (Dear ImGui's
        // saving rate), with the window placement, so a crash loses at most that.
        if (io.WantSaveIniSettings)
        {
            config.SetLayout(ImGui::SaveIniSettingsToMemory());
            config.SetChatWindow(ai.ChatPlacement());
            rememberWindow();
            config.Save();
            io.WantSaveIniSettings = false;
        }
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
                // The panels and the window are kept between runs: what is written is read back.
                const std::filesystem::path configFile = std::filesystem::temp_directory_path() / "aiad_ui_test" / ".aiad" / "aiadconfig.json";
                std::error_code ec;
                std::filesystem::remove(configFile, ec);
                {
                    AppConfig written(configFile);
                    written.SetLayout(ImGui::SaveIniSettingsToMemory());
                    written.SetWindow(RECT{40, 30, 1640, 1030}, false);
                    written.Save();
                }
                const AppConfig readBack(configFile);
                RECT rect{};
                bool maximized = true;
                const bool kept = readBack.Layout().find("[Window][Viewport]") != std::string::npos &&
                                  readBack.Layout().find("[Docking]") != std::string::npos && readBack.Window(rect, maximized) &&
                                  rect.left == 40 && rect.bottom == 1030 && !maximized;
                std::printf("%s panels (docking) and window placement saved to and read back from .aiad\\aiadconfig.json\n", kept ? "ok   " : "FAIL ");
                uiFailures += kept ? 0 : 1;
                // Where the conversation was; then every conversation is closed (its dock node goes away).
                uiChatBefore = ai.ChatPlacement();
                ai.CloseChats();
            }
            std::fflush(stdout);
        }
        else
            g_d3d.EndFrame();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        if (uiTest && uiCaptureFrames == 124)
            ai.OpenNewSession();
        if (uiTest && uiCaptureFrames == 134)
        {
            // The new conversation opens where the last one was.
            const Json after = ai.ChatPlacement();
            bool same = uiChatBefore.is_object() && after.is_object() && !after.value("floating", true) && ai.ChatCount() == 1;
            for (int i = 0; same && i < 4; ++i)
                same = std::abs(after["rect"][i].get<double>() - uiChatBefore["rect"][i].get<double>()) < 12.0;
            std::printf("%s a new conversation opens where the last one was (%s)\n", same ? "ok   " : "FAIL ",
                        after.contains("rect") ? after["rect"].dump().c_str() : "-");
            uiFailures += same ? 0 : 1;
            std::printf("ui-test: %s (%d failure(s))\n", uiFailures ? "FAIL" : "ok", uiFailures);
            std::fflush(stdout);
            done = true;
        }
    }

    if (uiTestThread.joinable())
        uiTestThread.join();
    if (!uiTest)
        StartShutdownWatchdog();
    stopTables();
    bridgeServer.Stop();
    g_d3d.WaitIdle();
    chatTextures.Destroy();
    viewport.reset();
    texture.Destroy(g_d3d);
    // Keep the panels and the window as they are now for the next run.
    config.SetLayout(ImGui::SaveIniSettingsToMemory());
    config.SetChatWindow(ai.ChatPlacement());
    rememberWindow();
    config.Save();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    g_d3d.Destroy();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return uiTest ? (uiFailures ? 1 : 0) : 0;
}
