// aiad-step.exe - converts a STEP assembly into an AIAD document and checks that the AIAD kernel reads it back.
//
//   aiad-step <model.step> <out.usda> [--name <root>] [--density <kg/m^3>]
//
// Every part instance of the assembly becomes a body (named after the instance), every solid of it a part
// whose exact geometry is the STEP B-rep (placed, in metres). The file gives no material: one material per
// colour found, all with the same density (--density, default 7850, steel), to be reassigned in the model.
// A part's feature list records where it came from ({"op":"import", ...}); it has no editable history.
//
// The document is then reopened as the application does (Document::Open) and every part is measured with
// the AIAD kernel; the report compares its face count with OCCT's and its volume with a fine tessellation of
// the source solid. Exit code 1 when a part failed.
#include "Document.h"
#include "Paths.h"
#include "StepImport.h"

#include <pxr/base/plug/registry.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdGeom/xform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>

using namespace aiad;
using namespace aiad::doc;

namespace
{

const char* kUsage = "usage: aiad-step <model.step> <out.usda> [--name <root>] [--density <kg/m^3>]\n";

std::string MaterialFor(const std::optional<std::array<double, 3>>& colour)
{
    if (!colour)
        return "Imported";
    const auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return std::format("Imported {:02X}{:02X}{:02X}", byte((*colour)[0]), byte((*colour)[1]), byte((*colour)[2]));
}

// Volume enclosed by a closed, outward-wound triangle mesh (divergence theorem).
double MeshVolume(const kernel::Mesh& mesh)
{
    double volume = 0.0;
    for (const std::array<int, 3>& t : mesh.triangles)
    {
        const std::array<float, 3>&p = mesh.points[t[0]], &q = mesh.points[t[1]], &r = mesh.points[t[2]];
        volume += (double(p[0]) * (double(q[1]) * r[2] - double(q[2]) * r[1]) + double(p[1]) * (double(q[2]) * r[0] - double(q[0]) * r[2]) +
                   double(p[2]) * (double(q[0]) * r[1] - double(q[1]) * r[0])) /
                  6.0;
    }
    return volume;
}

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

static int Run(int argc, char** argv)
{
    std::string input, output, rootName;
    double density = 7850.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--name" && i + 1 < argc)
            rootName = argv[++i];
        else if (a == "--density" && i + 1 < argc)
            density = std::atof(argv[++i]);
        else if (input.empty())
            input = a;
        else if (output.empty())
            output = a;
        else
        {
            std::printf("%s", kUsage);
            return 2;
        }
    }
    if (input.empty() || output.empty() || !(density > 0.0))
    {
        std::printf("%s", kUsage);
        return 2;
    }
    if (rootName.empty())
        rootName = std::filesystem::path(input).stem().string();

    // Static OpenUSD: plugin folders next to the executable (Dist layout) or the development ones.
    const std::filesystem::path usd = std::filesystem::exists(ExeDir() / "usd") ? ExeDir() / "usd" : std::filesystem::path(AIAD_USD_PLUGIN_DIR);
    const std::filesystem::path omni =
        std::filesystem::exists(ExeDir() / "usd/omniSolid/resources") ? ExeDir() / "usd/omniSolid/resources" : std::filesystem::path(AIAD_OMNISOLID_DIR);
    _putenv_s("OMNISOLID_PLUGIN_PATH", omni.string().c_str());
    pxr::PlugRegistry::GetInstance().RegisterPlugins(usd.string());
    kernel::Initialize();

    auto t0 = std::chrono::steady_clock::now();
    step::StepModel model;
    std::string error;
    if (!step::ReadStep(input, model, error))
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("read %s: %d products, %d instances, %zu solids (%.1f s)\n", input.c_str(), model.products, model.instances, model.solids.size(),
                Seconds(t0));

    // ---- Document: materials, one body per instance, one part per solid ----
    t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Document> doc = Document::Create(rootName);
    std::set<std::string> materials;
    for (const step::StepSolid& s : model.solids)
        if (materials.insert(MaterialFor(s.colour)).second)
        {
            Material m;
            m.name = MaterialFor(s.colour);
            m.density = density;
            if (s.colour)
                m.colour = *s.colour;
            if (!doc->DefineMaterial(m, error))
            {
                std::printf("material %s: %s\n", m.name.c_str(), error.c_str());
                return 1;
            }
        }

