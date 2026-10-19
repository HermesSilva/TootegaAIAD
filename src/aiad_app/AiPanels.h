// The AI side of the app, modelled on the Tootega Cockpit (VS Code GUI for Claude Code):
//  - "AI Hub": header with versions, account buttons (Usage, Sign in/out, Plugins, MCP, Skills,
//    Credentials, Settings), context window, cache life + keep-alive, tokens, cache, cost, session
//    activity, model / effort / permission, saved contexts;
//  - "AI Chat": streaming timeline (Markdown, thinking, tool cards with snapshot thumbnails), composer
//    (Enter sends, Shift+Enter new line, slash and @-part menus, pasted images, selection chip), search,
//    export, rewind (conversation and model), permission / question / plan modals.
// The agent reaches the model only through MCP -> bridge -> Command Bus, like any other operator.
#pragma once

#include "AgentSession.h"
#include "Account.h"
#include "CommandBus.h"
#include "Markdown.h"
#include "Transcripts.h"

#include <imgui.h>

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aiad::app
{

struct AiSettings
{
    std::string claudePath;
    std::string workspace;            // the agent's working folder: the open model's (not saved)
    std::string model;                // empty = CLI default
    std::string effort;               // empty = CLI default
    std::string permissionMode = "default";
    std::string askLanguage = "pt";
    std::string quietPrompt;
    std::string systemPrompt;
    bool agentsAndWorkflows = false;  // off = Task/Workflow disallowed (they spend a lot of tokens)
    bool keepCacheAlive = false;
    bool showThinking = true;
    int verbosity = 0;                // 0 verbose, 1 necessary, 2 dialogue, 3 quiet

    static AiSettings Load();
    void Save() const;
};

// What the panels need from the app (no geometry: names and images only).
struct AiHost
{
    cmd::CommandBus* bus = nullptr;
    std::string workspace;            // folder of the open model: the agent's working directory
    std::string bridgeUrl, bridgeToken;
    std::filesystem::path mcpExe;
    // Name of the selected part (empty = none) and the part names for @-mentions.
    std::function<std::string()> selectedPart;
    std::function<std::vector<std::string>()> partNames;
    // PNG of the viewport (attach to a prompt).
    std::function<std::vector<uint8_t>()> viewportPng;
    // Texture for a PNG (base64), uploaded by the app; null while not ready.
    std::function<ImTextureID(const std::string& base64Png, int& width, int& height)> texture;
};

// One conversation: its agent session and the UI state of its tab.
struct ChatTab
{
    int id = 0;
    std::unique_ptr<ai::AgentSession> session;
    bool open = true, docked = false, focus = true, focused = false;
    bool placeFloating = false;   // opens where the last conversation window floated
    std::string prompt;
    std::vector<std::pair<std::string, std::string>> attachments;  // mime, base64
    bool includeSelection = true;
    int menuIndex = 0;
    bool focusComposer = true;
    std::string search;
    bool searchOpen = false;
    int searchHit = -1;
    bool scrollToBottom = true;
    std::map<size_t, bool> expanded;
    std::map<size_t, float> heights;
    std::string planEdit, planRequest, denyNote;
    bool planEditing = false;
    std::map<std::string, std::vector<std::vector<bool>>> questionPicks;
    std::map<std::string, std::vector<std::string>> questionOther, questionNotes;
    std::string status;
    double lastKeepAlive = 0.0;
    uint64_t seenRevision = 0;
    std::string WindowName(const std::string& title) const { return title + "###chat" + std::to_string(id); }
};

class AiPanels
{
public:
    AiPanels(AiHost host, UiFonts fonts);
    ~AiPanels();

    void Pump();          // per frame, before drawing
    // Docks newly opened conversation tabs; call after the dockspace, before any window.
    void PrepareDocking();
    void DrawHub();
    void DrawChats();
    // "+ New session": a new conversation tab with its prompt.
    void OpenNewSession();
    // Fixture timeline (UI test): the active tab gets an offline session with recorded events.
    void LoadFixture(const std::vector<Json>& events, const std::string& prompt);
    ai::AgentSession* Session() { return m_tab ? m_tab->session.get() : nullptr; }
    size_t ChatCount() const { return m_tabs.size(); }
    const std::string& Workspace() const { return m_settings.workspace; }
    // The open document changed: new conversations work in its folder (empty = the default folder).
    // Open conversations keep the folder they started in.
    void SetWorkspace(const std::string& folder);
    // Where the last conversation window was (docked or floating): a new one opens there. Kept by the
    // host between runs (aiadconfig.json).
    Json ChatPlacement() const;
    void SetChatPlacement(const Json& placement);
    // Closes every conversation tab (as their close buttons do; UI test).
    void CloseChats();

private:
    ai::SessionOptions BuildOptions() const;
    // Opens a conversation tab (focusing it when that session is already open).
    void OpenSession(const std::string& resumeId);
    void DrawChat(ChatTab& tab);
    void Activate(ChatTab* tab);
    void ApplyOptions();
    void RefreshTranscripts();
    void SendPrompt();
    void Rewind(size_t item);
    void DrawHeader();
    void DrawMeters();
    void DrawControls();
    void DrawSavedContexts();
    void DrawTimelineItem(size_t index, const ai::TimelineItem& item, float width);
    void DrawToolCard(size_t index, const ai::TimelineItem& item, float width);
    void DrawComposer(float width);
    void DrawRequests();
    void DrawUsageModal();
    void DrawMcpModal();
    void DrawSkillsModal();
    void DrawPluginsModal();
    void DrawCredentialsModal();
    void DrawSettingsModal();
    void ExportHtml();
    void ExportMarkdown();
    void AutoSaveTimeline();
    bool Visible(const ai::TimelineItem& item) const;

    AiHost m_host;
    UiFonts m_fonts;
    AiSettings m_settings;
    std::filesystem::path m_mcpConfig;

    // Background reads.
    std::string m_cliVersion, m_latestVersion;
    std::future<std::string> m_cliVersionFuture, m_latestFuture;
    std::map<std::string, ai::Price> m_prices;
    std::future<std::map<std::string, ai::Price>> m_pricesFuture;
    ai::AccountUsage m_usage;
    std::future<ai::AccountUsage> m_usageFuture;
    double m_usageAt = -1e9;
    std::vector<ai::TranscriptInfo> m_transcripts;
    std::future<std::vector<ai::TranscriptInfo>> m_transcriptsFuture;
    std::vector<ai::ModelInfo> m_models;
    std::future<std::vector<ai::ModelInfo>> m_modelsFuture;
    std::string m_pluginsText;
    Json m_plugins;
    std::future<ai::CapturedRun> m_pluginsFuture;
    Json m_contextUsage;
    std::string m_authStatus;
    std::future<ai::CapturedRun> m_authFuture;

    std::string m_renaming;
    char m_renameBuffer[256] = {};
    std::string m_apiKeyInput;
    bool m_showUsage = false, m_showMcp = false, m_showSkills = false, m_showPlugins = false, m_showCredentials = false, m_showSettings = false;
    bool m_confirmDeleteAll = false;
    std::string m_confirmDelete;

    // Conversations: one dockable tab each (the Cockpit's session tabs); the hub shows the active one.
    struct LastChat
    {
        bool known = false, floating = false;
        ImGuiID dock = 0;
        ImVec2 pos, size;   // screen rectangle of the window
    } m_lastChat;
    ImGuiID ChatDockTarget();
    std::vector<std::unique_ptr<ChatTab>> m_tabs;
    ChatTab* m_tab = nullptr;                  // the tab being drawn, else the active one
    ai::AgentSession* m_session = nullptr;     // m_tab's session, or m_idle
    std::unique_ptr<ai::AgentSession> m_idle;  // hub figures when no conversation is open
    int m_nextTab = 1;
};

}  // namespace aiad::app
