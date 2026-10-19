#include "AiPanels.h"

#include "Paths.h"
#include "Text.h"

#include <windows.h>
#include <shellapi.h>
#include <wincred.h>

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>

namespace aiad::app
{

namespace
{

using namespace std::chrono;
using ai::ItemKind;

constexpr const char* kAppVersion = "0.1.0";
constexpr double kCacheLife = 3600.0;   // prompt cache TTL [s]
constexpr double kKeepAliveLead = 300.0;
constexpr const char* kKeepAlivePrompt = "keep-alive: answer only \"ok\". Do not use tools and do not change files.";

const ImVec4 kAccent(1.0f, 0.55f, 0.15f, 1.0f);
const ImVec4 kDim(0.62f, 0.66f, 0.72f, 1.0f);
const ImVec4 kGreen(0.40f, 0.82f, 0.45f, 1.0f);
const ImVec4 kRed(0.95f, 0.36f, 0.32f, 1.0f);
const ImVec4 kBlue(0.30f, 0.62f, 1.0f, 1.0f);
const ImVec4 kAmber(0.98f, 0.75f, 0.25f, 1.0f);

double Now()
{
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Thousands(int64_t v)
{
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(size_t(i), ",");
    return (v < 0 ? "-" : "") + s;
}

std::string Compact(double v)
{
    if (v >= 1e9)
        return std::format("{:.1f}B", v / 1e9);
    if (v >= 1e6)
        return std::format("{:.1f}M", v / 1e6);
    if (v >= 1e3)
        return std::format("{:.0f}k", v / 1e3);
    return std::format("{:.0f}", v);
}

std::string Duration(double seconds)
{
    const int s = int(seconds);
    if (s >= 3600)
        return std::format("{}h {}m", s / 3600, (s % 3600) / 60);
    if (s >= 60)
        return std::format("{}m {}s", s / 60, s % 60);
    return std::format("{}s", s);
}

std::string Bytes(uint64_t b)
{
    if (b >= (1ull << 20))
        return std::format("{:.1f} MB", double(b) / double(1 << 20));
    if (b >= 1024)
        return std::format("{:.0f} KB", double(b) / 1024.0);
    return std::format("{} B", b);
}

std::string LocalTime(system_clock::time_point t, const char* format)
{
    const std::time_t tt = system_clock::to_time_t(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}

void SectionTitle(const char* text)
{
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// Two-column "label ... value" row (value right-aligned in the column).
void KeyValue(const char* key, const std::string& value, const ImVec4* colour = nullptr, float columnWidth = 0.0f)
{
    const float x0 = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(key);
    ImGui::SameLine();
    const float w = columnWidth > 0 ? columnWidth : ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - x0;
    const float tw = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - tw));
    if (colour)
        ImGui::TextColored(*colour, "%s", value.c_str());
    else
        ImGui::TextUnformatted(value.c_str());
}

// Thin rounded bar: `fraction` filled with `fill`, the rest in the track colour; `tip` marks a
// second segment (e.g. the part of the context read from cache).
void Bar(float fraction, ImVec4 fill, float height = 7.0f, ImVec4 track = ImVec4(0.12f, 0.36f, 0.66f, 1.0f))
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    draw->AddRectFilled(p, ImVec2(p.x + w, p.y + height), ImGui::GetColorU32(track), height * 0.5f);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f > 0)
        draw->AddRectFilled(p, ImVec2(p.x + std::max(height, w * f), p.y + height), ImGui::GetColorU32(fill), height * 0.5f);
    ImGui::Dummy(ImVec2(w, height + 2));
}

ImVec4 BandColour(double fraction)
{
    return fraction > 0.85 ? kRed : fraction > 0.7 ? kAmber : kGreen;
}

// Small pill button (the hub's account row).
bool Pill(const char* label)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.13f, 0.18f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.24f, 0.36f, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.50f, 0.80f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Wraps the next pill to a new line when it does not fit.
void PillSameLine(const char* nextLabel)
{
    const float next = ImGui::CalcTextSize(nextLabel).x + 16 + ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + next < right)
        ImGui::SameLine();
}

void Spinner(float radius, ImU32 colour)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float t = float(ImGui::GetTime()) * 6.0f;
    const ImVec2 c(p.x + radius, p.y + ImGui::GetTextLineHeight() * 0.5f);
    draw->PathArcTo(c, radius, t, t + 4.2f, 16);
    draw->PathStroke(colour, 0, 2.0f);
    ImGui::Dummy(ImVec2(radius * 2 + 2, ImGui::GetTextLineHeight()));
}

std::string ToolSummary(const std::string& name, const Json& input)
{
    auto field = [&](const char* key) { return input.is_object() && input.contains(key) && input[key].is_string() ? input[key].get<std::string>() : std::string(); };
    if (name == "Bash" || name == "PowerShell")
        return field("command");
    if (name == "Read" || name == "Write" || name == "Edit")
        return field("file_path");
    if (name == "Grep" || name == "Glob")
        return field("pattern");
    if (name == "WebFetch")
        return field("url");
    if (name == "WebSearch")
        return field("query");
    if (name.starts_with("mcp__"))
    {
        std::string args = input.is_object() && !input.empty() ? input.dump() : "";
        return args.size() > 120 ? args.substr(0, 117) + "..." : args;
    }
    if (input.is_object())
        for (const auto& [k, v] : input.items())
            if (v.is_string())
                return v.get<std::string>();
    return {};
}

std::string ToolLabel(const std::string& name)
{
    if (name.starts_with("mcp__"))
    {
        const size_t sep = name.find("__", 5);
        const std::string server = name.substr(5, sep - 5), tool = sep == std::string::npos ? "" : name.substr(sep + 2);
        if (server == "aiad")
            return "AIAD " + (tool.starts_with("aiad_") ? tool.substr(5) : tool);
        return server + " " + tool;
    }
    return name;
}

std::string HtmlEscape(const std::string& s)
{
    std::string out;
    for (const char c : s)
        out += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : std::string(1, c);
    return out;
}

void WriteToVector(void* context, void* data, int size)
{
    auto* v = static_cast<std::vector<uint8_t>*>(context);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

// Clipboard bitmap (CF_DIB) as PNG, empty when the clipboard holds no image.
std::vector<uint8_t> ClipboardPng()
{
    std::vector<uint8_t> png;
    if (!IsClipboardFormatAvailable(CF_DIB) || !OpenClipboard(nullptr))
        return png;
    if (HANDLE h = GetClipboardData(CF_DIB))
        if (const auto* info = static_cast<const BITMAPINFOHEADER*>(GlobalLock(h)))
        {
            const int w = info->biWidth, hgt = std::abs(info->biHeight), bpp = info->biBitCount;
            if ((bpp == 32 || bpp == 24) && w > 0 && hgt > 0 && info->biCompression != BI_JPEG && info->biCompression != BI_PNG)
            {
                const uint8_t* bits = reinterpret_cast<const uint8_t*>(info) + info->biSize + (info->biCompression == BI_BITFIELDS ? 12 : 0) +
                                      info->biClrUsed * 4;
                const int stride = ((w * bpp + 31) / 32) * 4;
                std::vector<uint8_t> rgba(size_t(w) * hgt * 4);
                for (int y = 0; y < hgt; ++y)
                {
                    const uint8_t* row = bits + size_t(info->biHeight > 0 ? hgt - 1 - y : y) * stride;
                    for (int x = 0; x < w; ++x)
                    {
                        uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                        o[0] = row[x * (bpp / 8) + 2];
                        o[1] = row[x * (bpp / 8) + 1];
                        o[2] = row[x * (bpp / 8) + 0];
                        o[3] = 255;
                    }
                }
                stbi_write_png_to_func(WriteToVector, &png, w, hgt, 4, rgba.data(), w * 4);
            }
            GlobalUnlock(h);
        }
    CloseClipboard();
    return png;
}

std::string StoredApiKey()
{
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(L"TootegaAIAD/ANTHROPIC_API_KEY", CRED_TYPE_GENERIC, 0, &credential))
        return {};
    std::string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return key;
}

bool StoreApiKey(const std::string& key)
{
    if (key.empty())
        return CredDeleteW(L"TootegaAIAD/ANTHROPIC_API_KEY", CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(L"TootegaAIAD/ANTHROPIC_API_KEY");
    credential.CredentialBlobSize = DWORD(key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"ANTHROPIC_API_KEY");
    return CredWriteW(&credential, 0);
}

std::string DomainPrompt()
{
    return "You are operating Tootega AIAD, a CAD/CAE application (Windows, DirectX 12, CUDA). The open model is a single-cylinder "
           "four-stroke engine with a flywheel magneto; its design parameters generate every drawn part, and every simulated quantity "
           "(masses, inertias, valve lift, chamber volume, magneto) is derived from the drawing.\n"
           "You change the model ONLY through the `aiad` MCP tools (mcp__aiad__*), which run the app's Command Bus: the same commands, "
           "undo and audit trail as the user. Read before you change (aiad_engine_get_design, aiad_engine_get_derived, aiad_parts_list); "
           "after a change, verify it (aiad_viewport_snapshot, aiad_sim_run + aiad_sim_job_status). Simulations are background jobs: poll "
           "their status. Lengths are in millimetres and angles in crank degrees of the 720-degree cycle (0 = firing TDC) unless a tool "
           "says otherwise. Every change you make is visible to the user and can be undone with aiad_undo or Ctrl+Z. The aiad-mcp skill "
           "describes the tools, the feature format (planes, sketches, formulas) and this model: load it before changing the model.";
}

}  // namespace

// ---- Settings -------------------------------------------------------------------------------------

AiSettings AiSettings::Load()
{
    AiSettings s;
    const Json j = ParseJson(ReadTextFile(AppDataDir() / "ai-settings.json"));
    if (j.is_object())
    {
        s.claudePath = j.value("claudePath", s.claudePath);
        s.model = j.value("model", s.model);
        s.effort = j.value("effort", s.effort);
        s.permissionMode = j.value("permissionMode", s.permissionMode);
        s.askLanguage = j.value("askLanguage", s.askLanguage);
        s.quietPrompt = j.value("quietPrompt", s.quietPrompt);
        s.systemPrompt = j.value("systemPrompt", s.systemPrompt);
        s.agentsAndWorkflows = j.value("agentsAndWorkflows", s.agentsAndWorkflows);
        s.keepCacheAlive = j.value("keepCacheAlive", s.keepCacheAlive);
        s.showThinking = j.value("showThinking", s.showThinking);
        s.verbosity = j.value("verbosity", s.verbosity);
    }
    return s;
}

