// The one path that mutates the model. UI, AI (through the bridge and MCP) and scripts run the same
// registered commands, with the same validation, undo/redo and audit trail. The MCP tool catalog is
// generated from this registry (name + JSON Schema + flags), never written twice.
#pragma once

#include "Json.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aiad::cmd
{

struct CommandFlags
{
    bool readOnly = false;     // MCP readOnlyHint; may be auto-approved
    bool destructive = false;  // MCP destructiveHint
    bool idempotent = false;   // MCP idempotentHint
};

struct Image
{
    std::string mimeType;            // "image/png"
    std::vector<uint8_t> bytes;
};

// What a command returns. `text` is the readable answer for an LLM (Markdown); `result` the structured
// one. A mutating command fills undo/redo: the bus keeps them as one transaction.
struct CommandOutput
{
    bool ok = true;
    std::string error;               // when !ok: what failed and how to fix it (never a stack)
    std::string text;
    Json result;
    std::vector<Image> images;
    std::function<void()> undo, redo;
    std::string label;               // history entry ("Set bore = 82 mm")

    static CommandOutput Fail(std::string message)
    {
        CommandOutput o;
        o.ok = false;
        o.error = std::move(message);
        return o;
    }
};

struct CommandSpec
{
    std::string name;                // aiad_<area>_<verb>
    std::string title;
    std::string description;         // for humans and the LLM
    Json inputSchema;                // JSON Schema of the arguments (object)
    CommandFlags flags;
    std::function<CommandOutput(const Json& args)> handler;
};

struct AuditEntry
{
    std::chrono::system_clock::time_point time;
    std::string actor;               // "user", "ai", "script"
    std::string command;
    Json args;
    bool ok = true;
    std::string error;
    std::string label;
};

struct HistoryEntry
{
    std::string label, actor;
    std::chrono::system_clock::time_point time;
};

class CommandBus
{
public:
    CommandBus();

    void Register(CommandSpec spec);
    const std::vector<CommandSpec>& Catalog() const { return m_commands; }
    const CommandSpec* Find(const std::string& name) const;
    // [{name, title, description, inputSchema, annotations{readOnlyHint, destructiveHint, idempotentHint}}]
    Json CatalogJson() const;

    // Main thread only. Validates the arguments against the schema (required keys, types, ranges),
    // runs the handler, records the transaction and the audit entry.
    CommandOutput Execute(const std::string& name, const Json& args, const std::string& actor);

    bool CanUndo() const { return m_cursor > 0; }
    bool CanRedo() const { return m_cursor < m_history.size(); }
    bool Undo();
    bool Redo();
    // Undo stack depth now; UndoTo(depth) rewinds the model to that point (prompt rewind).
    size_t Depth() const { return m_cursor; }
    void UndoTo(size_t depth);
    std::vector<HistoryEntry> History() const;
    const std::vector<AuditEntry>& Audit() const { return m_audit; }
    // Bumped on every successful execute/undo/redo (UI refresh).
    uint64_t Revision() const { return m_revision; }

    std::function<void(const AuditEntry&)> OnExecuted;

private:
    struct Transaction
    {
        HistoryEntry entry;
        std::function<void()> undo, redo;
    };
    std::vector<CommandSpec> m_commands;
    std::vector<Transaction> m_history;
    size_t m_cursor = 0;
    std::vector<AuditEntry> m_audit;
    uint64_t m_revision = 0;
};

// Schema-driven validation (subset of JSON Schema: type, required, properties, enum, minimum,
// maximum, items, additionalProperties=false). Empty string = valid.
std::string Validate(const Json& schema, const Json& value, const std::string& path = "args");

}  // namespace aiad::cmd
