// STEP -> placed, healed solids (OCCT) -> BrepArray (USD BRep OCCT_BREP_IMPORT). Compiled as C++17.
#include "StepImport.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <ShapeFix_Shape.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include "OcctFileConverter.h"

#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>

#include <map>
#include <sstream>

namespace aiad::step
{

namespace
{

std::string LabelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) name;
    if (!label.FindAttribute(TDataStd_Name::GetID(), name))
        return {};
    return TCollection_AsciiString(name->Get()).ToCString();   // UTF-8
}

std::optional<std::array<double, 3>> Colour(const Handle(XCAFDoc_ColorTool)& tool, const TDF_Label& label)
{
    Quantity_Color c;
    if (tool->GetColor(label, XCAFDoc_ColorSurf, c) || tool->GetColor(label, XCAFDoc_ColorGen, c))
        return std::array<double, 3>{c.Red(), c.Green(), c.Blue()};
    return std::nullopt;
}

std::optional<std::array<double, 3>> Colour(const Handle(XCAFDoc_ColorTool)& tool, const TopoDS_Shape& shape)
{
    Quantity_Color c;
    if (tool->GetColor(shape, XCAFDoc_ColorSurf, c) || tool->GetColor(shape, XCAFDoc_ColorGen, c))
        return std::array<double, 3>{c.Red(), c.Green(), c.Blue()};
    return std::nullopt;
}

struct Reader
{
    Handle(XCAFDoc_ShapeTool) shapes;
    Handle(XCAFDoc_ColorTool) colours;
    StepModel& model;
    std::map<std::string, int> instanceNames;

    std::string UniqueInstance(std::string name)
    {
        const int n = ++instanceNames[name];
        return n == 1 ? name : name + " (" + std::to_string(n) + ")";
    }

    void Visit(const TDF_Label& label, const TopLoc_Location& parent, std::optional<std::array<double, 3>> colour, std::string instance)
    {
        TDF_Label prototype = label;
        TopLoc_Location location = parent;
        if (XCAFDoc_ShapeTool::IsReference(label))
        {
            XCAFDoc_ShapeTool::GetReferredShape(label, prototype);
            location = parent * XCAFDoc_ShapeTool::GetLocation(label);
            ++model.instances;
            if (const std::string n = LabelName(label); !n.empty())
                instance = n;
        }
        // The instance's colour wins over its product's, which wins over the parent's.
        if (auto c = Colour(colours, label))
            colour = c;
        else if (auto p = Colour(colours, prototype))
            colour = p;
        const std::string product = LabelName(prototype);
        if (XCAFDoc_ShapeTool::IsAssembly(prototype))
        {
            TDF_LabelSequence components;
            XCAFDoc_ShapeTool::GetComponents(prototype, components);
            for (Standard_Integer i = 1; i <= components.Length(); ++i)
                Visit(components.Value(i), location, colour, {});
            return;
        }
        ++model.products;
        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(prototype);
        std::vector<TopoDS_Shape> solids;
        for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next())
            solids.push_back(e.Current());
        // Occurrence names are often ids (SolidWorks: "NAUO12"): copies are named after their product.
        const std::string name = UniqueInstance(product.empty() ? instance : product);
        if (solids.empty())
        {
            model.warnings.push_back("'" + name + "': no solid (surfaces or wires only), skipped");
            return;
        }
        for (size_t k = 0; k < solids.size(); ++k)
            Add(solids[k], location, colour, product, name, instance, int(k), int(solids.size()));
    }

    void Add(const TopoDS_Shape& solid, const TopLoc_Location& location, std::optional<std::array<double, 3>> colour, const std::string& product,
             const std::string& instance, const std::string& occurrence, int index, int count)
    {
        const std::string where = "'" + instance + "'" + (count > 1 ? " solid " + std::to_string(index + 1) : "");
        if (auto c = Colour(colours, solid))   // a colour on the solid itself (multi-body parts)
            colour = c;
        else if (!colour)
            for (TopExp_Explorer f(solid, TopAbs_FACE); f.More() && !colour; f.Next())
                colour = Colour(colours, f.Current());
        // Placement baked into the geometry (the BrepArray has no instance transform), then healing: STEP
        // files often lack p-curves and carry small gaps the importer would reject.
        BRepBuilderAPI_Transform placed(solid, location.Transformation(), Standard_True);
        if (!placed.IsDone())
        {
            model.warnings.push_back(where + ": placement failed, skipped");
            return;
        }
        ShapeFix_Shape fix(placed.Shape());
        fix.Perform();
        TopoDS_Shape healed = fix.Shape();
        int solidsAfter = 0;
        for (TopExp_Explorer e(healed, TopAbs_SOLID); e.More(); e.Next())
            ++solidsAfter;
        if (solidsAfter != 1)
        {
            model.warnings.push_back(where + ": healing gave " + std::to_string(solidsAfter) + " solids, skipped");
            return;
        }
        if (healed.ShapeType() != TopAbs_SOLID)
            healed = TopExp_Explorer(healed, TopAbs_SOLID).Current();

        StepSolid s;
        s.product = product;
        s.instance = instance;
        s.occurrence = occurrence;
        s.index = index;
        s.count = count;
        s.colour = colour;
        GProp_GProps props;
        // Adaptive integration (relative error 1e-6): the fixed Gauss rule misses on trimmed B-spline faces.
        BRepGProp::VolumeProperties(healed, props, 1e-6, Standard_True);
        s.volume = props.Mass();
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(healed, TopAbs_FACE, faces);
        s.faces = faces.Extent();
        std::ostringstream out;
        BRepTools::Write(healed, out, Standard_False, Standard_False, TopTools_FormatVersion_CURRENT);
        s.brep = out.str();
        model.solids.push_back(std::move(s));
    }
};

}  // namespace