void AiSettings::Save() const
{
    const Json j = {{"claudePath", claudePath},
                    {"model", model},               {"effort", effort},
                    {"permissionMode", permissionMode}, {"askLanguage", askLanguage},
                    {"quietPrompt", quietPrompt},   {"systemPrompt", systemPrompt},
                    {"agentsAndWorkflows", agentsAndWorkflows}, {"keepCacheAlive", keepCacheAlive},
                    {"showThinking", showThinking}, {"verbosity", verbosity}};
    WriteTextFile(AppDataDir() / "ai-settings.json", j.dump(2));
}

// ---- Lifetime -------------------------------------------------------------------------------------

AiPanels::AiPanels(AiHost host, UiFonts fonts) : m_host(std::move(host)), m_fonts(fonts), m_settings(AiSettings::Load())
{
    // The agent works in the folder of the open model (its saved contexts are per folder too).
    m_settings.workspace = m_host.workspace.empty() ? Narrow((HomeDir() / "Documents" / "Tootega AIAD").wstring()) : m_host.workspace;
    std::error_code created;
    std::filesystem::create_directories(Widen(m_settings.workspace), created);
    if (const std::string key = StoredApiKey(); !key.empty())
        SetEnvironmentVariableA("ANTHROPIC_API_KEY", key.c_str());
    // MCP config: aiad-mcp.exe pinned to this instance's bridge.
    m_mcpConfig = AppDataDir() / std::format("mcp-config-{}.json", GetCurrentProcessId());
    const Json config = {{"mcpServers",
                          {{"aiad",
                            {{"type", "stdio"},
                             {"command", Narrow(m_host.mcpExe.wstring())},
                             {"args", Json::array()},
                             {"env", {{"AIAD_BRIDGE_URL", m_host.bridgeUrl}, {"AIAD_BRIDGE_TOKEN", m_host.bridgeToken}}}}}}}};
    WriteTextFile(m_mcpConfig, config.dump(2));
    // The tool catalog for aiad-mcp when the app is not running (it lists them, calls explain).
    if (m_host.bus)
        WriteTextFile(AppDataDir() / "catalog.json", m_host.bus->CatalogJson().dump(2));

    const std::string claudePath = m_settings.claudePath;
    m_cliVersionFuture = std::async(std::launch::async, [claudePath] { return ai::ClaudeVersion(ai::ResolveClaude(claudePath)); });
    m_latestFuture = std::async(std::launch::async, [] { return ai::LatestCliVersion(); });
    m_pricesFuture = std::async(std::launch::async, [] { return ai::LoadPricing(); });
    m_modelsFuture = std::async(std::launch::async, [] {
        std::string error;
        return ai::FetchModels(error);
    });
    m_usageFuture = std::async(std::launch::async, [] { return ai::FetchAccountUsage(); });
    m_usageAt = Now();
    // No conversation is open at start: "+ New session" or a saved context opens one (its own tab).
    m_transcripts = ai::ListTranscripts(Widen(m_settings.workspace));
    m_idle = std::make_unique<ai::AgentSession>(BuildOptions());
    m_session = m_idle.get();
}

AiPanels::~AiPanels()
{
    for (auto& tab : m_tabs)
        tab->session->Stop();
    std::error_code ec;
    std::filesystem::remove(m_mcpConfig, ec);
}

ai::SessionOptions AiPanels::BuildOptions() const
{
    ai::SessionOptions o;
    o.claudePath = m_settings.claudePath;
    o.cwd = Widen(m_settings.workspace);
    o.model = m_settings.model;
    o.effort = m_settings.effort;
    o.permissionMode = m_settings.permissionMode;
    o.mcpConfig = m_mcpConfig;
    o.askLanguage = m_settings.askLanguage;
    // Read-only Command Bus tools need no approval.
    if (m_host.bus)
        for (const cmd::CommandSpec& c : m_host.bus->Catalog())
            if (c.flags.readOnly)
                o.allowedTools.push_back("mcp__aiad__" + c.name);
    if (!m_settings.agentsAndWorkflows)
        o.disallowedTools = {"Task", "Workflow"};
    std::string prompt = m_settings.quietPrompt;
    prompt += (prompt.empty() ? "" : "\n\n") + DomainPrompt();
    if (!m_settings.systemPrompt.empty())
        prompt += "\n\n" + m_settings.systemPrompt;
    o.appendSystemPrompt = prompt;
    return o;
}

void AiPanels::Activate(ChatTab* tab)
{
    m_tab = tab;
    m_session = tab ? tab->session.get() : m_idle.get();
}

// Settings (model, effort, mode, prompts, credentials) apply to every open conversation on its next send.
void AiPanels::ApplyOptions()
{
    for (auto& tab : m_tabs)
        tab->session->SetOptions(BuildOptions());
    m_idle->SetOptions(BuildOptions());
}

void AiPanels::OpenNewSession()
{
    OpenSession({});
}

void AiPanels::OpenSession(const std::string& resumeId)
{
    // A session already open in a tab is focused, never opened twice (two CLIs on one transcript).
    if (!resumeId.empty())
        for (auto& tab : m_tabs)
            if (tab->session->SessionId() == resumeId)
            {
                tab->focus = true;
                Activate(tab.get());
                return;
            }
    auto tab = std::make_unique<ChatTab>();
    tab->id = m_nextTab++;
    ai::SessionOptions o = BuildOptions();
    o.resumeId = resumeId;
    tab->session = std::make_unique<ai::AgentSession>(o);
    ChatTab* raw = tab.get();
    tab->session->OnTurnEnd = [this, raw] {
        ChatTab* previous = m_tab;
        m_tab = raw;
        AutoSaveTimeline();
        m_tab = previous;
        RefreshTranscripts();
    };
    if (!resumeId.empty())
        tab->session->LoadTimeline(ai::LoadTranscript(Widen(m_settings.workspace), resumeId));
    m_tabs.push_back(std::move(tab));
    Activate(raw);
}

void AiPanels::LoadFixture(const std::vector<Json>& events, const std::string& prompt)
{
    if (!m_tab)
        OpenNewSession();
    ai::SessionOptions o = BuildOptions();
    o.offline = true;
    m_tab->session = std::make_unique<ai::AgentSession>(o);
    Activate(m_tab);
    m_session->Send(prompt, {}, m_host.bus ? m_host.bus->Depth() : 0);
    for (const Json& e : events)
        m_session->Feed(e);
    m_tab->scrollToBottom = true;
}

void AiPanels::RefreshTranscripts()
{
    if (m_transcriptsFuture.valid())
        return;
    const std::filesystem::path cwd = Widen(m_settings.workspace);
    m_transcriptsFuture = std::async(std::launch::async, [cwd] { return ai::ListTranscripts(cwd); });
}