    const pxr::UsdStageRefPtr& stage = doc->Stage();
    const std::string source = std::filesystem::path(input).filename().string();
    std::map<std::string, std::string> bodyOf;        // instance -> body name (unique prim name)
    std::set<std::string> bodyPrims;
    std::map<pxr::SdfPath, const step::StepSolid*> written;
    std::vector<std::string> failures = model.warnings;
    for (const step::StepSolid& s : model.solids)
    {
        std::string& body = bodyOf[s.instance];
        if (body.empty())
        {
            body = s.instance;
            for (int n = 2; bodyPrims.count(PrimName(body)); ++n)
                body = s.instance + " " + std::to_string(n);
            bodyPrims.insert(PrimName(body));
            if (!doc->DefineBody(body, Json::array({0, 0, 0}), error))
            {
                failures.push_back("body '" + body + "': " + error);
                continue;
            }
        }
        const std::string name = s.count > 1 ? std::format("{} [{}]", s.instance, s.index + 1) : s.instance;
        const pxr::SdfPath bodyPath = doc->BodyPath(body);
        pxr::SdfPath path = bodyPath.AppendChild(pxr::TfToken(PrimName(name)));
        for (int n = 2; stage->GetPrimAtPath(path); ++n)
            path = bodyPath.AppendChild(pxr::TfToken(PrimName(name) + "_" + std::to_string(n)));
        pxr::UsdPrim part = pxr::UsdGeomXform::Define(stage, path).GetPrim();
        const auto set = [&](const char* attribute, const std::string& value) {
            part.CreateAttribute(pxr::TfToken(attribute), pxr::SdfValueTypeNames->String, true).Set(value);
        };
        set("aiad:name", name);
        set("aiad:material", MaterialFor(s.colour));
        set("aiad:role", "");
        set("aiad:error", "");
        part.CreateAttribute(pxr::TfToken("aiad:sectioned"), pxr::SdfValueTypeNames->Bool, true).Set(true);
        const Json features = Json::array({{{"op", "import"},
                                            {"format", "STEP"},
                                            {"file", source},
                                            {"product", s.product},
                                            {"occurrence", s.occurrence},
                                            {"solid", s.index + 1}}});
        set("aiad:features", features.dump(1));

        const pxr::SdfPath shape = path.AppendChild(pxr::TfToken("Shape"));
        if (!step::WriteBrepArray(s.brep, stage, shape, error))
        {
            failures.push_back("'" + name + "': " + error);
            stage->RemovePrim(path);
            continue;
        }
        const pxr::SdfPath look = doc->Root().AppendChild(pxr::TfToken("Looks")).AppendChild(pxr::TfToken(PrimName(MaterialFor(s.colour))));
        pxr::UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(shape)).Bind(pxr::UsdShadeMaterial(stage->GetPrimAtPath(look)));
        written[path] = &s;
    }
    // Bodies left without parts (every solid failed) go too.
    for (const auto& [instance, body] : bodyOf)
        if (pxr::UsdPrim prim = stage->GetPrimAtPath(doc->BodyPath(body)); prim && prim.GetChildren().empty())
            stage->RemovePrim(prim.GetPath());
    if (!doc->Save(output, error))
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s: %zu parts, %zu bodies, %zu materials (%.1f s)\n", output.c_str(), written.size(), doc->Bodies().size(), materials.size(),
                Seconds(t0));

    // ---- Check: reopen as the application does and measure every part with the AIAD kernel ----
    t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Document> check = Document::Open(output, error);
    if (!check)
    {
        std::printf("reopening failed: %s\n", error.c_str());
        return 1;
    }
    int ok = 0;
    kernel::Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    // Volumes [mm^3]: OCCT (source solid) and the AIAD kernel (the converted one).
    std::printf("\n%-44s %6s %6s %11s %11s %8s\n", "part", "faces", "kernel", "OCCT", "AIAD", "diff");
    for (const PartInfo& p : check->Parts())
    {
        const auto it = written.find(p.path);
        const step::StepSolid* s = it == written.end() ? nullptr : it->second;
        const kernel::Solid* solid = check->Shape(p.path);
        kernel::MassProperties mp;
        std::string e;
        if (!s || !solid || !solid->Valid() || !check->Mass(p.path, mp, e))
        {
            failures.push_back("'" + p.name + "': the AIAD kernel cannot read it back" + (e.empty() ? "" : " (" + e + ")"));
            continue;
        }
        const int faces = kernel::FaceCount(*solid);
        const bool closed = kernel::IsManifoldSolid(*solid);
        const double diff = s->volume > 0 ? (mp.volume - s->volume) / s->volume : 0.0;
        const bool good = closed && faces == s->faces && std::abs(diff) < 0.01;
        if (good)
            ++ok;
        else
        {
            // The display mesh tells a wrong conversion (its volume is off too) from a kernel mass integration that
            // drifts (its volume matches OCCT's).
            const kernel::Mesh* mesh = check->DisplayMesh(p.path);
            failures.push_back(std::format("'{}': {}{}{}", p.name, closed ? "" : "not a closed solid; ",
                                           faces == s->faces ? "" : std::format("{} faces instead of {}; ", faces, s->faces),
                                           std::abs(diff) < 0.01 ? ""
                                                                 : std::format("kernel volume off by {:+.1f}% (its display mesh: {:+.1f}%)", diff * 100,
                                                                               mesh ? (MeshVolume(*mesh) - s->volume) / s->volume * 100 : 0.0)));
        }
        std::printf("%-44.44s %6d %6s %11.3f %11.3f %+7.2f%%\n", p.name.c_str(), faces, closed ? "closed" : "OPEN", s->volume * 1e9,
                    mp.volume * 1e9, diff * 100);
        kernel::Vec3 a, b;
        if (check->Bounds(p.path, a, b))
        {
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
            hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
        }
    }
    std::printf("\nsize %.2f x %.2f x %.2f mm; checked in %.1f s\n", (hi.x - lo.x) * 1e3, (hi.y - lo.y) * 1e3, (hi.z - lo.z) * 1e3, Seconds(t0));
    for (const std::string& f : failures)
        std::printf("  ! %s\n", f.c_str());
    std::printf("%d of %zu solids imported and verified\n", ok, model.solids.size());
    return ok == int(model.solids.size()) && model.warnings.empty() ? 0 : 1;
}

int main(int argc, char** argv)
{
    const int code = Run(argc, argv);
    // Static OCCT has no defined destruction order against the other static libraries (its type registry
    // goes before maps that still hold handles): skip the static destructors, everything of ours is released.
    std::fflush(stdout);
    std::_Exit(code);
}
