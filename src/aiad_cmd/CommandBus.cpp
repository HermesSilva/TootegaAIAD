#include "CommandBus.h"

#include <algorithm>
#include <format>

namespace aiad::cmd
{

std::string Validate(const Json& schema, const Json& value, const std::string& path)
{
    if (!schema.is_object())
        return {};
    if (schema.contains("type"))
    {
        auto matches = [&](const std::string& type) {
            return type == "object"    ? value.is_object()
                   : type == "array"   ? value.is_array()
                   : type == "string"  ? value.is_string()
                   : type == "number"  ? value.is_number()
                   : type == "integer" ? value.is_number_integer() || (value.is_number_float() && value.get<double>() == double(int64_t(value.get<double>())))
                   : type == "boolean" ? value.is_boolean()
                   : type == "null"    ? value.is_null()
                                       : true;
        };
        const Json& type = schema["type"];
        bool ok = false;
        if (type.is_string())
            ok = matches(type.get<std::string>());
        else if (type.is_array())
            for (const Json& t : type)
                ok = ok || (t.is_string() && matches(t.get<std::string>()));
        if (!ok)
            return std::format("{} must be of type {} (got {})", path, type.is_string() ? type.get<std::string>() : type.dump(), value.type_name());
    }
    if (schema.contains("enum"))
    {
        const Json& options = schema["enum"];
        if (std::find(options.begin(), options.end(), value) == options.end())
            return std::format("{} must be one of {}", path, options.dump());
    }
    if (value.is_number())
    {
        const double v = value.get<double>();
        if (schema.contains("minimum") && v < schema["minimum"].get<double>())
            return std::format("{} = {} is below the minimum {}", path, v, schema["minimum"].get<double>());
        if (schema.contains("maximum") && v > schema["maximum"].get<double>())
            return std::format("{} = {} is above the maximum {}", path, v, schema["maximum"].get<double>());
    }
    if (value.is_object())
    {
        if (schema.contains("required"))
            for (const auto& key : schema["required"])
                if (!value.contains(key.get<std::string>()))
                    return std::format("{}.{} is required", path, key.get<std::string>());
        const Json properties = schema.value("properties", Json::object());
        const bool closed = schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                            !schema["additionalProperties"].get<bool>();
        for (const auto& [key, item] : value.items())
        {
            if (properties.contains(key))
            {
                if (std::string e = Validate(properties[key], item, path + "." + key); !e.empty())
                    return e;
            }
            else if (closed)
            {
                std::string known;
                for (const auto& [k, _] : properties.items())
                    known += (known.empty() ? "" : ", ") + k;
                return std::format("{}.{} is not a known argument (known: {})", path, key, known);
            }
            else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object())
            {
                if (std::string e = Validate(schema["additionalProperties"], item, path + "." + key); !e.empty())
                    return e;
            }
        }
    }
    if (value.is_array() && schema.contains("items"))
        for (size_t i = 0; i < value.size(); ++i)
            if (std::string e = Validate(schema["items"], value[i], std::format("{}[{}]", path, i)); !e.empty())
                return e;
    return {};
}

CommandBus::CommandBus()
{
    // History is part of the bus itself: the AI undoes with the same commands as Ctrl+Z.
    Register({"aiad_undo", "Undo", "Undoes the last model change (any actor). Same as Ctrl+Z.", Json{{"type", "object"}, {"properties", Json::object()}},
              {}, [this](const Json&) {
                  if (!Undo())
                      return CommandOutput::Fail("Nothing to undo.");
                  CommandOutput o;
                  o.text = "Undone.";
                  return o;
              }});
    Register({"aiad_redo", "Redo", "Redoes the last undone model change. Same as Ctrl+Y.", Json{{"type", "object"}, {"properties", Json::object()}},
              {}, [this](const Json&) {
                  if (!Redo())
                      return CommandOutput::Fail("Nothing to redo.");
                  CommandOutput o;
                  o.text = "Redone.";
                  return o;
              }});
    Register({"aiad_history", "History", "Lists the undo history (oldest first) and the current position.",
              Json{{"type", "object"}, {"properties", Json::object()}}, {true, false, true}, [this](const Json&) {
                  CommandOutput o;
                  Json list = Json::array();
                  std::string text = "| # | Change | By | State |\n|---|---|---|---|\n";
                  const auto history = History();
                  for (size_t i = 0; i < history.size(); ++i)
                  {
                      list.push_back({{"label", history[i].label}, {"actor", history[i].actor}, {"done", i < m_cursor}});
                      text += std::format("| {} | {} | {} | {} |\n", i + 1, history[i].label, history[i].actor, i < m_cursor ? "done" : "undone");
                  }
                  o.result = {{"entries", list}, {"position", m_cursor}};
                  o.text = history.empty() ? "No changes yet." : text;
                  return o;
              }});
}