void AiPanels::Pump()
{
    auto ready = [](auto& f) { return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    if (ready(m_cliVersionFuture))
        m_cliVersion = ai::ParseSemver(m_cliVersionFuture.get());
    if (ready(m_latestFuture))
        m_latestVersion = m_latestFuture.get();
    if (ready(m_pricesFuture))
        m_prices = m_pricesFuture.get();
    if (ready(m_usageFuture))
        m_usage = m_usageFuture.get();
    if (ready(m_transcriptsFuture))
        m_transcripts = m_transcriptsFuture.get();
    if (ready(m_modelsFuture))
        m_models = m_modelsFuture.get();
    if (ready(m_authFuture))
        m_authStatus = m_authFuture.get().output;
    if (ready(m_pluginsFuture))
    {
        const ai::CapturedRun run = m_pluginsFuture.get();
        const size_t start = run.output.find_first_of("[{");
        m_plugins = start == std::string::npos ? Json() : ParseJson(std::string_view(run.output).substr(start));
        m_pluginsText = m_plugins.is_discarded() || m_plugins.is_null() ? run.output : "";
    }
    // Account usage every 2 minutes (no tokens spent).
    if (Now() - m_usageAt > 120.0 && !m_usageFuture.valid())
    {
        m_usageAt = Now();
        m_usageFuture = std::async(std::launch::async, [] { return ai::FetchAccountUsage(); });
    }
    // Every conversation keeps running, visible or not.
    ChatTab* active = m_tab;
    for (auto& tab : m_tabs)
    {
        Activate(tab.get());
        m_session->Pump();
        if (m_session->Revision() != m_tab->seenRevision)
        {
            m_tab->seenRevision = m_session->Revision();
            m_tab->scrollToBottom = true;
        }
        // Keep-alive: re-arm the 1 h cache before it lapses, never revive an expired one.
        const auto& st = m_session->Stats();
        if (m_settings.keepCacheAlive && !m_session->Busy() && !m_session->SessionId().empty() && st.lastTurnEnd.time_since_epoch().count() > 0)
        {
            const double age = duration<double>(system_clock::now() - st.lastTurnEnd).count();
            if (age > kCacheLife - kKeepAliveLead && age < kCacheLife && Now() - m_tab->lastKeepAlive > 600.0)
            {
                m_tab->lastKeepAlive = Now();
                m_session->Send(kKeepAlivePrompt, {}, m_host.bus ? m_host.bus->Depth() : 0);
            }
        }
    }
    Activate(active);
}
// ---- Hub ------------------------------------------------------------------------------------------

void AiPanels::DrawHeader()
{
    // Logo: orange ring with "AI".
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 11.0f;
    const ImVec2 c(p.x + r, p.y + r);
    draw->AddCircleFilled(c, r, IM_COL32(28, 34, 46, 255));
    draw->AddCircle(c, r, ImGui::GetColorU32(kAccent), 0, 2.5f);
    draw->AddText(m_fonts.bold, m_fonts.size * 0.72f, ImVec2(c.x - 6.5f, c.y - 6.5f), ImGui::GetColorU32(kAccent), "AI");
    ImGui::Dummy(ImVec2(2 * r + 4, 2 * r));
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    ImGui::PushFont(m_fonts.bold, m_fonts.size * 1.1f);
    ImGui::TextUnformatted("Tootega AIAD");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "v%s", kAppVersion);
    ImGui::SameLine();
    const bool outdated = ai::IsOutdated(m_cliVersion, m_latestVersion);
    if (m_cliVersion.empty())
        ImGui::TextColored(kDim, "Claude CLI ...");
    else
        ImGui::TextColored(outdated ? kRed : kDim, "Claude CLI %s", m_cliVersion.c_str());
    if (ImGui::IsItemHovered() && !m_latestVersion.empty())
        ImGui::SetTooltip("Latest: %s (click for the release notes)", m_latestVersion.c_str());
    if (ImGui::IsItemClicked())
        ShellExecuteW(nullptr, L"open", L"https://github.com/anthropics/claude-code/releases", nullptr, nullptr, SW_SHOWNORMAL);
    if (outdated)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.18f, 0.05f, 1));
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        if (ImGui::SmallButton("^ Update"))
            ai::RunInConsole(ai::ResolveClaude(m_settings.claudePath), {"update"}, Widen(m_settings.workspace));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    ImGui::Dummy(ImVec2(1, 2));
    const bool signedIn = !ai::ReadOauthToken().empty() || !StoredApiKey().empty();
    if (Pill("Usage"))
    {
        m_showUsage = true;
        m_usageAt = Now();
        if (!m_usageFuture.valid())
            m_usageFuture = std::async(std::launch::async, [] { return ai::FetchAccountUsage(); });
    }
    PillSameLine(signedIn ? "Sign out" : "Sign in");
    if (Pill(signedIn ? "Sign out" : "Sign in"))
        ai::RunInConsole(ai::ResolveClaude(m_settings.claudePath), {"auth", signedIn ? "logout" : "login"}, Widen(m_settings.workspace));
    PillSameLine("Plugins");
    if (Pill("Plugins"))
    {
        m_showPlugins = true;
        if (!m_pluginsFuture.valid())
        {
            const auto claude = ai::ResolveClaude(m_settings.claudePath);
            const std::filesystem::path cwd = Widen(m_settings.workspace);
            m_pluginsFuture = std::async(std::launch::async, [claude, cwd] { return ai::RunCaptured(claude, {"plugin", "list", "--json", "--available"}, cwd); });
        }
    }
    PillSameLine("MCP");
    if (Pill("MCP"))
        m_showMcp = true;
    PillSameLine("Skills");
    if (Pill("Skills"))
    {
        m_showSkills = true;
        m_session->RequestControl("get_context_usage", [this](const Json& r) { m_contextUsage = r; });
    }
    PillSameLine("Credentials vault");
    if (Pill("Credentials vault"))
    {
        m_showCredentials = true;
        if (!m_authFuture.valid())
        {
            const auto claude = ai::ResolveClaude(m_settings.claudePath);
            m_authFuture = std::async(std::launch::async, [claude] { return ai::RunCaptured(claude, {"auth", "status"}, std::filesystem::temp_directory_path()); });
        }
    }
    PillSameLine("Settings");
    if (Pill("Settings"))
        m_showSettings = true;
    ImGui::Separator();
}

void AiPanels::DrawMeters()
{
    const ai::SessionStats& st = m_session->Stats();
    // Context window.
    const double used = double(st.contextUsed), limit = double(std::max<int64_t>(1, st.contextLimit));
    SectionTitle("CONTEXT WINDOW");
    if (m_session->Busy())
    {
        ImGui::SameLine();
        Spinner(5.0f, ImGui::GetColorU32(kAccent));
    }
    ImGui::SameLine();
    const std::string percent = std::format("{:.0f}%", 100.0 * used / limit);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(percent.c_str()).x);
    ImGui::PushFont(m_fonts.bold, 0.0f);
    ImGui::TextUnformatted(percent.c_str());
    ImGui::PopFont();
    Bar(float(used / limit), BandColour(used / limit));
    ImGui::Text("%s / %s tokens", Thousands(st.contextUsed).c_str(), Thousands(st.contextLimit).c_str());
    ImGui::SameLine(0, 4);
    ImGui::TextColored(kDim, "- %s tokens left", Thousands(st.contextLimit - st.contextUsed).c_str());
    if (used / limit > 0.85)
        ImGui::TextColored(kRed, "Context near the limit: the CLI will compact it soon.");
    KeyValue("Duration", Duration(duration<double>(steady_clock::now() - st.sessionStart).count()));

    // Cache life (1 h TTL from the last turn).
    const bool hasTurn = st.lastTurnEnd.time_since_epoch().count() > 0;
    const double age = hasTurn ? duration<double>(system_clock::now() - st.lastTurnEnd).count() : kCacheLife;
    const double left = std::max(0.0, kCacheLife - age);
    KeyValue("Cache life", hasTurn ? (left > 0 ? Duration(left) + " left" : std::string("expired")) : std::string("-"), left > 0 ? &kGreen : &kDim);
    Bar(float(left / kCacheLife), kGreen, 6.0f);
    if (ImGui::Checkbox("Keep cache alive (refresh before 1h)", &m_settings.keepCacheAlive))
        m_settings.Save();

    // Tokens | Cache.
    const auto price = ai::PriceFor(m_prices, m_session->ActiveModel().empty() ? m_settings.model : m_session->ActiveModel());
    const double savings = price ? double(st.cacheReadTokens) * price->input * 0.9 / 1e6 : 0.0;
    if (ImGui::BeginTable("##tokens_cache", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();
        SectionTitle("TOKENS");
        const float w = ImGui::GetContentRegionAvail().x - 10;
        KeyValue("Input", Thousands(st.inputTokens), nullptr, w);
        KeyValue("Output", Thousands(st.outputTokens), nullptr, w);
        ImGui::TableNextColumn();
        SectionTitle("CACHE");
        const float w2 = ImGui::GetContentRegionAvail().x;
        KeyValue("Hit rate", std::format("{:.0f}%", st.CacheHitRate() * 100), nullptr, w2);
        KeyValue("Read", Compact(double(st.cacheReadTokens)), nullptr, w2);
        KeyValue("Write", Compact(double(st.cacheCreateTokens)), nullptr, w2);
        KeyValue("Est. savings", price ? std::format("${:.2f}", savings) : std::string("-"), nullptr, w2);
        ImGui::EndTable();
    }
    SectionTitle("COST");
    KeyValue("Session", std::format("${:.2f}", st.costUsd));
    KeyValue("Last turn", std::format("${:.3f}", st.lastTurnCostUsd));

    SectionTitle("SESSION ACTIVITY");
    KeyValue("Turns", Thousands(st.turns));
    KeyValue("Peak context", std::format("{} ({:.0f}%)", Compact(double(st.peakContext)), 100.0 * double(st.peakContext) / limit));
    KeyValue("Compactions", std::to_string(st.compactions));
    KeyValue("Cache resets", st.cacheResets ? std::format("{} - ${:.2f}", st.cacheResets, st.cacheResetCostUsd) : std::string("0"));
    if (st.fiveHour.utilization >= 0 || !st.fiveHour.status.empty())
        KeyValue("5-hour limit", st.fiveHour.utilization >= 0 ? std::format("{:.0f}%", st.fiveHour.utilization * 100) : st.fiveHour.status,
                 st.fiveHour.status == "rejected" ? &kRed : st.fiveHour.status == "allowed_warning" ? &kAmber : nullptr);
    ImGui::Separator();
}

