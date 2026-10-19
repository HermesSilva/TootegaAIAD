// Agent session reducer and saved contexts, replaying stream-json shaped as the CLI emits it.
// What failure would this catch?
// - streamed text duplicated by the final `assistant` event, or tool cards losing their input/result;
// - a permission / question / plan answered with a payload the CLI rejects (allow needs updatedInput,
//   answers go inside updatedInput, "always" carries the suggestions);
// - an unknown event breaking the session (the contract is version-tolerant);
// - cost/context figures wrong across turns and CLI restarts;
// - saved contexts: wrong folder encoding, meta lines shown as prompts, rewind cutting at the wrong prompt.
#include "Account.h"
#include "AgentSession.h"
#include "Paths.h"
#include "Transcripts.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>

using namespace aiad;
using namespace aiad::ai;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do                                                                   \
    {                                                                    \
        if (!(c))                                                        \
        {                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static Json J(const char* text)
{
    return Json::parse(text);
}

static void Streaming()
{
    SessionOptions o;
    o.offline = true;
    AgentSession s(o);
    s.Send("Make the bore 82 mm", {}, 3);
    s.Feed(J(R"({"type":"system","subtype":"init","session_id":"abc","model":"claude-opus-5-5[1m]","tools":["Read","mcp__aiad__aiad_engine_get_design"],
                "mcp_servers":[{"name":"aiad","status":"connected"}],"slash_commands":["compact","context"]})"));
    CHECK(s.SessionId() == "abc" && s.Stats().contextLimit == 1000000 && s.SlashCommands().size() == 2);
    s.Feed(J(R"({"type":"stream_event","event":{"type":"message_start","message":{"id":"m1","usage":{"input_tokens":10,"cache_read_input_tokens":5000,"cache_creation_input_tokens":200}}}})"));
    // Live: the figures count before the turn's result arrives (long turns take minutes).
    CHECK(s.Stats().inputTokens == 10 && s.Stats().cacheReadTokens == 5000 && s.Stats().cacheCreateTokens == 200);
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Reading "}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"the design."}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_stop","index":0}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"t1","name":"mcp__aiad__aiad_engine_set_parameters","input":{}}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"parameters\":{\"bo"}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"re\":82}}"}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_stop","index":1}})"));
    // The full message arrives after the stream: it must not duplicate the text nor the tool.
    s.Feed(J(R"({"type":"assistant","message":{"id":"m1","role":"assistant","content":[{"type":"text","text":"Reading the design."},
                {"type":"tool_use","id":"t1","name":"mcp__aiad__aiad_engine_set_parameters","input":{"parameters":{"bore":82}}}]}})"));
    s.Feed(J(R"({"type":"something_new_in_2027","payload":{"x":1}})"));  // ignored
    s.Feed(J(R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1",
                "content":[{"type":"text","text":"Bore set to 82 mm."},{"type":"image","source":{"type":"base64","media_type":"image/png","data":"iVBORw0KGgo="}}]}]}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"message_start","message":{"id":"m2"}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Done."}}})"));
    s.Feed(J(R"({"type":"stream_event","event":{"type":"message_stop"}})"));
    s.Feed(J(R"({"type":"assistant","message":{"id":"m2","role":"assistant","content":[{"type":"text","text":"Done."}]}})"));
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":0.25,"num_turns":2,
                "usage":{"input_tokens":20,"output_tokens":300,"cache_read_input_tokens":9000,"cache_creation_input_tokens":400}})"));

    const auto& t = s.Timeline();
    int user = 0, assistant = 0, tools = 0, ends = 0;
    for (const TimelineItem& i : t)
    {
        user += i.kind == ItemKind::User;
        assistant += i.kind == ItemKind::Assistant;
        tools += i.kind == ItemKind::Tool;
        ends += i.kind == ItemKind::TurnEnd;
    }
    CHECK(user == 1 && assistant == 2 && tools == 1 && ends == 1);
    CHECK(t[0].undoDepth == 3);
    CHECK(t[1].text == "Reading the design." && !t[1].streaming);
    CHECK(t[2].tool.input["parameters"]["bore"] == 82 && t[2].tool.done && t[2].tool.resultText == "Bore set to 82 mm." && t[2].tool.imagesBase64.size() == 1);
    CHECK(t[3].text == "Done.");
    CHECK(!s.Busy());
    CHECK(s.Stats().costUsd == 0.25 && s.Stats().turns == 1 && s.Stats().cacheReadTokens == 9000 && s.Stats().outputTokens == 300);
    CHECK(s.Stats().contextUsed == 5210 && s.Stats().peakContext == 5210);
    CHECK(s.Stats().tools.at("mcp__aiad__aiad_engine_set_parameters").calls == 1);

    // Second turn: total_cost_usd is cumulative for the process.
    s.Send("again", {}, 4);
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":0.40,"usage":{"input_tokens":1,"output_tokens":1}})"));
    CHECK(std::abs(s.Stats().costUsd - 0.40) < 1e-12 && std::abs(s.Stats().lastTurnCostUsd - 0.15) < 1e-12 && s.Stats().turns == 2);
    // A result we did not start (replay on --resume) is ignored.
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":9.0})"));
    CHECK(s.Stats().turns == 2 && s.Stats().costUsd < 1.0);
}

