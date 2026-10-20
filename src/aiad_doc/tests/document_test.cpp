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

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>

#include <algorithm>
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

    // A body moved (as the AI does when it repositions the camshaft), then undone: the previous rest
    // placement, a formula, comes back.
    CHECK(bus.Execute("aiad_body_define", {{"body", "Cam"}, {"origin", Json::array({"bore", 0, 0})}}, "ai").ok);
    CHECK(bus.Execute("aiad_body_define", {{"body", "Cam"}, {"origin", Json::array({"bore", -0.08, 0})}}, "ai").ok);
    CHECK(Near(doc->BodyOrigin("Cam").y, -0.08, 1e-12));
    CHECK(bus.Undo() && Near(doc->BodyOrigin("Cam").y, 0.0, 1e-12) && Near(doc->BodyOrigin("Cam").x, 0.08, 1e-12));

    // The mechanism is part of the document: a fixed body (ground) and a joint (UsdPhysics prim), with
    // formulas that follow the parameters, checked on definition, undoable.
    CHECK(bus.Execute("aiad_body_define", {{"body", "Base"}, {"origin", Json::array({0, 0, 0})}, {"fixed", true}}, "user").ok && doc->BodyFixed("Base") &&
          !doc->BodyFixed("Cam"));
    CHECK(bus.Execute("aiad_joint_define", {{"name", "Hinge"}, {"type", "revolute"}, {"body0", "Cam"}, {"body1", "Base"},
                                            {"point", Json::array({"bore / 2", 0, 0})}, {"axis", Json::array({0, 0, 1})},
                                            {"motor", {{"mode", "speed"}, {"value", 10}}}},
                      "ai")
              .ok);
    const pxr::UsdPrim hinge = doc->Stage()->GetPrimAtPath(doc->Root().AppendChild(pxr::TfToken("Joints")).AppendChild(pxr::TfToken("Hinge")));
    auto localPos0 = [&] {
        pxr::GfVec3f v(0.0f);
        hinge.GetAttribute(pxr::TfToken("physics:localPos0")).Get(&v);
        return v;
    };
    CHECK(hinge.GetTypeName() == "PhysicsRevoluteJoint" && Near(localPos0()[0], -0.04, 1e-6));   // bore/2 - Cam origin (bore)
    CHECK(bus.Execute("aiad_parameters_set", {{"values", {{"bore", 0.1}}}}, "user").ok && Near(localPos0()[0], -0.05, 1e-6));
    bus.Undo();
    CHECK(!bus.Execute("aiad_joint_define", {{"name", "Bad"}, {"type", "revolute"}, {"body0", "Nope"}, {"point", Json::array({0, 0, 0})}}, "ai").ok);
    CHECK(!bus.Execute("aiad_joint_define", {{"name", "Bad"}, {"type", "spherical"}, {"body0", "Cam"}, {"point", Json::array({0, 0, 0})},
                                             {"motor", {{"mode", "speed"}, {"value", 1}}}},
                       "ai")
               .ok);   // a motor needs a joint with an axis of motion
    CHECK(doc->Joints().size() == 1 && doc->FindJoint("Hinge")->motor.value("mode", "") == "speed");
    CHECK(bus.Undo() && !doc->FindJoint("Hinge") && bus.Redo() && doc->FindJoint("Hinge"));
    // Stops of a joint (a seat): formulas, written as the UsdPhysics limits.
    CHECK(bus.Execute("aiad_joint_define", {{"name", "Slide"}, {"type", "prismatic"}, {"body0", "Frame"}, {"body1", "Base"},
                                            {"point", Json::array({0, 0, 0})}, {"axis", Json::array({1, 0, 0})}, {"limits", Json::array({nullptr, "wall"})}},
                      "ai")
              .ok);
    float upper = 0.0f;
    doc->Stage()->GetPrimAtPath(doc->Root().AppendChild(pxr::TfToken("Joints")).AppendChild(pxr::TfToken("Slide"))).GetAttribute(pxr::TfToken("physics:upperLimit")).Get(&upper);
    CHECK(Near(upper, 0.004, 1e-6) && doc->FindJoint("Slide")->limits[0].is_null());
    CHECK(!bus.Execute("aiad_joint_define", {{"name", "Bad"}, {"type", "spherical"}, {"body0", "Cam"}, {"point", Json::array({0, 0, 0})},
                                             {"limits", Json::array({0, 1})}},
                       "ai")
               .ok);   // a ball joint has no coordinate to stop

    // A gear drawn with the involute tool: about the pitch disc in area, measured as a wheel (teeth, pitch
    // radius, axis) for the couplings.
    CHECK(bus.Execute("aiad_part_create", {{"name", "Pinion"}, {"body", "Cam"}, {"material", "Steel"},
                                           {"features", {{{"op", "extrude"}, {"plane", "XY"}, {"distance", 0.01},
                                                          {"profile", {{"gear", {{"module", 0.002}, {"teeth", 20}}}}}}}}},
                      "ai")
              .ok);
    CHECK(Near(Volume(*doc, "Pinion"), pi * 0.02 * 0.02 * 0.01, 0.06));
    Wheel wheel;
    CHECK(FindWheel(doc->FindPart("Pinion", error)->features, doc->Lookup(), wheel, error) && wheel.teeth == 20 && Near(wheel.pitchRadius, 0.02, 1e-12) &&
          Near(wheel.axis.z, 1.0, 1e-12) && Near(wheel.centre.z, 0.005, 1e-12));
    // A profile turned by a later feature is measured where it ends up.
    ExtrudedProfile turned;
    const Json lobe = {{{"op", "extrude"}, {"plane", "XY"}, {"distance", 0.01}, {"profile", {{"rect", {{"min", {0.01, -0.001}}, {"max", {0.02, 0.001}}}}}}},
                       {{"op", "rotate"}, {"axis", {0, 0, 1}}, {"angle", 180}}};
    CHECK(FindExtrudedProfile(lobe, doc->Lookup(), turned, error) &&
          std::all_of(turned.points.begin(), turned.points.end(), [](const kernel::Vec3& p) { return p.x < -0.0099 && p.x > -0.0201; }));

    // A hairspring drawn with the spiral tool (strip 50 um x 0.2 mm, 10 turns from 0.6 mm, pitch 0.2 mm), measured
    // on its section as the simulator measures drawn and imported springs: length half the perimeter, thickness the
    // area over it. After its mass was computed: SMLib's mass integration used to spoil every later tessellation.
    CHECK(bus.Execute("aiad_part_create",
                      {{"name", "Hairspring"},
                       {"body", "Cam"},
                       {"material", "Steel"},
                       {"features",
                        {{{"op", "extrude"},
                          {"plane", "XY"},
                          {"distance", 0.0002},
                          {"profile", {{"spiral", {{"inner_radius", 0.0006}, {"pitch", 0.0002}, {"turns", 10}, {"thickness", 0.00005}}}}}}}}},
                      "ai")
              .ok);
    if (const auto spring = doc->FindPart("Hairspring", error))
    {
        kernel::MassProperties mp;
        CHECK(doc->Mass(spring->path, mp, error));
        const auto loops = doc->SectionOutline(spring->path, {0, 0, 0.0001}, {0, 0, 1});
        double perimeter = 0.0, area = 0.0;
        for (const auto& loop : loops)
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const kernel::Vec3& a = loop[i];
                const kernel::Vec3& b = loop[(i + 1) % loop.size()];
                perimeter += std::hypot(b.x - a.x, b.y - a.y);
                area += (a.x * b.y - b.x * a.y) / 2.0;
            }
        double length = 0.0;   // the centre line r = 0.6 mm + 0.2 mm a / 2 pi
        for (int i = 0; i < 100000; ++i)
        {
            const double a = (i + 0.5) * 20.0 * pi / 100000, r = 0.0006 + 0.0002 * a / (2.0 * pi);
            length += std::hypot(r, 0.0002 / (2.0 * pi)) * 20.0 * pi / 100000;
        }
        std::printf("spiral: strip %.2f mm long (centre line %.2f mm), %.2f um thick\n", perimeter / 2.0 * 1e3, length * 1e3, std::abs(area) / (perimeter / 2.0) * 1e6);
        CHECK(loops.size() == 1 && Near(perimeter / 2.0, length, 0.01) && Near(std::abs(area) / (perimeter / 2.0), 0.00005, 0.02));
    }
    CHECK(bus.Execute("aiad_part_delete", {{"part", "Hairspring"}}, "ai").ok);

    // Elements: tied to joints, parts and bodies by name, checked on definition, undoable.
    CHECK(bus.Execute("aiad_element_define", {{"name", "Mesh"}, {"type", "rack"}, {"definition", {{"joint0", "Hinge"}, {"joint1", "Slide"}, {"part0", "Pinion"}}}},
                      "ai")
              .ok &&
          doc->FindElement("Mesh") && doc->FindElement("Mesh")->type == "rack");
    CHECK(!bus.Execute("aiad_element_define", {{"name", "Bad"}, {"type", "gear"}, {"definition", {{"joint0", "Hinge"}, {"joint1", "Nope"}, {"part0", "Pinion"}, {"part1", "Pinion"}}}},
                       "ai")
               .ok);   // unknown joint
    CHECK(!bus.Execute("aiad_element_define", {{"name", "Bad"}, {"type", "load"}, {"definition", {{"joint", "Hinge"}, {"friction", 1}}}}, "ai").ok);   // unknown field
    CHECK(!bus.Execute("aiad_element_define", {{"name", "Bad"}, {"type", "load"}, {"definition", {{"joint", "Hinge"}, {"constant", "nope * 2"}}}}, "ai").ok);
    CHECK(bus.Execute("aiad_element_define", {{"name", "Drag"}, {"type", "load"}, {"definition", {{"joint", "Hinge"}, {"quadratic", "bore * 0.01"}}}}, "ai").ok);
    CHECK(doc->Elements().size() == 2 && bus.Undo() && doc->Elements().size() == 1 && bus.Undo() && doc->Elements().empty());
    CHECK(bus.Execute("aiad_part_delete", {{"part", "Pinion"}}, "ai").ok);

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