void AiPanels::DrawControls()
{
    bool changed = false;
    if (ImGui::BeginTable("##controls", 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("e", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Model");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Effort");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Permission");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string modelLabel = m_settings.model.empty() ? "Default (" + (m_session->ActiveModel().empty() ? std::string("CLI") : m_session->ActiveModel()) + ")"
                                                          : m_settings.model;
        for (const ai::ModelInfo& m : m_models)
            if (m.id == m_settings.model.substr(0, m_settings.model.find('[')))
                modelLabel = m.displayName + (m_settings.model.ends_with("[1m]") ? " 1M" : "");
        if (ImGui::BeginCombo("##model", modelLabel.c_str()))
        {
            if (ImGui::Selectable("Default (CLI setting)", m_settings.model.empty()))
                m_settings.model.clear(), changed = true;
            for (const ai::ModelInfo& m : m_models)
            {
                if (ImGui::Selectable(m.displayName.c_str(), m_settings.model == m.id))
                    m_settings.model = m.id, changed = true;
                if (m.maxInputTokens >= 1000000 && ImGui::Selectable((m.displayName + " 1M").c_str(), m_settings.model == m.id + "[1m]"))
                    m_settings.model = m.id + "[1m]", changed = true;
            }
            if (m_models.empty())
                ImGui::TextDisabled("(model list unavailable: no credentials or offline)");
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        static const char* efforts[] = {"", "low", "medium", "high", "xhigh", "max"};
        static const char* effortLabels[] = {"Default", "Low", "Medium", "High", "XHigh", "Max"};
        int effort = 0;
        for (int i = 0; i < 6; ++i)
            if (m_settings.effort == efforts[i])
                effort = i;
        if (ImGui::Combo("##effort", &effort, effortLabels, 6))
            m_settings.effort = efforts[effort], changed = true;
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        static const char* modes[] = {"default", "plan", "acceptEdits", "auto", "dontAsk", "bypassPermissions"};
        static const char* modeLabels[] = {"Ask", "Plan", "Accept edits", "Auto", "Don't ask", "Bypass"};
        int mode = 0;
        for (int i = 0; i < 6; ++i)
            if (m_settings.permissionMode == modes[i])
                mode = i;
        if (ImGui::Combo("##mode", &mode, modeLabels, 6))
            m_settings.permissionMode = modes[mode], changed = true;
        ImGui::EndTable();
    }
    if (ImGui::Checkbox("Agents & workflows", &m_settings.agentsAndWorkflows))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: subagents (Task) and workflows are disabled; they spend a lot of tokens.");
    if (changed)
    {
        m_settings.Save();
        ApplyOptions();  // takes effect on the next send, same session
    }
    ImGui::Separator();
}

void AiPanels::DrawSavedContexts()
{
    SectionTitle("SAVED CONTEXTS");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.28f, 0.34f, 1));
    ImGui::SmallButton(std::to_string(m_transcripts.size()).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (Pill("+ New session"))
        OpenNewSession();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Border, kRed);
    if (Pill("Delete all"))
        m_confirmDeleteAll = true;
    ImGui::PopStyleColor();
    ImGui::TextColored(kBlue, "%s", m_settings.workspace.c_str());
    if (ImGui::IsItemClicked())
        ShellExecuteW(nullptr, L"open", Widen(m_settings.workspace).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    // Grouped by time like the Cockpit: Today / Yesterday / Last 7 days / Older.
    const auto now = system_clock::now();
    const std::string today = LocalTime(now, "%Y-%m-%d"), yesterday = LocalTime(now - hours(24), "%Y-%m-%d");
    const char* group = nullptr;
    const float cardWidth = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
    for (const ai::TranscriptInfo& t : m_transcripts)
    {
        const std::string day = LocalTime(t.updated, "%Y-%m-%d");
        const char* g = day == today ? "TODAY" : day == yesterday ? "YESTERDAY" : now - t.updated < hours(24 * 7) ? "LAST 7 DAYS" : "OLDER";
        if (g != group)
        {
            group = g;
            SectionTitle(g);
        }
        ImGui::PushID(t.id.c_str());
        // Open in a tab (spinner while it works); the active tab gets the thick border.
        ai::AgentSession* openSession = nullptr;
        for (auto& tab : m_tabs)
            if (tab->session->SessionId() == t.id)
                openSession = tab->session.get();
        const bool active = openSession != nullptr;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight() * 3.6f;
        ImGui::InvisibleButton("card", ImVec2(cardWidth, h));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            OpenSession(t.id);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Rename"))
            {
                m_renaming = t.id;
                strncpy_s(m_renameBuffer, t.title.c_str(), _TRUNCATE);
            }
            if (ImGui::MenuItem("Delete"))
                m_confirmDelete = t.id;
            if (ImGui::MenuItem("Remote control (open in a terminal)"))
                ai::RunInConsole(ai::ResolveClaude(m_settings.claudePath), {"--remote-control", "--resume", t.id}, Widen(m_settings.workspace));
            ImGui::EndPopup();
        }
        draw->AddRectFilled(p, ImVec2(p.x + cardWidth, p.y + h), IM_COL32(18, 32, 52, 255), 6.0f);
        draw->AddRect(p, ImVec2(p.x + cardWidth, p.y + h), active ? IM_COL32(60, 150, 255, 255) : hovered ? IM_COL32(80, 110, 150, 255) : IM_COL32(45, 60, 80, 255),
                      6.0f, 0, openSession == m_session ? 2.0f : 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y + 6));
        if (openSession && openSession->Busy())
        {
            Spinner(5.0f, ImGui::GetColorU32(kAccent));
            ImGui::SameLine();
        }
        if (m_renaming == t.id)
        {
            ImGui::SetNextItemWidth(cardWidth - 30);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                ai::SetCustomTitle(t.id, m_renameBuffer);
                m_renaming.clear();
                RefreshTranscripts();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_renaming.clear();
        }
        else
        {
            ImGui::PushFont(m_fonts.bold, 0.0f);
            ImGui::PushClipRect(p, ImVec2(p.x + cardWidth - 8, p.y + h), true);
            ImGui::TextUnformatted(t.title.empty() ? "(untitled)" : t.title.c_str());
            ImGui::PopClipRect();
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y + h - ImGui::GetTextLineHeight() * 1.45f));
        ImGui::TextColored(kAmber, "%s - %d msgs", LocalTime(t.updated, "%d/%m/%Y, %H:%M").c_str(), t.messages);
        ImGui::SameLine();
        ImGui::TextColored(kDim, "- %s", Bytes(t.sizeBytes).c_str());
        if (hovered)
            ImGui::SetTooltip("%s\n%d prompts, %d answers, %d tool calls\nmodel %s\nRight click: rename, delete, remote control", t.id.c_str(), t.userPrompts,
                              t.assistantMessages, t.toolCalls, t.model.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        ImGui::Dummy(ImVec2(cardWidth, 6));  // the gap is an item: ImGui sizes the window by items
        ImGui::PopID();
    }
    if (m_transcripts.empty())
        ImGui::TextDisabled("No saved context in this folder yet.");

    // Confirmations (irreversible).
    if (m_confirmDeleteAll)
        ImGui::OpenPopup("Delete all contexts?");
    if (!m_confirmDelete.empty())
        ImGui::OpenPopup("Delete context?");
    if (ImGui::BeginPopupModal("Delete all contexts?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete the %zu saved transcripts of\n%s?\nThis cannot be undone.", m_transcripts.size(), m_settings.workspace.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.12f, 1));
        if (ImGui::Button("Delete all") )
        {
            for (auto& tab : m_tabs)  // their transcripts are going away
            {
                tab->session->Stop();
                tab->open = false;
            }
            ai::DeleteAllTranscripts(Widen(m_settings.workspace));
            RefreshTranscripts();
            m_confirmDeleteAll = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_confirmDeleteAll = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete context?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete this saved transcript? This cannot be undone.");
        if (ImGui::Button("Delete"))
        {
            for (auto& tab : m_tabs)
                if (tab->session->SessionId() == m_confirmDelete)
                {
                    tab->session->Stop();
                    tab->open = false;
                }
            ai::DeleteTranscript(Widen(m_settings.workspace), m_confirmDelete);
            RefreshTranscripts();
            m_confirmDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_confirmDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AiPanels::DrawHub()
{
    ImGui::Begin("AI Hub");
    if (m_session)
    {
        DrawHeader();
        DrawMeters();
        DrawControls();
        DrawSavedContexts();
    }
    ImGui::End();
    DrawUsageModal();
    DrawMcpModal();
    DrawSkillsModal();
    DrawPluginsModal();
    DrawCredentialsModal();
    DrawSettingsModal();
}

// ---- Modals ---------------------------------------------------------------------------------------

void AiPanels::DrawUsageModal()
{
    if (!m_showUsage)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Usage", &m_showUsage))
    {
        SectionTitle("ACCOUNT LIMITS (OAuth /usage, no tokens spent)");
        if (m_usageFuture.valid())
            Spinner(6, ImGui::GetColorU32(kAccent));
        if (!m_usage.ok)
            ImGui::TextColored(kAmber, "%s", m_usage.error.empty() ? "Reading..." : m_usage.error.c_str());
        for (const ai::LimitWindow& w : m_usage.windows)
        {
            const ImVec4 band = BandColour(w.used);
            KeyValue(w.label.c_str(), std::format("{:.0f}%", w.used * 100), &band);
            Bar(float(w.used), BandColour(w.used));
            if (!w.resetsAt.empty())
                ImGui::TextColored(kDim, "resets %s", w.resetsAt.c_str());
        }
        const ai::SessionStats& st = m_session->Stats();
        SectionTitle("WHERE YOUR TOKENS WENT (this session, results ~4 chars/token)");
        std::vector<std::pair<std::string, ai::ToolUsage>> tools(st.tools.begin(), st.tools.end());
        std::sort(tools.begin(), tools.end(), [](const auto& a, const auto& b) { return a.second.resultChars > b.second.resultChars; });
        if (ImGui::BeginTable("##tools", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("Tool");
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Context in", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();
            for (const auto& [name, u] : tools)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ToolLabel(name).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", u.calls);
                ImGui::TableNextColumn();
                ImGui::Text("~%s tk", Compact(double(u.resultChars) / 4.0).c_str());
            }
            ImGui::EndTable();
        }
        if (tools.empty())
            ImGui::TextDisabled("No tool calls yet.");
        SectionTitle("SESSION");
        KeyValue("Cost (from the CLI)", std::format("${:.4f}", st.costUsd));
        KeyValue("Active time", Duration(st.activeSeconds));
        KeyValue("Turns", std::to_string(st.turns));
    }
    ImGui::End();
}

void AiPanels::DrawMcpModal()
{
    if (!m_showMcp)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MCP servers", &m_showMcp))
    {
        const Json& servers = m_session->McpServers();
        if (servers.empty())
            ImGui::TextWrapped("The list arrives with the first turn of the session (system/init). Send a message to load it.");
        for (const Json& s : servers)
        {
            const std::string name = s.value("name", ""), status = s.value("status", "");
            const ImVec4 colour = status == "connected" ? kGreen : status == "pending" ? kAmber : kRed;
            ImGui::PushID(name.c_str());
            if (ImGui::CollapsingHeader((name + "##h").c_str(), name == "aiad" ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            {
                ImGui::TextColored(colour, "%s", status.c_str());
                const std::string prefix = "mcp__" + name + "__";
                for (const std::string& t : m_session->Tools())
                    if (t.starts_with(prefix))
                        ImGui::BulletText("%s", t.substr(prefix.size()).c_str());
            }
            else
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
                ImGui::TextColored(colour, "%s", status.c_str());
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextColored(kDim, "aiad bridge: %s", m_host.bridgeUrl.c_str());
        ImGui::TextColored(kDim, "Connect or approve other servers with /mcp in the CLI.");
    }
    ImGui::End();
}

void AiPanels::DrawSkillsModal()
{
    if (!m_showSkills)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Skills", &m_showSkills))
    {
        ImGui::TextWrapped("Every listed skill keeps its name and description in the context of every turn; a triggered skill loads its whole body. "
                           "Figures from get_context_usage (computed by the CLI: no turn, no tokens).");
        if (m_contextUsage.is_object())
        {
            // Tolerant: any array of objects with a name and a token count.
            for (const auto& [key, value] : m_contextUsage.items())
            {
                if (value.is_number())
                    KeyValue(key.c_str(), Thousands(value.get<int64_t>()));
                if (!value.is_array())
                    continue;
                SectionTitle(key.c_str());
                for (const Json& row : value)
                    if (row.is_object() && row.contains("name"))
                    {
                        const int64_t tokens = row.value("tokens", row.value("metadataTokens", int64_t(0)));
                        KeyValue(row.value("name", "").c_str(), tokens ? Thousands(tokens) + " tk" : row.value("source", ""));
                    }
            }
        }
        else
            ImGui::TextDisabled("Asking the CLI...");
        if (!m_session->Skills().empty())
        {
            SectionTitle("LOADED IN THIS SESSION");
            for (const std::string& s : m_session->Skills())
                ImGui::BulletText("%s", s.c_str());
        }
    }
    ImGui::End();
}

void AiPanels::DrawPluginsModal()
{
    if (!m_showPlugins)
        return;
    ImGui::SetNextWindowSize(ImVec2(600, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plugins", &m_showPlugins))
    {
        if (m_pluginsFuture.valid())
        {
            Spinner(6, ImGui::GetColorU32(kAccent));
            ImGui::SameLine();
            ImGui::TextDisabled("claude plugin list --json --available");
        }
        auto action = [&](std::vector<std::string> args) {
            const auto claude = ai::ResolveClaude(m_settings.claudePath);
            const std::filesystem::path cwd = Widen(m_settings.workspace);
            m_pluginsFuture = std::async(std::launch::async, [claude, cwd, args] {
                ai::RunCaptured(claude, args, cwd, 180000);
                return ai::RunCaptured(claude, {"plugin", "list", "--json", "--available"}, cwd);
            });
        };
        Json installed = m_plugins.is_object() ? m_plugins.value("installed", Json::array()) : m_plugins.is_array() ? m_plugins : Json::array();
        Json available = m_plugins.is_object() ? m_plugins.value("available", Json::array()) : Json::array();
        SectionTitle("INSTALLED");
        for (const Json& p : installed)
        {
            const std::string id = p.value("id", p.value("name", ""));
            const bool enabled = p.value("enabled", true);
            ImGui::PushID(id.c_str());
            ImGui::TextUnformatted(id.c_str());
            ImGui::SameLine();
            ImGui::TextColored(kDim, "%s", p.value("version", "").c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
            if (ImGui::SmallButton(enabled ? "Disable" : "Enable"))
                action({"plugin", enabled ? "disable" : "enable", id});
            ImGui::SameLine();
            if (ImGui::SmallButton("Uninstall"))
                action({"plugin", "uninstall", id});
            ImGui::PopID();
        }
        if (installed.empty() && !m_pluginsFuture.valid())
            ImGui::TextDisabled("None.");
        SectionTitle("AVAILABLE");
        for (const Json& p : available)
        {
            const std::string id = p.value("pluginId", p.value("id", p.value("name", "")));
            ImGui::PushID(id.c_str());
            ImGui::TextUnformatted(id.c_str());
            if (p.contains("description") && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.value("description", "").c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
            if (ImGui::SmallButton("Install"))
                action({"plugin", "install", id});
            ImGui::PopID();
        }
        if (!m_pluginsText.empty())
            RenderMono(m_pluginsText, m_fonts, ImGui::GetContentRegionAvail().x);
    }
    ImGui::End();
}

void AiPanels::DrawCredentialsModal()
{
    if (!m_showCredentials)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Credentials vault", &m_showCredentials))
    {
        ImGui::TextWrapped("Credentials are read-only for the app: never written to files, never logged. The Claude login belongs to the CLI.");
        SectionTitle("CLAUDE LOGIN");
        KeyValue("Credentials file", ai::HasCredentialsFile() ? "present" : "absent");
        KeyValue("Subscription", ai::SubscriptionType().empty() ? "-" : ai::SubscriptionType());
        if (!m_authStatus.empty())
            RenderMono(m_authStatus, m_fonts, ImGui::GetContentRegionAvail().x);
        SectionTitle("ANTHROPIC API KEY (Windows Credential Manager)");
        const bool stored = !StoredApiKey().empty();
        ImGui::TextColored(stored ? kGreen : kDim, stored ? "Stored (passed to the CLI and to the model list)" : "Not stored");
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##key", &m_apiKeyInput, ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ImGui::Button(m_apiKeyInput.empty() ? "Remove" : "Save"))
        {
            StoreApiKey(m_apiKeyInput);
            SetEnvironmentVariableA("ANTHROPIC_API_KEY", m_apiKeyInput.empty() ? nullptr : m_apiKeyInput.c_str());
            m_apiKeyInput.clear();
            ApplyOptions();
        }
    }
    ImGui::End();
}

void AiPanels::DrawSettingsModal()
{
    if (!m_showSettings)
        return;
    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("AI settings", &m_showSettings))
    {
        bool changed = false;
        ImGui::TextUnformatted("Claude Code executable (empty = PATH, then ~/.local/bin)");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputText("##claude", &m_settings.claudePath);
        ImGui::TextUnformatted("Workspace: the folder of the open model (the agent's working directory; saved contexts live there)");
        ImGui::TextColored(kBlue, "%s", m_settings.workspace.c_str());
        static const char* languages[] = {"pt", "en", "es"};
        static const char* languageLabels[] = {"Portuguese (pt-BR)", "English", "Spanish"};
        int language = 0;
        for (int i = 0; i < 3; ++i)
            if (m_settings.askLanguage == languages[i])
                language = i;
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Language of the agent's questions", &language, languageLabels, 3))
            m_settings.askLanguage = languages[language], changed = true;
        ImGui::TextUnformatted("Quiet directive (start of the injected prompt; empty injects nothing)");
        changed |= ImGui::InputTextMultiline("##quiet", &m_settings.quietPrompt, ImVec2(-1, 70));
        ImGui::TextUnformatted("Extra system prompt");
        changed |= ImGui::InputTextMultiline("##system", &m_settings.systemPrompt, ImVec2(-1, 110));
        changed |= ImGui::Checkbox("Show thinking", &m_settings.showThinking);
        ImGui::SetNextItemWidth(220);
        static const char* verbosity[] = {"Verbose", "Necessary", "Dialogue", "Quiet"};
        changed |= ImGui::Combo("Timeline verbosity (display only)", &m_settings.verbosity, verbosity, 4);
        if (changed)
        {
            m_settings.Save();
            ApplyOptions();

        }
        ImGui::Separator();
        ImGui::TextColored(kDim, "Settings: %s", Narrow((AppDataDir() / "ai-settings.json").wstring()).c_str());
    }
    ImGui::End();
}

// ---- Chat -----------------------------------------------------------------------------------------

bool AiPanels::Visible(const ai::TimelineItem& item) const
{
    if (item.kind == ItemKind::Thinking && (!m_settings.showThinking || m_settings.verbosity >= 1))
        return false;
    if (m_settings.verbosity >= 2 && (item.kind == ItemKind::Tool || item.kind == ItemKind::TurnEnd || item.kind == ItemKind::Compaction))
        return false;
    if (m_settings.verbosity >= 3 && item.kind == ItemKind::Notice && !item.error)
        return false;
    return true;
}

void AiPanels::DrawToolCard(size_t index, const ai::TimelineItem& item, float width)
{
    const ai::ToolCall& tool = item.tool;
    ImGui::PushID(int(index));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Indent(8);
    ImGui::Dummy(ImVec2(1, 2));
    bool& open = m_tab->expanded[index];
    const bool aiad = tool.name.starts_with("mcp__aiad__");
    if (!tool.done)
        Spinner(5, ImGui::GetColorU32(kAccent));
    else
        ImGui::TextColored(tool.isError ? kRed : kGreen, tool.isError ? "\xE2\x9C\x97" : "\xE2\x9C\x93");  // ✗ ✓
    ImGui::SameLine();
    ImGui::PushFont(m_fonts.bold, 0.0f);
    ImGui::TextColored(aiad ? kAccent : kBlue, "%s", ToolLabel(tool.name).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    const std::string summary = ToolSummary(tool.name, tool.input);
    ImGui::PushFont(m_fonts.mono, 0.0f);
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(start.x + width - 70, ImGui::GetCursorScreenPos().y + 40), true);
    ImGui::TextColored(kDim, "%s", summary.substr(0, summary.find('\n')).c_str());
    ImGui::PopClipRect();
    ImGui::PopFont();
    if (tool.done && tool.seconds > 0)
    {
        ImGui::SameLine(width - 60);
        ImGui::TextColored(kDim, "%.1fs", tool.seconds);
    }
    const ImVec2 headerMax(start.x + width, ImGui::GetItemRectMax().y + 2);
    if (ImGui::IsMouseHoveringRect(start, headerMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        open = !open;
    // Snapshot thumbnails always visible: the picture is the point of the card.
    if (!tool.imagesBase64.empty() && m_host.texture)
        for (const std::string& image : tool.imagesBase64)
        {
            int w = 0, h = 0;
            if (const ImTextureID id = m_host.texture(image, w, h); id && w > 0)
            {
                const float tw = std::min(width - 24.0f, open ? float(w) : 360.0f);
                ImGui::Image(ImTextureRef(id), ImVec2(tw, tw * float(h) / float(w)));
                if (ImGui::IsItemClicked())
                    open = !open;
            }
        }
    if (open)
    {
        if (!tool.input.is_null() && !tool.input.empty())
        {
            ImGui::TextColored(kDim, "input");
            RenderMono(tool.name == "Bash" || tool.name == "PowerShell" ? summary : tool.input.dump(2), m_fonts, width - 24);
        }
        if (!tool.resultText.empty())
        {
            ImGui::TextColored(tool.isError ? kRed : kDim, tool.isError ? "error" : "result");
            const std::string shown = tool.resultText.size() > 6000 ? tool.resultText.substr(0, 6000) + "\n... (truncated)" : tool.resultText;
            if (aiad)
                RenderMarkdown(shown, m_fonts, width - 24);
            else
                RenderMono(shown, m_fonts, width - 24);
        }
    }
    else if (tool.isError && !tool.resultText.empty())
        ImGui::TextColored(kRed, "%s", tool.resultText.substr(0, std::min<size_t>(200, tool.resultText.find('\n'))).c_str());
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::Unindent(8);
    const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(start, end, IM_COL32(24, 28, 36, 255), 5.0f);
    draw->AddRectFilled(start, ImVec2(start.x + 3, end.y), ImGui::GetColorU32(tool.isError ? kRed : aiad ? kAccent : kBlue), 2.0f);
    draw->ChannelsMerge();
    ImGui::PopID();
}

void AiPanels::DrawTimelineItem(size_t index, const ai::TimelineItem& item, float width)
{
    switch (item.kind)
    {
    case ItemKind::User:
    {
        if (item.text.starts_with("keep-alive:"))
        {
            ImGui::TextColored(kDim, "- cache keep-alive -");
            break;
        }
        ImGui::PushID(int(index));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::Indent(10);
        ImGui::Dummy(ImVec2(1, 3));
        ImGui::TextColored(kAccent, "You");
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%s%s", LocalTime(item.time, "%H:%M").c_str(), item.images ? std::format("  - {} image(s)", item.images).c_str() : "");
        ImGui::SameLine(width - 74);
        if (ImGui::SmallButton("Rewind"))
            Rewind(index);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Go back to before this prompt: the conversation is truncated here and the model changes made since are undone.");
        RenderMarkdown(item.text, m_fonts, width - 24);
        ImGui::Dummy(ImVec2(1, 3));
        ImGui::Unindent(10);
        const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(start, end, IM_COL32(34, 44, 62, 255), 6.0f);
        draw->AddRect(start, end, IM_COL32(70, 100, 150, 160), 6.0f);
        draw->ChannelsMerge();
        ImGui::PopID();
        break;
    }
    case ItemKind::Assistant:
        ImGui::PushID(int(index));
        RenderMarkdown(item.text, m_fonts, width);
        if (item.streaming)
            Spinner(4, ImGui::GetColorU32(kAccent));
        else if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()))
        {
            if (ImGui::SmallButton("Copy"))
                ImGui::SetClipboardText(item.text.c_str());
        }
        ImGui::PopID();
        break;
    case ItemKind::Thinking:
    {
        ImGui::PushID(int(index));
        bool& open = m_tab->expanded[index];
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (ImGui::Selectable(open ? "v Thinking" : "> Thinking", false))
            open = !open;
        if (open)
        {
            ImGui::Indent(14);
            RenderMarkdown(item.text, m_fonts, width - 14);
            ImGui::Unindent(14);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
        break;
    }
    case ItemKind::Tool:
        DrawToolCard(index, item, width);
        break;
    case ItemKind::Notice:
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Indent(10);
        ImGui::PushStyleColor(ImGuiCol_Text, item.error ? kRed : kAmber);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 14);
        ImGui::TextUnformatted(item.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(10);
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3, ImGui::GetCursorScreenPos().y - 2), ImGui::GetColorU32(item.error ? kRed : kAmber));
        break;
    }
    case ItemKind::Compaction:
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight() + 6;
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(25, 60, 110, 255), 4.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 3));
        if (item.preTokens > 0)
            ImGui::Text("Context compacted: %s -> %s tokens (-%s)%s", Compact(double(item.preTokens)).c_str(), Compact(double(item.postTokens)).c_str(),
                        Compact(double(item.preTokens - item.postTokens)).c_str(), item.text.empty() ? "" : (" - " + item.text).c_str());
        else
            ImGui::TextUnformatted("Context compacted");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        ImGui::Dummy(ImVec2(width, 2));
        break;
    }
    case ItemKind::TurnEnd:
        ImGui::TextColored(kDim, "done in %s%s", Duration(item.seconds).c_str(), item.costUsd > 0 ? std::format("  -  ${:.3f}", item.costUsd).c_str() : "");
        ImGui::Separator();
        break;
    }
}

void AiPanels::Rewind(size_t item)
{
    const auto& timeline = m_session->Timeline();
    if (item >= timeline.size() || timeline[item].kind != ItemKind::User)
        return;
    int prompt = 0;
    for (size_t i = 0; i < item; ++i)
        prompt += timeline[i].kind == ItemKind::User;
    const std::string text = timeline[item].text;
    const size_t depth = timeline[item].undoDepth;
    const std::string id = m_session->SessionId();
    m_session->Stop();
    if (m_host.bus && depth != size_t(-1))
        m_host.bus->UndoTo(depth);
    if (!id.empty())
        ai::TruncateTranscript(Widen(m_settings.workspace), id, prompt);
    m_session->TruncateTimeline(item);
    m_tab->prompt = text;
    m_tab->focusComposer = true;
    m_tab->expanded.clear();
    m_tab->heights.clear();
    RefreshTranscripts();
}

void AiPanels::SendPrompt()
{
    std::string text = m_tab->prompt;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    if (text.empty() && m_tab->attachments.empty())
        return;
    const std::string part = m_host.selectedPart ? m_host.selectedPart() : std::string();
    if (m_tab->includeSelection && !part.empty() && !text.starts_with("/"))
        text = std::format("[Selected in the viewport: {}]\n{}", part, text);
    m_session->Send(text, m_tab->attachments, m_host.bus ? m_host.bus->Depth() : 0);
    m_tab->prompt.clear();
    m_tab->attachments.clear();
    m_tab->scrollToBottom = true;
}

void AiPanels::DrawComposer(float width)
{
    // Attachments and the selection chip.
    for (size_t i = 0; i < m_tab->attachments.size(); ++i)
    {
        ImGui::PushID(int(i));
        if (Pill(std::format("image {}  x", i + 1).c_str()))
        {
            m_tab->attachments.erase(m_tab->attachments.begin() + long(i));
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    const std::string part = m_host.selectedPart ? m_host.selectedPart() : std::string();
    if (!part.empty())
    {
        ImGui::Checkbox(("@ " + part).c_str(), &m_tab->includeSelection);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tell the agent which part is selected in the viewport.");
        ImGui::SameLine();
    }
    if (Pill("+ Viewport image") && m_host.viewportPng)
    {
        const std::vector<uint8_t> png = m_host.viewportPng();
        if (!png.empty())
            m_tab->attachments.push_back({"image/png", Base64Encode(png.data(), png.size())});
    }
    ImGui::NewLine();

    // Slash commands and @-parts menus.
    std::vector<std::string> menu;
    std::string token;
    const size_t at = m_tab->prompt.find_last_of(" \n");
    const std::string last = m_tab->prompt.substr(at == std::string::npos ? 0 : at + 1);
    const bool slash = m_tab->prompt.starts_with('/') && m_tab->prompt.find(' ') == std::string::npos;
    if (slash)
    {
        for (const std::string& c : m_session->SlashCommands())
            if (("/" + c).starts_with(m_tab->prompt))
                menu.push_back("/" + c);
    }
    else if (last.starts_with('@') && m_host.partNames)
    {
        std::string key = last.substr(1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (const std::string& p : m_host.partNames())
        {
            std::string lower = p;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find(key) != std::string::npos)
                menu.push_back("@" + p);
        }
    }
    if (menu.size() > 12)
        menu.resize(12);
    m_tab->menuIndex = menu.empty() ? 0 : std::clamp(m_tab->menuIndex, 0, int(menu.size()) - 1);
    if (!menu.empty())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            m_tab->menuIndex = (m_tab->menuIndex + 1) % int(menu.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            m_tab->menuIndex = (m_tab->menuIndex + int(menu.size()) - 1) % int(menu.size());
    }

    struct CursorState
    {
        int cursor = 0;
        int setCursor = -1;
    };
    static CursorState state;
    auto callback = [](ImGuiInputTextCallbackData* data) {
        auto* s = static_cast<CursorState*>(data->UserData);
        if (s->setCursor >= 0)
        {
            data->CursorPos = data->SelectionStart = data->SelectionEnd = std::min(s->setCursor, data->BufTextLen);
            s->setCursor = -1;
        }
        s->cursor = data->CursorPos;
        return 0;
    };
    if (m_tab->focusComposer)
    {
        ImGui::SetKeyboardFocusHere();
        m_tab->focusComposer = false;
        state.setCursor = int(m_tab->prompt.size());
    }
    const float lines = std::clamp(float(std::count(m_tab->prompt.begin(), m_tab->prompt.end(), '\n') + 1), 2.0f, 10.0f);
    const bool busy = m_session->Busy();
    const float buttonWidth = 96;
    const bool enter = ImGui::InputTextMultiline("##prompt", &m_tab->prompt, ImVec2(width - buttonWidth - 8, ImGui::GetTextLineHeight() * lines + 10),
                                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                                     ImGuiInputTextFlags_CallbackAlways,
                                                 callback, &state);
    const bool composerFocused = ImGui::IsItemActive();
    const ImVec2 inputMin = ImGui::GetItemRectMin();
    if (m_tab->prompt.empty() && !composerFocused)
        ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + 6, ImGui::GetItemRectMin().y + 5), ImGui::GetColorU32(kDim),
                                            "Ask the agent...  (Enter sends, Shift+Enter new line, / commands, @ parts, Ctrl+V image)");
    if (ImGui::IsItemHovered() && m_tab->prompt.empty())
        ImGui::SetTooltip("Enter sends, Shift+Enter new line, / commands, @ parts, Ctrl+V pastes an image");
    if (composerFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
    {
        const std::vector<uint8_t> png = ClipboardPng();
        if (!png.empty())
            m_tab->attachments.push_back({"image/png", Base64Encode(png.data(), png.size())});
    }
    if (enter)
    {
        if (!menu.empty())
        {
            const std::string choice = menu[size_t(m_tab->menuIndex)];
            m_tab->prompt = slash ? choice + " " : m_tab->prompt.substr(0, at == std::string::npos ? 0 : at + 1) + choice + " ";
            m_tab->focusComposer = true;
        }
        else if (ImGui::GetIO().KeyShift)
        {
            m_tab->prompt.insert(size_t(std::min<int>(state.cursor, int(m_tab->prompt.size()))), "\n");
            state.setCursor = state.cursor + 1;
            m_tab->focusComposer = true;
        }
        else
        {
            // While the agent works, the prompt is queued by the CLI and answered next.
            SendPrompt();
            m_tab->focusComposer = true;
        }
    }
    // Send is always there (while busy it queues the prompt); Stop joins it while the agent works.
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.38f, 0.08f, 1));
    if (ImGui::Button(busy ? "Send (queue)" : "Send", ImVec2(buttonWidth, 0)))
    {
        SendPrompt();
        m_tab->focusComposer = true;
    }
    ImGui::PopStyleColor();
    if (busy && ImGui::IsItemHovered())
        ImGui::SetTooltip("The agent is working: this prompt is queued and answered right after the current one.");
    if (busy)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.12f, 1));
        if (ImGui::Button("Stop", ImVec2(buttonWidth, 0)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_session->Pending().size()))
            m_session->Interrupt();
        ImGui::PopStyleColor();
        if (m_session->QueuedPrompts() > 0)
            ImGui::TextColored(kDim, "%d queued", m_session->QueuedPrompts());
    }
    ImGui::EndGroup();
    if (!menu.empty())
    {
        ImGui::SetNextWindowPos(inputMin, ImGuiCond_Always, ImVec2(0, 1));
        ImGui::BeginTooltip();
        for (int i = 0; i < int(menu.size()); ++i)
            ImGui::Selectable(menu[size_t(i)].c_str(), i == m_tab->menuIndex);
        ImGui::TextDisabled("Up/Down, Enter to pick");
        ImGui::EndTooltip();
    }
}

