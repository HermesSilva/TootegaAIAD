// Saved contexts: the sessions Claude Code persists in <claude config>/projects/<encoded cwd>/<id>.jsonl.
// List (title, date, size, counts), load into a timeline (resume), rename, delete, rewind.
#pragma once

#include "AgentSession.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace aiad::ai
{

struct TranscriptInfo
{
    std::string id, title, model;
    std::chrono::system_clock::time_point updated;
    uint64_t sizeBytes = 0;
    int messages = 0, userPrompts = 0, assistantMessages = 0, toolCalls = 0;
};

// Claude Code's folder name for a cwd: every non-alphanumeric character becomes '-'.
std::string EncodeCwd(const std::wstring& cwd);
std::filesystem::path ProjectDir(const std::filesystem::path& cwd);

// Most recent first. Summaries are cached by (path, size, mtime): big transcripts are read once.
std::vector<TranscriptInfo> ListTranscripts(const std::filesystem::path& cwd);
std::vector<TimelineItem> LoadTranscript(const std::filesystem::path& cwd, const std::string& id);
bool DeleteTranscript(const std::filesystem::path& cwd, const std::string& id);
int DeleteAllTranscripts(const std::filesystem::path& cwd);
// Rewind: keeps the transcript lines before the `promptIndex`-th user prompt (0-based, counted as
// LoadTranscript counts them). Atomic (tmp + rename).
bool TruncateTranscript(const std::filesystem::path& cwd, const std::string& id, int promptIndex);

// User titles (rename), kept by the app in %APPDATA%\Tootega\AIAD\titles.json.
std::string CustomTitle(const std::string& id);
void SetCustomTitle(const std::string& id, const std::string& title);

// Helpers shared with the parser tests.
bool IsMetaUserText(const std::string& text);
std::string ShortTitle(const std::string& text, size_t max = 60);

}  // namespace aiad::ai
