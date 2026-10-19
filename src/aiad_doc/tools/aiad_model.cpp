// aiad-model.exe - builds a document from a command script (the same Command Bus commands the user
// and the AI run) and reports it.
//
//   aiad-model build <script.json> <out.usda>     runs the script, saves the document
//   aiad-model info <document.usda>               parts, masses per body, regeneration errors
#include "DocCommands.h"
#include "Document.h"
#include "Paths.h"

#include <pxr/base/plug/registry.h>

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
        std::printf("  %-32s %-14s %8.1f g  %8.2f cm^3\n", p.name.c_str(), p.body.c_str(), mp.mass * 1e3, mp.volume * 1e6);
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
    if (mode != "build" || argc < 4)
    {
        std::printf("usage: aiad-model build <script.json> <out.usda>\n");
        return 2;
    }
    const Json script = ParseJson(ReadTextFile(argv[2]));
    if (script.is_discarded())
    {
        std::printf("%s is not valid JSON\n", argv[2]);
        return 1;
    }
    const std::string root = std::filesystem::path(argv[3]).stem().string();
    std::unique_ptr<Document> d = Document::Create(root);
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
    Report(*d);
    if (!d->Save(argv[3], error))
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("saved %s\n", argv[3]);
    return 0;
}