void AiPanels::DrawRequests()
{
    if (m_session->Pending().empty())
        return;
    const ai::PendingRequest request = m_session->Pending().front();
    const char* title = request.kind == ai::RequestKind::Question ? "Question from the agent###request"
                        : request.kind == ai::RequestKind::Plan  ? "Plan for approval###request"
                                                                 : "Permission###request";
    ImGui::OpenPopup("###request");
    const ImVec2 size(std::min(720.0f, ImGui::GetMainViewport()->Size.x * 0.6f), 0);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_None))
        return;
    const float width = ImGui::GetContentRegionAvail().x;
    if (request.kind == ai::RequestKind::Permission)
    {
        ImGui::PushFont(m_fonts.bold, m_fonts.size * 1.1f);
        ImGui::TextColored(request.tool.starts_with("mcp__aiad__") ? kAccent : kBlue, "%s", ToolLabel(request.tool).c_str());
        ImGui::PopFont();
        if (!request.description.empty())
            ImGui::TextWrapped("%s", request.description.c_str());
        const std::string preview = request.tool == "Bash" || request.tool == "PowerShell" ? ToolSummary(request.tool, request.input) : request.input.dump(2);
        // Invisible characters exposed (zero-width, bidi overrides, control characters).
        bool suspicious = false;
        for (size_t i = 0; i < preview.size(); ++i)
        {
            const uint8_t c = uint8_t(preview[i]);
            if ((c < 0x20 && c != '\n' && c != '\t' && c != '\r') ||
                (c == 0xE2 && i + 2 < preview.size() && uint8_t(preview[i + 1]) == 0x80 &&
                 ((uint8_t(preview[i + 2]) >= 0x8B && uint8_t(preview[i + 2]) <= 0x8F) || (uint8_t(preview[i + 2]) >= 0xAA && uint8_t(preview[i + 2]) <= 0xAE))))
                suspicious = true;
        }
        if (suspicious)
            ImGui::TextColored(kRed, "Warning: the input contains invisible or control characters.");
        ImGui::BeginChild("##preview", ImVec2(0, std::min(320.0f, ImGui::CalcTextSize(preview.c_str(), nullptr, false, width).y + 20)), ImGuiChildFlags_Borders);
        RenderMono(preview, m_fonts, width - 20);
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##deny", "Reason, if you deny (sent to the agent)", &m_tab->denyNote);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.25f, 1));
        const bool allow = ImGui::Button("Allow  (Ctrl+Enter)") || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool always = ImGui::Button("Always allow") ;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Allow and remember the rule the CLI suggested for this session.");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.12f, 1));
        const bool deny = ImGui::Button("Deny  (Esc)") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::PopStyleColor();
        if (allow || always)
            m_session->Allow(request.requestId, always);
        else if (deny)
            m_session->Deny(request.requestId, m_tab->denyNote);
        if (allow || always || deny)
        {
            m_tab->denyNote.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    else if (request.kind == ai::RequestKind::Question)
    {
        auto& picks = m_tab->questionPicks[request.requestId];
        auto& other = m_tab->questionOther[request.requestId];
        auto& notes = m_tab->questionNotes[request.requestId];
        picks.resize(request.questions.size());
        other.resize(request.questions.size());
        notes.resize(request.questions.size());
        bool complete = true;
        if (ImGui::BeginTabBar("##questions"))
        {
            for (size_t q = 0; q < request.questions.size(); ++q)
            {
                const ai::Question& question = request.questions[q];
                picks[q].resize(question.options.size() + 1);
                bool any = std::find(picks[q].begin(), picks[q].end(), true) != picks[q].end() || !notes[q].empty();
                complete &= any;
                ImGui::PushID(int(q));
                if (ImGui::BeginTabItem((question.header.empty() ? std::format("Q{}", q + 1) : question.header).c_str()))
                {
                    ImGui::PushFont(m_fonts.bold, 0.0f);
                    ImGui::TextWrapped("%s", question.question.c_str());
                    ImGui::PopFont();
                    ImGui::TextColored(kDim, question.multiSelect ? "Pick one or more" : "Pick one");
                    for (size_t o = 0; o <= question.options.size(); ++o)
                    {
                        const bool isOther = o == question.options.size();
                        const std::string label = isOther ? std::string("Other") : question.options[o].label;
                        ImGui::PushID(int(o));
                        bool picked = picks[q][o];
                        if (question.multiSelect ? ImGui::Checkbox(label.c_str(), &picked) : ImGui::RadioButton(label.c_str(), picked))
                        {
                            if (!question.multiSelect)
                                std::fill(picks[q].begin(), picks[q].end(), false);
                            picks[q][o] = question.multiSelect ? picked : true;
                        }
                        if (!isOther && !question.options[o].description.empty())
                        {
                            ImGui::Indent(28);
                            ImGui::TextColored(kDim, "%s", question.options[o].description.c_str());
                            ImGui::Unindent(28);
                        }
                        if (isOther && picks[q][o])
                        {
                            ImGui::SetNextItemWidth(-1);
                            ImGui::InputText("##other", &other[q]);
                        }
                        ImGui::PopID();
                    }
                    ImGui::TextUnformatted("Add a note to this answer (optional)");
                    ImGui::InputTextMultiline("##note", &notes[q], ImVec2(-1, 54));
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
            }
            ImGui::EndTabBar();
        }
        ImGui::BeginDisabled(!complete);
        const bool send = ImGui::Button("Send answers");
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Decline") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (send)
        {
            std::map<std::string, std::string> answers;
            for (size_t q = 0; q < request.questions.size(); ++q)
            {
                std::string choices;
                for (size_t o = 0; o < request.questions[q].options.size(); ++o)
                    if (picks[q][o])
                        choices += (choices.empty() ? "" : ", ") + request.questions[q].options[o].label;
                if (picks[q].back() && !other[q].empty())
                    choices += (choices.empty() ? "" : ", ") + other[q];
                std::string note = notes[q];
                answers[request.questions[q].question] = note.empty() ? choices : choices.empty() ? note : choices + "\n" + note;
            }
            m_session->Answer(request.requestId, answers);
            ImGui::CloseCurrentPopup();
        }
        else if (cancel)
        {
            m_session->Deny(request.requestId, "The user declined to answer.");
            ImGui::CloseCurrentPopup();
        }
    }
    else
    {
        if (m_tab->planRequest != request.requestId)
        {
            m_tab->planRequest = request.requestId;
            m_tab->planEdit = request.plan;
            m_tab->planEditing = false;
        }
        if (ImGui::RadioButton("Preview", !m_tab->planEditing))
            m_tab->planEditing = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Edit", m_tab->planEditing))
            m_tab->planEditing = true;
        const float h = std::min(ImGui::GetMainViewport()->Size.y * 0.6f, 520.0f);
        if (m_tab->planEditing)
            ImGui::InputTextMultiline("##plan", &m_tab->planEdit, ImVec2(-1, h));
        else
        {
            ImGui::BeginChild("##planview", ImVec2(0, h), ImGuiChildFlags_Borders);
            RenderMarkdown(m_tab->planEdit, m_fonts, ImGui::GetContentRegionAvail().x);
            ImGui::EndChild();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.25f, 1));
        const bool approve = ImGui::Button("Approve & run");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool keep = ImGui::Button("Keep planning (send my notes)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sends your edited plan back as feedback and keeps the agent in plan mode.");
        if (approve)
        {
            m_session->ApprovePlan(request.requestId, m_tab->planEdit);
            ImGui::CloseCurrentPopup();
        }
        else if (keep)
        {
            m_session->Deny(request.requestId, m_tab->planEdit == request.plan ? std::string("Keep planning.") : "Revise the plan. My edited version:\n\n" + m_tab->planEdit);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void AiPanels::PrepareDocking()
{
    // A new conversation docks beside the other conversations, or the first one splits the viewport
    // area (chat between the 3D view and the hub). Done before any window of the frame is submitted.
    ImGuiID target = 0;
    for (auto& tab : m_tabs)
        if (tab->docked)
            if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr(("###chat" + std::to_string(tab->id)).c_str())); w && w->DockId)
                target = w->DockId;
    for (auto& tab : m_tabs)
    {
        if (tab->docked)
            continue;
        if (!target)
            if (ImGuiWindow* viewport = ImGui::FindWindowByName("Viewport"); viewport && viewport->DockId)
            {
                ImGuiID rest = 0;
                target = ImGui::DockBuilderSplitNode(viewport->DockId, ImGuiDir_Right, 0.42f, nullptr, &rest);
            }
        if (target)
            ImGui::DockBuilderDockWindow(("###chat" + std::to_string(tab->id)).c_str(), target);
        tab->docked = true;
    }
}

void AiPanels::DrawChats()
{
    ChatTab* active = m_tab;
    for (auto& tab : m_tabs)
    {
        Activate(tab.get());
        ImGui::PushID(tab->id);  // each conversation's modals have their own ids
        DrawChat(*tab);
        ImGui::PopID();
        if (tab->focused)
            active = tab.get();
    }
    // Closed tabs: their CLI stops; the conversation stays saved (reopen it from the hub).
    for (auto& tab : m_tabs)
        if (!tab->open)
            tab->session->Stop();
    std::erase_if(m_tabs, [&](const std::unique_ptr<ChatTab>& tab) {
        if (tab->open)
            return false;
        if (active == tab.get())
            active = nullptr;
        return true;
    });
    if (!active && !m_tabs.empty())
        active = m_tabs.back().get();
    Activate(active);
}

void AiPanels::DrawChat(ChatTab& tab)
{
    std::string title = "New session";
    for (const ai::TranscriptInfo& t : m_transcripts)
        if (!m_session->SessionId().empty() && t.id == m_session->SessionId() && !t.title.empty())
            title = t.title;
    if (title.size() > 40)
        title = ai::ShortTitle(title, 40);
    if (m_session->Busy())
        title = "* " + title;  // working
    if (tab.focus)
    {
        ImGui::SetNextWindowFocus();
        tab.focus = false;
    }
    const bool visible = ImGui::Begin(tab.WindowName(title).c_str(), &tab.open);
    tab.focused = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible)
    {
        ImGui::End();
        DrawRequests();  // a question from a background conversation still reaches the user
        return;
    }
    // Toolbar: model, verbosity, search, export.
    if (!m_session->ActiveModel().empty())
    {
        ImGui::TextColored(kDim, "%s", m_session->ActiveModel().c_str());
        ImGui::SameLine();
    }
    static const char* verbosity[] = {"Verbose", "Necessary", "Dialogue", "Quiet"};
    ImGui::SetNextItemWidth(100);
    if (ImGui::Combo("##verbosity", &m_settings.verbosity, verbosity, 4))
        m_settings.Save();
    ImGui::SameLine();
    if (ImGui::SmallButton("Find") || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)))
        m_tab->searchOpen = !m_tab->searchOpen;
    ImGui::SameLine();
    if (ImGui::SmallButton("HTML"))
        ExportHtml();
    ImGui::SameLine();
    if (ImGui::SmallButton("MD"))
        ExportMarkdown();
    if (m_tab->searchOpen)
    {
        ImGui::SetNextItemWidth(-150);
        if (ImGui::InputTextWithHint("##search", "Find in conversation", &m_tab->search, ImGuiInputTextFlags_EnterReturnsTrue))
            ++m_tab->searchHit;
        ImGui::SameLine();
        int hits = 0;
        for (const auto& item : m_session->Timeline())
            hits += !m_tab->search.empty() && (item.text.find(m_tab->search) != std::string::npos || item.tool.resultText.find(m_tab->search) != std::string::npos);
        ImGui::TextColored(kDim, "%d match%s", hits, hits == 1 ? "" : "es");
    }
    if (!m_tab->status.empty())
        ImGui::TextColored(kDim, "%s", m_tab->status.c_str());
    ImGui::Separator();

    // Timeline (items off screen are skipped with their last measured height).
    const float composerHeight = ImGui::GetTextLineHeight() * 5.5f + 50;
    ImGui::BeginChild("##timeline", ImVec2(0, -composerHeight), ImGuiChildFlags_None);
    const float width = ImGui::GetContentRegionAvail().x - 6;
    const auto& timeline = m_session->Timeline();
    const float scrollY = ImGui::GetScrollY(), viewHeight = ImGui::GetWindowHeight();
    int hit = 0, wanted = -1;
    for (size_t i = 0; i < timeline.size(); ++i)
    {
        const ai::TimelineItem& item = timeline[i];
        if (!Visible(item))
            continue;
        const bool match = !m_tab->search.empty() && (item.text.find(m_tab->search) != std::string::npos || item.tool.resultText.find(m_tab->search) != std::string::npos);
        const float y = ImGui::GetCursorPosY();
        auto cached = m_tab->heights.find(i);
        const bool streaming = item.streaming || (item.kind == ItemKind::Tool && !item.tool.done) || i + 2 >= timeline.size();
        if (cached != m_tab->heights.end() && !streaming && !match && (y + cached->second < scrollY - 200 || y > scrollY + viewHeight + 200))
        {
            ImGui::Dummy(ImVec2(width, cached->second - ImGui::GetStyle().ItemSpacing.y));
            continue;
        }
        if (match)
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y), ImVec2(p.x - 1, p.y + ImGui::GetTextLineHeight() * 2), ImGui::GetColorU32(kAmber));
            if (m_tab->searchHit >= 0 && hit == m_tab->searchHit)
                wanted = int(i);
            ++hit;
        }
        DrawTimelineItem(i, item, width);
        m_tab->heights[i] = ImGui::GetCursorPosY() - y;
        if (wanted == int(i))
            ImGui::SetScrollHereY(0.2f);
    }
    if (m_tab->searchHit >= hit && hit > 0)
        m_tab->searchHit = 0;
    if (m_session->Busy())
    {
        Spinner(6, ImGui::GetColorU32(kAccent));
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%s %s", m_session->Compacting() ? "Compacting the context..." : "Working...", Duration(m_session->TurnSeconds()).c_str());
    }
    if (timeline.empty())
    {
        ImGui::Dummy(ImVec2(1, 30));
        ImGui::TextColored(kDim, "Ask the agent to inspect or change the engine, e.g.:");
        ImGui::BulletText("What limits the power of this engine? Check it with a simulation.");
        ImGui::BulletText("Increase the bore to 84 mm and show me the result.");
        ImGui::BulletText("Hide the head cover and take a snapshot.");
    }
    if (m_tab->scrollToBottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 80)
        ImGui::SetScrollHereY(1.0f);
    m_tab->scrollToBottom = false;
    ImGui::EndChild();
    ImGui::Separator();
    DrawComposer(ImGui::GetContentRegionAvail().x);
    ImGui::End();
    DrawRequests();
}

