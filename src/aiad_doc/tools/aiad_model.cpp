// aiad-model.exe - builds a document from a command script (the same Command Bus commands the user
// and the AI run) and reports it.
//
//   aiad-model build <script.json> <out.usda>                  runs the script on a new document, saves it
//   aiad-model apply <document.usda> <script.json> [<out.usda>] runs the script on a document (saved in place)
//   aiad-model info <document.usda>                            parts, masses per body, regeneration errors
//   aiad-model outline <document.usda> <z> <part>...           the parts' outlines on the plane z (model frame, m), as
//                                                              the simulator cuts them for touches
#include "DocCommands.h"
#include "Document.h"
#include "Paths.h"
#include "Text.h"

#include <pxr/base/plug/registry.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using namespace aiad;
using namespace aiad::doc;

static void Report(Document& d)
{
    std::map<std::string, kernel::MassProperties> bodies;
    int failures = 0;
    for (const PartInfo& p : d.Parts())
    {
        kernel::MassProperties mp;
        std::string e;
        if (!p.error.empty() || !d.Mass(p.path, mp, e))
        {
            std::printf("  FAILED %-32s %s\n", p.name.c_str(), (p.error.empty() ? e : p.error).c_str());
            ++failures;
            continue;
        }
        const kernel::Vec3 o = d.BodyOrigin(p.body);
        std::printf("  %-32s %-14s %8.1f g  %8.2f cm^3  centre (%.3f, %.3f, %.3f) mm\n", p.name.c_str(), p.body.c_str(), mp.mass * 1e3, mp.volume * 1e6,
                    (o.x + mp.centroid.x) * 1e3, (o.y + mp.centroid.y) * 1e3, (o.z + mp.centroid.z) * 1e3);
        kernel::MassProperties& b = bodies[p.body];
        b.mass += mp.mass;
        b.volume += mp.volume;
    }
    for (const auto& [name, b] : bodies)
        std::printf("body %-14s %8.3f kg\n", name.c_str(), b.mass);
    std::printf("%zu parts, %d failed\n", d.Parts().size(), failures);
}

int main(int argc, char** argv)
{
    // The arguments as UTF-8: part names may hold any character (an imported "Two–piece"), which the ANSI code page
    // of char** argv cannot.
    std::vector<std::string> utf8;
    std::vector<char*> utf8Argv;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 0; i < argc; ++i)
            utf8.push_back(Narrow(wide[i]));
        LocalFree(wide);
        for (std::string& s : utf8)
            utf8Argv.push_back(s.data());
        argv = utf8Argv.data();
    }
    if (argc < 3)
    {
        std::printf("usage: aiad-model build <script.json> <out.usda> | info <document.usda>\n");
        return 2;
    }
    // Static OpenUSD: plugin folders next to the executable (Dist layout) or the development ones.
    const std::filesystem::path usd = std::filesystem::exists(ExeDir() / "usd") ? ExeDir() / "usd" : std::filesystem::path(AIAD_USD_PLUGIN_DIR);
    const std::filesystem::path omni =
        std::filesystem::exists(ExeDir() / "usd/omniSolid/resources") ? ExeDir() / "usd/omniSolid/resources" : std::filesystem::path(AIAD_OMNISOLID_DIR);
    _putenv_s("OMNISOLID_PLUGIN_PATH", omni.string().c_str());
    pxr::PlugRegistry::GetInstance().RegisterPlugins(usd.string());
    kernel::Initialize();

    const std::string mode = argv[1];
    std::string error;
    if (mode == "outline" && argc >= 5)
    {
        std::unique_ptr<Document> d = Document::Open(argv[2], error);
        if (!d)
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        const double z = std::atof(argv[3]);
        for (int a = 4; a < argc; ++a)
        {
            std::string e;
            const auto part = d->FindPart(argv[a], e);
            if (!part)
            {
                std::printf("no part %s: %s\n", argv[a], e.c_str());
                continue;
            }
            const kernel::Vec3 o = d->BodyOrigin(part->body);
            const auto loops = d->SectionOutline(part->path, {0, 0, z - o.z}, {0, 0, 1});
            std::printf("part %s body %s: %zu loops\n", argv[a], part->body.c_str(), loops.size());
            for (const auto& loop : loops)
            {
                std::printf("loop %zu\n", loop.size());
                for (const auto& p : loop)
                    std::printf("%.7f %.7f\n", p.x + o.x, p.y + o.y);
            }
        }
        return 0;
    }
    if (mode == "info")
    {
        std::unique_ptr<Document> d = Document::Open(argv[2], error);
        if (!d)
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        Report(*d);
        return 0;
    }
    if ((mode != "build" && mode != "apply") || argc < 4)
    {
        std::printf("usage: aiad-model build <script.json> <out.usda> | apply <document.usda> <script.json> [<out.usda>]\n");
        return 2;
    }
    // build: a new document from the script; apply: the script run on an existing document (saved in place,
    // or to <out.usda>).
    const bool apply = mode == "apply";
    const char* scriptPath = apply ? argv[3] : argv[2];
    const char* outPath = apply ? (argc >= 5 ? argv[4] : argv[2]) : argv[3];
    const Json script = ParseJson(ReadTextFile(scriptPath));
    if (script.is_discarded())
    {
        std::printf("%s is not valid JSON\n", scriptPath);
        return 1;
    }
    std::unique_ptr<Document> d = apply ? Document::Open(argv[2], error) : Document::Create(std::filesystem::path(outPath).stem().string());
    if (!d)
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    cmd::CommandBus bus;
    RegisterDocumentCommands(bus, [&] { return d.get(); });
    const auto start = std::chrono::steady_clock::now();
    int steps = 0;
    if (!RunScript(bus, script, error, &steps))
    {
        std::printf("script failed at %s\n", error.c_str());
        return 1;
    }
    std::printf("%d steps in %.1f s\n", steps, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    // Mass and bounds of every part saved with it: the document opens without computing them (seconds for an
    // imported part).
    const auto derived = std::chrono::steady_clock::now();
    for (const PartInfo& p : d->Parts())
    {
        kernel::MassProperties mp;
        kernel::Vec3 lo, hi;
        std::string e;
        d->Mass(p.path, mp, e);
        d->Bounds(p.path, lo, hi);
    }
    std::printf("masses and bounds in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - derived).count());
    if (!apply)
        Report(*d);
    if (!d->Save(outPath, error))
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("saved %s\n", outPath);
    return 0;
}
