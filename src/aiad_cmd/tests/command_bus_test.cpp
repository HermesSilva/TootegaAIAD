// Command Bus: what failure would this catch?
// - a bad argument reaching a handler (the model would be mutated with garbage);
// - undo/redo/rewind leaving the model out of step with the history;
// - the MCP catalog missing a command or its read-only flag.
#include "CommandBus.h"

#include <cstdio>

using namespace aiad;
using namespace aiad::cmd;

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

int main()
{
    double bore = 80.0;
    CommandBus bus;
    bus.Register({"aiad_test_set_bore", "Set bore", "Sets the bore [mm].",
                  Json{{"type", "object"},
                       {"properties", {{"bore", {{"type", "number"}, {"minimum", 40}, {"maximum", 120}}}}},
                       {"required", {"bore"}},
                       {"additionalProperties", false}},
                  {}, [&](const Json& args) {
                      const double before = bore, after = args["bore"];
                      CommandOutput o;
                      bore = after;
                      o.undo = [&, before] { bore = before; };
                      o.redo = [&, after] { bore = after; };
                      o.label = "bore";
                      return o;
                  }});

    // Validation never reaches the handler.
    CHECK(!bus.Execute("aiad_test_set_bore", Json::object(), "ai").ok);
    CHECK(!bus.Execute("aiad_test_set_bore", {{"bore", "big"}}, "ai").ok);
    CHECK(!bus.Execute("aiad_test_set_bore", {{"bore", 300}}, "ai").ok);
    CHECK(!bus.Execute("aiad_test_set_bore", {{"bore", 82}, {"stroke", 60}}, "ai").ok);
    CHECK(!bus.Execute("aiad_nope", Json::object(), "ai").ok);
    CHECK(bore == 80.0);
    CHECK(bus.Depth() == 0);

    // Transactions, undo, redo, a new change drops the redo branch.
    CHECK(bus.Execute("aiad_test_set_bore", {{"bore", 82}}, "user").ok);
    CHECK(bus.Execute("aiad_test_set_bore", {{"bore", 84}}, "ai").ok);
    CHECK(bore == 84.0 && bus.Depth() == 2);
    CHECK(bus.Execute("aiad_undo", Json::object(), "ai").ok);
    CHECK(bore == 82.0);
    CHECK(bus.Redo() && bore == 84.0);
    CHECK(bus.Undo() && bus.Undo() && bore == 80.0 && !bus.CanUndo());
    CHECK(bus.Execute("aiad_test_set_bore", {{"bore", 90}}, "user").ok);
    CHECK(!bus.CanRedo() && bore == 90.0);

    // Prompt rewind: back to the depth recorded when the prompt was sent.
    const size_t mark = bus.Depth();
    CHECK(bus.Execute("aiad_test_set_bore", {{"bore", 100}}, "ai").ok);
    CHECK(bus.Execute("aiad_test_set_bore", {{"bore", 110}}, "ai").ok);
    bus.UndoTo(mark);
    CHECK(bore == 90.0 && bus.Depth() == mark);

    // Catalog: every command, with its annotations.
    const Json catalog = bus.CatalogJson();
    bool foundHistory = false, foundSet = false;
    for (const Json& c : catalog)
    {
        foundHistory |= c["name"] == "aiad_history" && c["annotations"]["readOnlyHint"] == true;
        foundSet |= c["name"] == "aiad_test_set_bore" && c["annotations"]["readOnlyHint"] == false && c["inputSchema"]["required"][0] == "bore";
    }
    CHECK(foundHistory && foundSet);
    CHECK(bus.Audit().size() == 11);  // every Execute, failed ones included (direct Undo/Redo are not commands)

    // Another document opened: the old document's undo steps must not run against the new one.
    bus.ClearHistory();
    CHECK(!bus.CanUndo() && !bus.CanRedo() && bus.History().empty() && !bus.Undo() && bore == 90.0);
    CHECK(bus.Audit().size() == 11);

    std::printf(g_failures ? "command_bus_test: %d failure(s)\n" : "command_bus_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