// A prompt sent while the agent works is queued by the CLI, which either folds it into the running turn
// (one result: the session must not stay busy waiting for a second one) or answers it as the next turn
// (its stream makes the session busy again). Every turn is counted; a replayed result is not.
static void QueuedPrompts()
{
    SessionOptions o;
    o.offline = true;
    AgentSession s(o);
    s.Send("first", {}, 0);
    s.Send("second, while the first runs", {}, 0);
    CHECK(s.Busy() && s.OfflineWrites().size() == 2);  // both reach the CLI at once
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":0.1,"usage":{"input_tokens":5,"output_tokens":7}})"));
    CHECK(!s.Busy() && s.Stats().turns == 1);   // folded into the first turn
    s.Feed(J(R"({"type":"stream_event","event":{"type":"message_start","message":{"id":"m9","usage":{"input_tokens":1}}}})"));
    CHECK(s.Busy());                            // ... or answered as a turn of its own
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":0.25,"usage":{"input_tokens":3,"output_tokens":4}})"));
    CHECK(!s.Busy() && s.Stats().turns == 2 && s.Stats().outputTokens == 11 && std::abs(s.Stats().costUsd - 0.25) < 1e-12);
    s.Feed(J(R"({"type":"result","subtype":"success","total_cost_usd":9.0})"));  // a replay: ignored
    CHECK(s.Stats().turns == 2);
    // Stop with nothing running on the CLI side: the session is idle at once.
    s.Send("third", {}, 0);
    s.Interrupt();
    CHECK(!s.Busy());
}