void CommandBus::Register(CommandSpec spec)
{
    m_commands.erase(std::remove_if(m_commands.begin(), m_commands.end(), [&](const CommandSpec& c) { return c.name == spec.name; }),
                     m_commands.end());
    m_commands.push_back(std::move(spec));
}

const CommandSpec* CommandBus::Find(const std::string& name) const
{
    for (const CommandSpec& c : m_commands)
        if (c.name == name)
            return &c;
    return nullptr;
}

Json CommandBus::CatalogJson() const
{
    Json list = Json::array();
    for (const CommandSpec& c : m_commands)
        list.push_back({{"name", c.name},
                        {"title", c.title},
                        {"description", c.description},
                        {"inputSchema", c.inputSchema},
                        {"annotations",
                         {{"title", c.title},
                          {"readOnlyHint", c.flags.readOnly},
                          {"destructiveHint", c.flags.destructive},
                          {"idempotentHint", c.flags.idempotent},
                          {"openWorldHint", false}}}});
    return list;
}

CommandOutput CommandBus::Execute(const std::string& name, const Json& args, const std::string& actor)
{
    AuditEntry audit{std::chrono::system_clock::now(), actor, name, args};
    CommandOutput out;
    const CommandSpec* spec = Find(name);
    if (!spec)
    {
        std::string known;
        for (const CommandSpec& c : m_commands)
            known += (known.empty() ? "" : ", ") + c.name;
        out = CommandOutput::Fail(std::format("Unknown command '{}'. Known commands: {}.", name, known));
    }
    else if (std::string error = Validate(spec->inputSchema, args.is_null() ? Json::object() : args); !error.empty())
    {
        out = CommandOutput::Fail("Invalid arguments: " + error + ".");
    }
    else
    {
        try
        {
            out = spec->handler(args.is_null() ? Json::object() : args);
        }
        catch (const std::exception& e)
        {
            out = CommandOutput::Fail(std::format("{} failed: {}", name, e.what()));
        }
    }
    if (out.ok && out.undo && out.redo)
    {
        m_history.resize(m_cursor);
        m_history.push_back({{out.label.empty() ? spec->title : out.label, actor, audit.time}, out.undo, out.redo});
        m_cursor = m_history.size();
    }
    if (out.ok)
        ++m_revision;
    audit.ok = out.ok;
    audit.error = out.error;
    audit.label = out.label;
    m_audit.push_back(audit);
    if (OnExecuted)
        OnExecuted(audit);
    return out;
}

bool CommandBus::Undo()
{
    if (!CanUndo())
        return false;
    m_history[--m_cursor].undo();
    ++m_revision;
    return true;
}

bool CommandBus::Redo()
{
    if (!CanRedo())
        return false;
    m_history[m_cursor++].redo();
    ++m_revision;
    return true;
}

void CommandBus::UndoTo(size_t depth)
{
    while (m_cursor > depth && Undo())
    {
    }
}

void CommandBus::ClearHistory()
{
    m_history.clear();
    m_cursor = 0;
    ++m_revision;
}

std::vector<HistoryEntry> CommandBus::History() const
{
    std::vector<HistoryEntry> list;
    for (const Transaction& t : m_history)
        list.push_back(t.entry);
    return list;
}

}  // namespace aiad::cmd