bool ReadStep(const std::string& path, StepModel& model, std::string& error)
{
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    // The document works in metres: the reader scales the file's unit to it.
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0);
    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_False);
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone)
    {
        error = "cannot read the STEP file " + path;
        return false;
    }
    if (!reader.Transfer(doc))
    {
        error = "the STEP file " + path + " has no transferable shape";
        return false;
    }
    Reader r{XCAFDoc_DocumentTool::ShapeTool(doc->Main()), XCAFDoc_DocumentTool::ColorTool(doc->Main()), model, {}};
    TDF_LabelSequence roots;
    r.shapes->GetFreeShapes(roots);
    for (Standard_Integer i = 1; i <= roots.Length(); ++i)
        r.Visit(roots.Value(i), TopLoc_Location(), std::nullopt, LabelName(roots.Value(i)));
    app->Close(doc);
    if (model.solids.empty())
    {
        error = "the STEP file " + path + " has no solid";
        return false;
    }
    return true;
}

bool WriteBrepArray(const std::string& brep, const pxr::UsdStageRefPtr& stage, const pxr::SdfPath& path, std::string& error)
{
    // The importer names its prim itself (<root>/brepArray0): convert in a scratch stage, copy it in place.
    const pxr::UsdStageRefPtr scratch = pxr::UsdStage::CreateInMemory();
    const pxr::SdfPath root("/Import");
    scratch->DefinePrim(root);
    if (!occt::ConvertOcctDataToUsd(brep, scratch, root, error))
        return false;
    const pxr::SdfPath converted = root.AppendChild(pxr::TfToken("brepArray0"));
    if (!scratch->GetPrimAtPath(converted))
    {
        error = "the importer wrote no BrepArray";
        return false;
    }
    if (!pxr::SdfCopySpec(scratch->GetRootLayer(), converted, stage->GetRootLayer(), path))
    {
        error = "cannot copy the BrepArray to " + path.GetString();
        return false;
    }
    return true;
}

}  // namespace aiad::step