static void Interactions()
{
    SessionOptions o;
    o.offline = true;
    AgentSession s(o);
    s.Send("go", {}, 0);
    s.Feed(J(R"({"type":"control_request","request_id":"r1","request":{"subtype":"can_use_tool","tool_name":"mcp__aiad__aiad_engine_set_parameters",
                "input":{"parameters":{"stroke":64}},"permission_suggestions":[{"type":"addRules","rules":[{"toolName":"mcp__aiad__aiad_engine_set_parameters"}],"behavior":"allow","destination":"session"}]}})"));
    s.Feed(J(R"({"type":"control_request","request_id":"r2","request":{"subtype":"can_use_tool","tool_name":"AskUserQuestion",
                "input":{"questions":[{"question":"Which bore?","header":"Bore","multiSelect":false,"options":[{"label":"80 mm","description":"stock"},{"label":"82 mm","description":"+5%"}]}]}}})"));
    s.Feed(J(R"({"type":"control_request","request_id":"r3","request":{"subtype":"can_use_tool","tool_name":"ExitPlanMode","input":{"plan":"1. Read\n2. Change"}}})"));
    s.Feed(J(R"({"type":"control_request","request_id":"r4","request":{"subtype":"can_use_tool","tool_name":"Bash","input":{"command":"rm -rf /"}}})"));
    CHECK(s.Pending().size() == 4);
    CHECK(s.Pending()[1].kind == RequestKind::Question && s.Pending()[1].questions[0].options.size() == 2);
    CHECK(s.Pending()[2].kind == RequestKind::Plan && s.Pending()[2].plan == "1. Read\n2. Change");

    s.Allow("r1", true);
    s.Answer("r2", {{"Which bore?", "82 mm\nkeep the stroke"}});
    s.ApprovePlan("r3", "1. Read\n2. Change\n3. Snapshot");
    s.Deny("r4", "Not on this machine");
    CHECK(s.Pending().empty());
    const auto& w = s.OfflineWrites();
    CHECK(w.size() == 5);  // user message + 4 answers
    const Json& allow = w[1]["response"];
    CHECK(w[1]["type"] == "control_response" && allow["subtype"] == "success" && allow["request_id"] == "r1");
    CHECK(allow["response"]["behavior"] == "allow" && allow["response"]["updatedInput"]["parameters"]["stroke"] == 64);
    CHECK(allow["response"]["updatedPermissions"][0]["behavior"] == "allow");
    CHECK(w[2]["response"]["response"]["updatedInput"]["answers"]["Which bore?"] == "82 mm\nkeep the stroke");
    CHECK(w[2]["response"]["response"]["updatedInput"]["questions"].size() == 1);
    CHECK(w[3]["response"]["response"]["updatedInput"]["plan"] == "1. Read\n2. Change\n3. Snapshot");
    CHECK(w[4]["response"]["response"]["behavior"] == "deny" && w[4]["response"]["response"]["message"] == "Not on this machine");

    // Compaction and an engine warning (recognised by shape, shown once).
    s.Feed(J(R"({"type":"system","subtype":"status","status":"compacting"})"));
    CHECK(s.Compacting());
    s.Feed(J(R"({"type":"system","subtype":"compact_boundary","compact_metadata":{"trigger":"auto","pre_tokens":180000,"post_tokens":20000}})"));
    s.Feed(J(R"({"type":"system","subtype":"fast_mode_credit_warning","warning":"Fast mode credits exhausted"})"));
    s.Feed(J(R"({"type":"system","subtype":"fast_mode_credit_warning","warning":"Fast mode credits exhausted"})"));
    int compactions = 0, notices = 0;
    for (const TimelineItem& i : s.Timeline())
    {
        compactions += i.kind == ItemKind::Compaction && i.preTokens == 180000 && i.postTokens == 20000;
        notices += i.kind == ItemKind::Notice;
    }
    CHECK(!s.Compacting() && compactions == 1 && notices == 1 && s.Stats().compactions == 1);

    s.Feed(J(R"({"type":"rate_limit_event","rate_limit_info":{"status":"allowed_warning","rateLimitType":"five_hour","utilization":0.91,"resetsAt":1790000000}})"));
    CHECK(s.Stats().fiveHour.status == "allowed_warning" && s.Stats().fiveHour.utilization > 0.9 && s.Stats().fiveHour.resetsAt == 1790000000);
}

