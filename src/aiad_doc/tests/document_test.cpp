// Document + commands. What failure would this catch?
// - a parameter change that does not reach the parts using it (drawing and physics out of step);
// - a failed regeneration leaving the document half changed (must be atomic);
// - undo not restoring the exact part, or a reopened file losing the exact geometry;
// - formulas: wrong precedence, unknown names or cycles accepted silently;
// - sketch tools (cam, sprocket, hull, polygon) producing invalid solids or a cam lift off its law.
#include "DocCommands.h"
#include "Document.h"
#include "Expression.h"
#include "Features.h"

#include <pxr/base/plug/registry.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>

using namespace aiad;
using namespace aiad::doc;

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

static bool Near(double a, double b, double rel)
{
    return std::abs(a - b) <= rel * std::max(std::abs(b), 1e-12);
}

static double Volume(Document& d, const std::string& part)
{
    std::string e;
    auto p = d.FindPart(part, e);
    kernel::MassProperties mp;
    return p && d.Mass(p->path, mp, e) ? mp.volume : -1.0;
}

int main()
{
    _putenv_s("OMNISOLID_PLUGIN_PATH", AIAD_OMNISOLID_DIR);
    pxr::PlugRegistry::GetInstance().RegisterPlugins(std::string(AIAD_USD_PLUGIN_DIR));
    kernel::Initialize();
    const double pi = std::numbers::pi;

    // Formulas.
    double v = 0;
    std::string e;
    auto names = [](const std::string& n, double& out) {
        if (n == "bore")
            return out = 0.08, true;
        return false;
    };
    CHECK(Evaluate("bore / 2 + 2 * 3 ^ 2", names, v, e) && Near(v, 0.04 + 18, 1e-12));
    CHECK(Evaluate("max(1, -2, 5) - sin(30) * 2", names, v, e) && Near(v, 4.0, 1e-12));
    CHECK(Evaluate("1e-3 * 2", names, v, e) && Near(v, 0.002, 1e-12));
    CHECK(!Evaluate("stroke * 2", names, v, e) && e.find("unknown name 'stroke'") != std::string::npos);
    CHECK(!Evaluate("(1 + 2", names, v, e));
    CHECK(ReferencedNames("max(bore, r1) * 1e-3 + pi") == std::set<std::string>({"bore", "r1"}));

    cmd::CommandBus bus;
    std::unique_ptr<Document> doc = Document::Create("Test");
    RegisterDocumentCommands(bus, [&] { return doc.get(); });

    // A script builds a parametric part, as the engine model is built.
    const Json script = Json::parse(R"([
      {"command":"aiad_parameter_define","args":{"name":"bore","value":0.08,"unit":"m","min":0.04,"max":0.12}},
      {"command":"aiad_parameter_define","args":{"name":"wall","value":"bore * 0.05","unit":"m"}},
      {"command":"aiad_parameter_define","args":{"name":"height","value":0.1,"unit":"m"}},
      {"command":"aiad_material_define","args":{"name":"Steel","density":7850,"colour":[0.6,0.6,0.62],"metallic":1,"roughness":0.3}},
      {"command":"aiad_part_create","args":{"name":"Liner","body":"Frame","material":"Steel","sectioned":true,
        "features":[{"op":"revolve","plane":"YX","profile":[["bore/2",0],["bore/2+wall",0],["bore/2+wall","height"],["bore/2","height"]]}]}},
      {"command":"aiad_part_create","args":{"name":"Flange","body":"Frame","material":"Steel","features":[
        {"op":"extrude","plane":"XY","profile":{"rounded_rect":{"min":[-0.06,-0.06],"max":[0.06,0.06],"radius":0.01}},"distance":0.01},
        {"op":"cylinder","mode":"cut","base":[0.045,0,-0.01],"axis":[0,0,1],"radius":0.004,"height":0.03},
        {"op":"pattern_polar","count":4,"axis_point":[0,0,0],"axis":[0,0,1],"step_angle":90}]}}
    ])");
    int steps = 0;
    std::string error;
    CHECK(RunScript(bus, script, error, &steps) && steps == 6);
    if (!error.empty())
        std::printf("script: %s\n", error.c_str());
    const auto tube = [&](double bore, double wall, double h) { return pi * (std::pow(bore / 2 + wall, 2) - std::pow(bore / 2, 2)) * h; };
    CHECK(Near(Volume(*doc, "Liner"), tube(0.08, 0.004, 0.1), 1e-3));
    const double plate = (0.12 * 0.12 - (4 - pi) * 0.01 * 0.01) * 0.01 - 4 * pi * 0.004 * 0.004 * 0.01;
    CHECK(Near(Volume(*doc, "Flange"), plate, 2e-3));

    // A parameter change reaches the part through the formula chain (wall = bore * 0.05) and only it.
    cmd::CommandOutput set = bus.Execute("aiad_parameters_set", {{"values", {{"bore", 0.1}}}}, "ai");
    CHECK(set.ok && set.result["regenerated"].size() == 1 && set.result["regenerated"][0] == "Liner");
    CHECK(Near(Volume(*doc, "Liner"), tube(0.1, 0.005, 0.1), 1e-3));
    CHECK(Near(Volume(*doc, "Flange"), plate, 2e-3));
    CHECK(bus.Undo() && Near(Volume(*doc, "Liner"), tube(0.08, 0.004, 0.1), 1e-3) && Near(doc->Value("bore"), 0.08, 1e-12));
    CHECK(bus.Redo() && Near(doc->Value("bore"), 0.1, 1e-12));
    bus.Undo();

    // Out of range, unknown, cycle: refused, nothing changed.
    CHECK(!bus.Execute("aiad_parameters_set", {{"values", {{"bore", 0.5}}}}, "ai").ok);
    CHECK(!bus.Execute("aiad_parameters_set", {{"values", {{"stroke", 0.5}}}}, "ai").ok);
    CHECK(!bus.Execute("aiad_parameters_set", {{"values", {{"height", "wall * 2"}, {"wall", "height"}}}}, "ai").ok);
    CHECK(Near(doc->Value("bore"), 0.08, 1e-12) && Near(doc->Value("wall"), 0.004, 1e-12));
    // Atomic: a value that cannot be built (zero height) leaves everything as it was.
    const double before = Volume(*doc, "Liner");
    CHECK(!bus.Execute("aiad_parameters_set", {{"values", {{"height", 0.0}}}}, "ai").ok);
    CHECK(Near(Volume(*doc, "Liner"), before, 1e-9) && Near(doc->Value("height"), 0.1, 1e-12));

    // Feature edit and part update, undoable.
    CHECK(bus.Execute("aiad_feature_update", {{"part", "Flange"}, {"index", 1}, {"set", {{"distance", 0.02}}}}, "ai").ok);
    CHECK(Volume(*doc, "Flange") > 1.5 * plate);
    CHECK(bus.Undo() && Near(Volume(*doc, "Flange"), plate, 2e-3));
    CHECK(bus.Execute("aiad_part_update", {{"part", "Flange"}, {"material", "Steel"}, {"body", "Crankshaft"}}, "user").ok);
    CHECK(doc->FindPart("Flange", error) && doc->FindPart("Flange", error)->body == "Crankshaft");
    CHECK(bus.Undo() && doc->FindPart("Flange", error)->body == "Frame");
    CHECK(bus.Execute("aiad_part_delete", {{"part", "flan"}}, "ai").ok && !doc->FindPart("Flange", error));
    CHECK(bus.Undo() && Near(Volume(*doc, "Flange"), plate, 2e-3));
    CHECK(!bus.Execute("aiad_part_create", {{"name", "Bad"}, {"body", "Frame"}, {"material", "Steel"}, {"features", {{{"op", "extrude"}, {"plane", "XY"}}}}}, "ai").ok);
    CHECK(!doc->FindPart("Bad", error));

    // Sketch tools.
    const auto lookup = doc->Lookup();
    CHECK(BuildPart(Json::parse(R"([{"op":"extrude","plane":"XY","profile":{"sprocket":{"root_radius":0.018,"tip_radius":0.021,"teeth":18}},"distance":0.01}])"), lookup, e).Valid());
    CHECK(BuildPart(Json::parse(R"([{"op":"extrude","plane":"XY","profile":{"hull":{"circles":[{"center":[0,0],"radius":0.027},{"center":[0.03,0],"radius":0.021},
        {"center":[0,0],"radius":0.046,"from":135,"to":225}]}},"distance":0.01}])"), lookup, e).Valid());
    CHECK(BuildPart(Json::parse(R"([{"op":"extrude","plane":"XY","profile":{"polygon":{"center":[0,0],"across_flats":0.024,"sides":6}},"distance":0.01}])"), lookup, e).Valid());
    const Json cam = Json::parse(R"({"cam":{"base_radius":0.016,"lift":0.008,"open":350,"close":580}})");
    CHECK(BuildPart(Json::array({{{"op", "extrude"}, {"plane", "XY"}, {"profile", cam}, {"distance", 0.013}}}), lookup, e).Valid());
    std::vector<std::array<double, 2>> outline;
    CHECK(SketchOutline(cam, lookup, outline, e));
    const std::vector<double> lift = MeasureFlatFollowerLift(outline, 0.016);
    // Peak of the sin^2 law at the middle of the opening (465 deg) and closed far from it.
    CHECK(Near(lift[465], 0.008, 0.02) && lift[100] < 1e-4 && lift[700] < 1e-4);

    // Save, reopen: the exact geometry comes back from the BrepArray prims.
    const std::string file = (std::filesystem::temp_directory_path() / "aiad_document_test.usda").string();
    CHECK(bus.Execute("aiad_doc_save", {{"path", file}}, "user").ok);
    std::unique_ptr<Document> reopened = Document::Open(file, error);
    CHECK(reopened && reopened->Parts().size() == 2);
    if (reopened)
        CHECK(Near(Volume(*reopened, "Liner"), Volume(*doc, "Liner"), 1e-6) && Near(reopened->Value("wall"), 0.004, 1e-12));
    std::filesystem::remove(file);

    std::printf(g_failures ? "document_test: %d failure(s)\n" : "document_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
