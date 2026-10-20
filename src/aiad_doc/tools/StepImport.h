// STEP (AP203 / AP214 / AP242) -> exact solids, through Open CASCADE (the static OCCT built with Netgen) and
// the USD BRep `.brep` importer (OCCT_BREP_IMPORT). The assembly is flattened: every solid of every part
// instance comes out placed in the model frame, in metres, with its product / instance names and colour.
//
// Only StepImport.cpp includes OCCT and USD BRep headers; this header exposes neither.
#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace aiad::step
{

struct StepSolid
{
    std::string product;                        // part name of the STEP product ("210 Third wheel")
    std::string instance;                       // unique name of this copy of the product ("Screw", "Screw (2)")
    std::string occurrence;                     // the file's name for the copy (often an id: "NAUO12")
    int index = 0, count = 1;                   // solid index inside the instance (bodies of a multi-body part)
    std::optional<std::array<double, 3>> colour;   // linear RGB, when the file gives one
    double volume = 0.0;                        // [m^3], measured by OCCT (to compare with the AIAD kernel)
    int faces = 0;
    std::string brep;                           // OCCT ASCII `.brep` of the healed, placed solid
};

struct StepModel
{
    std::vector<StepSolid> solids;
    int products = 0, instances = 0;
    std::vector<std::string> warnings;          // what was skipped or repaired, one line each
};

bool ReadStep(const std::string& path, StepModel& model, std::string& error);

// Converts one solid's `.brep` into a BrepArray prim at `path` (its parent must exist in the root layer).
bool WriteBrepArray(const std::string& brep, const pxr::UsdStageRefPtr& stage, const pxr::SdfPath& path, std::string& error);

}  // namespace aiad::step