static void SavedContexts()
{
    // A private Claude config folder with one synthetic transcript.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aiad_transcripts_test";
    std::filesystem::remove_all(root);
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root.wstring().c_str());
    const std::filesystem::path cwd = L"D:\\Work Space\\Motor (v2)";
    CHECK(EncodeCwd(cwd.wstring()) == "D--Work-Space-Motor--v2-");
    const std::filesystem::path dir = root / "projects" / "D--Work-Space-Motor--v2-";
    WriteTextFile(dir / "s1.jsonl",
                  R"({"type":"user","message":{"role":"user","content":"<command-name>/clear</command-name>"}})" "\n"
                  R"({"type":"user","isMeta":true,"message":{"role":"user","content":"meta"}})" "\n"
                  R"({"type":"user","message":{"role":"user","content":"Design a four stroke engine. It must run."},"uuid":"u1"})" "\n"
                  R"({"type":"assistant","message":{"id":"a1","model":"claude-opus-5-5","content":[{"type":"text","text":"Sure."},{"type":"tool_use","id":"t1","name":"Read","input":{"file_path":"x"}}]}})" "\n"
                  R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"file text"}]}})" "\n"
                  R"({"type":"user","message":{"role":"user","content":"Now the magneto."},"uuid":"u2"})" "\n"
                  R"({"type":"assistant","message":{"id":"a2","content":[{"type":"text","text":"Ok."}]}})" "\n");
    const auto list = ListTranscripts(cwd);
    CHECK(list.size() == 1);
    if (!list.empty())
    {
        CHECK(list[0].id == "s1" && list[0].title == "Design a four stroke engine." && list[0].userPrompts == 2 && list[0].toolCalls == 1);
        CHECK(list[0].model == "claude-opus-5-5");
    }
    auto items = LoadTranscript(cwd, "s1");
    CHECK(items.size() == 5);  // prompt, text, tool, prompt, text
    if (items.size() == 5)
        CHECK(items[0].text.starts_with("Design") && items[2].tool.resultText == "file text" && items[2].tool.done && items[3].text == "Now the magneto.");

    SetCustomTitle("s1", "Engine M1");
    CHECK(ListTranscripts(cwd)[0].title == "Engine M1");
    SetCustomTitle("s1", "");

    CHECK(TruncateTranscript(cwd, "s1", 1));
    items = LoadTranscript(cwd, "s1");
    CHECK(items.size() == 3 && items.back().kind == ItemKind::Tool);
    CHECK(DeleteTranscript(cwd, "s1") && ListTranscripts(cwd).empty());
    CHECK(ShortTitle(std::string(100, 'a'), 60).size() <= 63);
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", nullptr);
    std::filesystem::remove_all(root);
}

static void AccountParsing()
{
    const AccountUsage u = ParseUsage(J(R"({"limits":[{"kind":"session","utilization":42,"resets_at":"2026-10-19T03:00:00Z"},
        {"kind":"weekly_all","utilization":7},{"kind":"weekly_scoped","utilization":12,"scope":{"model":{"display_name":"Opus"}}}]})"));
    CHECK(u.ok && u.windows.size() == 3 && std::abs(u.windows[0].used - 0.42) < 1e-12 && u.windows[2].label == "Weekly (Opus)");
    const AccountUsage legacy = ParseUsage(J(R"({"five_hour":{"utilization":50},"seven_day":{"utilization":10}})"));
    CHECK(legacy.ok && legacy.windows.size() == 2);
    const auto prices = ParsePricingMarkdown(
        "# Pricing\n## Model pricing\n| Model | Base Input Tokens | 5m Cache Writes | 1h Cache Writes | Cache Hits | Output Tokens |\n"
        "|---|---|---|---|---|---|\n| Claude Opus 5.5 | $5 / MTok | $6.25 / MTok | $10 / MTok | $0.50 / MTok | $25 / MTok |\n"
        "| Claude Opus 5 | $15 / MTok | x | x | x | $75 / MTok |\n| [Claude Haiku 5.5](/x) | $1 / MTok | x | x | x | $5 / MTok |\n## Batch\n| Claude Opus 5.5 | $99 | | | | $99 |\n");
    CHECK(prices.size() == 3 && prices.at("claude-opus-5-5").input == 5.0 && prices.at("claude-opus-5-5").output == 25.0);
    CHECK(PriceFor(prices, "claude-opus-5-5[1m]")->input == 5.0 && PriceFor(prices, "claude-opus-5-20260101")->input == 15.0);
    CHECK(PriceFor(prices, "claude-haiku-5-5")->output == 5.0 && !PriceFor(prices, "gpt-9"));
    CHECK(IsOutdated("2.1.302 (Claude Code)", "2.1.310") && !IsOutdated("2.1.302", "2.1.302") && !IsOutdated("2.2.0", "2.1.999"));
}

int main()
{
    Streaming();
    QueuedPrompts();
    Interactions();
    SavedContexts();
    AccountParsing();
    std::printf(g_failures ? "agent_session_test: %d failure(s)\n" : "agent_session_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