// ---- Export ---------------------------------------------------------------------------------------

void AiPanels::ExportMarkdown()
{
    std::string md = "# Tootega AIAD - conversation\n\n";
    for (const auto& item : m_session->Timeline())
        switch (item.kind)
        {
        case ItemKind::User:
            md += "## You\n\n" + item.text + "\n\n";
            break;
        case ItemKind::Assistant:
            md += item.text + "\n\n";
            break;
        case ItemKind::Tool:
            md += "> **" + ToolLabel(item.tool.name) + "** `" + ToolSummary(item.tool.name, item.tool.input) + "`" + (item.tool.isError ? " (error)" : "") + "\n\n";
            break;
        case ItemKind::Notice:
            md += "> " + item.text + "\n\n";
            break;
        default:
            break;
        }
    const std::filesystem::path file =
        std::filesystem::path(Widen(m_settings.workspace)) / ".aiad" / "TimeLine" / Widen(std::format("{}-{}.md", m_session->SessionId().empty() ? "session" : m_session->SessionId(), LocalTime(system_clock::now(), "%Y%m%d-%H%M%S")));
    if (WriteTextFile(file, md))
    {
        m_tab->status = "Exported " + Narrow(file.wstring());
        ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void AiPanels::ExportHtml()
{
    std::string html = "<!doctype html><html><head><meta charset='utf-8'><title>Tootega AIAD conversation</title><style>"
                       "body{font-family:Segoe UI,sans-serif;background:#1c1f26;color:#e6e6e6;max-width:980px;margin:24px auto}"
                       ".you{background:#22304a;border:1px solid #46649a;border-radius:8px;padding:8px 12px;margin:14px 0}"
                       ".msg{white-space:pre-wrap;margin:8px 0}.tool{background:#181c24;border-left:3px solid #4d9fff;border-radius:5px;padding:6px 10px;margin:6px 0}"
                       ".aiad{border-left-color:#ff8c26}.err{border-left-color:#f25c52}details pre{white-space:pre-wrap;color:#b8c0cc}"
                       "img{max-width:100%;border-radius:4px;margin-top:6px}.note{color:#f5be40}</style></head><body><h2>Tootega AIAD - conversation</h2>";
    for (const auto& item : m_session->Timeline())
        switch (item.kind)
        {
        case ItemKind::User:
            html += "<div class='you'><b>You</b><div class='msg'>" + HtmlEscape(item.text) + "</div></div>";
            break;
        case ItemKind::Assistant:
            html += "<div class='msg'>" + HtmlEscape(item.text) + "</div>";
            break;
        case ItemKind::Tool:
        {
            const ai::ToolCall& t = item.tool;
            html += std::string("<details class='tool") + (t.name.starts_with("mcp__aiad__") ? " aiad" : "") + (t.isError ? " err" : "") + "'><summary><b>" +
                    HtmlEscape(ToolLabel(t.name)) + "</b> " + HtmlEscape(ToolSummary(t.name, t.input)) + "</summary><pre>" + HtmlEscape(t.input.dump(2)) +
                    "</pre><pre>" + HtmlEscape(t.resultText) + "</pre>";
            for (const std::string& image : t.imagesBase64)
                html += "<img src='data:image/png;base64," + image + "'>";
            html += "</details>";
            break;
        }
        case ItemKind::Notice:
            html += "<div class='note'>" + HtmlEscape(item.text) + "</div>";
            break;
        default:
            break;
        }
    html += "</body></html>";
    const std::filesystem::path file =
        std::filesystem::path(Widen(m_settings.workspace)) / ".aiad" / "TimeLine" / Widen(std::format("{}-{}.html", m_session->SessionId().empty() ? "session" : m_session->SessionId(), LocalTime(system_clock::now(), "%Y%m%d-%H%M%S")));
    if (WriteTextFile(file, html))
    {
        m_tab->status = "Exported " + Narrow(file.wstring());
        ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void AiPanels::AutoSaveTimeline()
{
    // Like the Cockpit's .cockpit/TimeLine: the readable record of every turn, per session.
    if (m_session->SessionId().empty())
        return;
    std::string md;
    for (const auto& item : m_session->Timeline())
        if (item.kind == ItemKind::User)
            md += "## You (" + LocalTime(item.time, "%Y-%m-%d %H:%M") + ")\n\n" + item.text + "\n\n";
        else if (item.kind == ItemKind::Assistant)
            md += item.text + "\n\n";
        else if (item.kind == ItemKind::Tool)
            md += "> " + ToolLabel(item.tool.name) + " `" + ToolSummary(item.tool.name, item.tool.input) + "`\n\n";
    WriteTextFile(std::filesystem::path(Widen(m_settings.workspace)) / ".aiad" / "TimeLine" / Widen(m_session->SessionId() + ".md"), md);
}

}  // namespace aiad::app
