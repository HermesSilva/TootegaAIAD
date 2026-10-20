#include "Document.h"

#include "Features.h"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdPhysics/fixedJoint.h>
#include <pxr/usd/usdPhysics/prismaticJoint.h>
#include <pxr/usd/usdPhysics/revoluteJoint.h>
#include <pxr/usd/usdPhysics/sphericalJoint.h>
#include <pxr/usd/usdPhysics/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace aiad::doc
{

using namespace pxr;

namespace
{

const TfToken kParameters("aiad:parameters"), kMaterials("aiad:materials"), kBody("aiad:body"), kName("aiad:name"),
    kMaterial("aiad:material"), kRole("aiad:role"), kSectioned("aiad:sectioned"), kFeatures("aiad:features"), kError("aiad:error");
const TfToken kShape("Shape"), kImported("Imported"), kSection("Section"), kLooks("Looks"), kBrepArray("BrepArray");
const TfToken kFixed("aiad:fixed"), kJoints("Joints"), kJointType("aiad:type"), kBody0("aiad:body0"), kBody1("aiad:body1"),
    kPoint("aiad:point"), kAxis("aiad:axis"), kMotor("aiad:motor"), kLimits("aiad:limits"), kStart("aiad:start"), kView("aiad:view"), kInstruments("Instruments"), kElements("Elements"),
    kDefinition("aiad:definition");
const std::set<std::string> kJointTypes{"revolute", "prismatic", "cylindrical", "spherical", "planar", "fixed"};

std::string GetString(const UsdPrim& prim, const TfToken& name)
{
    std::string value;
    if (UsdAttribute a = prim.GetAttribute(name))
        a.Get(&value);
    return value;
}

void SetString(UsdPrim& prim, const TfToken& name, const std::string& value)
{
    prim.CreateAttribute(name, SdfValueTypeNames->String, true).Set(value);
}

// Every string inside a JSON value (formulas live in strings).
void Strings(const Json& j, std::vector<std::string>& out)
{
    if (j.is_string())
        out.push_back(j.get<std::string>());
    else if (j.is_array() || j.is_object())
        for (const Json& v : j)
            Strings(v, out);
}

Json ToJson(const Parameter& p)
{
    Json j = {{"name", p.name}, {"value", p.value}, {"unit", p.unit}, {"description", p.description}};
    if (p.minimum)
        j["min"] = *p.minimum;
    if (p.maximum)
        j["max"] = *p.maximum;
    return j;
}

Parameter FromJson(const Json& j)
{
    Parameter p;
    p.name = j.value("name", "");
    p.value = j.value("value", Json(0.0));
    p.unit = j.value("unit", "");
    p.description = j.value("description", "");
    if (j.contains("min") && j["min"].is_number())
        p.minimum = j["min"].get<double>();
    if (j.contains("max") && j["max"].is_number())
        p.maximum = j["max"].get<double>();
    return p;
}

bool ValidName(const std::string& n)
{
    if (n.empty() || !(std::isalpha(uint8_t(n[0])) || n[0] == '_'))
        return false;
    return std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; });
}

}  // namespace

std::string PrimName(const std::string& display)
{
    std::string out;
    bool underscore = false;
    for (const char c : display)
    {
        if (std::isalnum(uint8_t(c)))
        {
            out += c;
            underscore = false;
        }
        else if (!underscore && !out.empty())
        {
            out += '_';
            underscore = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty() || std::isdigit(uint8_t(out[0])))
        out = "P" + out;
    return out;
}

std::unique_ptr<Document> Document::Create(const std::string& rootName)
{
    auto doc = std::unique_ptr<Document>(new Document());
    doc->m_stage = UsdStage::CreateInMemory();
    UsdGeomSetStageMetersPerUnit(doc->m_stage, 1.0);
    UsdGeomSetStageUpAxis(doc->m_stage, UsdGeomTokens->z);
    doc->m_root = SdfPath::AbsoluteRootPath().AppendChild(TfToken(PrimName(rootName)));
    UsdPrim root = UsdGeomXform::Define(doc->m_stage, doc->m_root).GetPrim();
    doc->m_stage->SetDefaultPrim(root);
    SetString(root, kParameters, "[]");
    ++doc->m_parameterRevision;
    SetString(root, kMaterials, "{}");
    ++doc->m_materialRevision;
    return doc;
}

std::unique_ptr<Document> Document::Open(const std::string& path, std::string& error)
{
    auto doc = std::unique_ptr<Document>(new Document());
    doc->m_stage = UsdStage::Open(path);
    if (!doc->m_stage || !doc->m_stage->GetDefaultPrim())
    {
        error = "cannot open the document " + path;
        return nullptr;
    }
    doc->m_path = path;
    doc->m_root = doc->m_stage->GetDefaultPrim().GetPath();
    // Saved as interface + payload (see Save): edited as one layer, the payload's geometry flattened into it.
    if (const SdfPrimSpecHandle root = doc->m_stage->GetRootLayer()->GetPrimAtPath(doc->m_root); root && root->HasPayloads())
    {
        for (const SdfPayload& payload : root->GetPayloadList().GetAddedOrExplicitItems())
            if (const std::filesystem::path geometry = (std::filesystem::path(path).parent_path() / payload.GetAssetPath()).lexically_normal();
                !std::filesystem::exists(geometry))
            {
                error = "the geometry of " + path + " is in " + geometry.string() + ", which is missing (a document is its .usda and its .geom.usdc)";
                return nullptr;
            }
        doc->m_stage = UsdStage::Open(doc->m_stage->Flatten(false));
    }
    // Exact geometry back from the BrepArray prims; a part without readable geometry is regenerated.
    for (const PartInfo& part : doc->Parts())
    {
        std::string e;
        std::vector<kernel::Solid> solids = kernel::ReadFromPrim(doc->m_stage->GetPrimAtPath(part.path.AppendChild(kShape)), e);
        if (solids.size() == 1)
        {
            doc->SetShape(part.path, std::move(solids[0]));
            doc->LoadDerived(part.path);
        }
        else
            doc->Regenerate(part.path, e);
    }
    return doc;
}

std::string Document::View() const
{
    return GetString(m_stage->GetPrimAtPath(m_root), kView);
}

void Document::SetView(const std::string& view)
{
    if (UsdPrim root = m_stage->GetPrimAtPath(m_root))
    {
        const UsdEditContext edit(m_stage, m_stage->GetRootLayer());
        SetString(root, kView, view);
    }
}

bool Document::Save(const std::string& path, std::string& error)
{
    if (OnSaving)
        OnSaving();
    // OpenUSD's asset pattern (interface + payload): the design in a small text layer (parts, features, bodies,
    // joints, elements, parameters) and the exact geometry (BrepArray prims, 95 % of the data) in a binary crate layer
    // beside it (<name>.geom.usdc), brought in by a payload on the root prim. Nothing is approximated.
    const SdfLayerHandle source = m_stage->GetRootLayer();
    const std::filesystem::path file(path);
    const std::string geometryName = file.stem().string() + ".geom.usdc";
    SdfLayerRefPtr design = SdfLayer::CreateAnonymous("design.usda");
    design->TransferContent(source);
    SdfLayerRefPtr geometry = SdfLayer::CreateAnonymous("geometry.usdc");
    std::vector<SdfPath> breps;
    source->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& p) {
        if (p.IsPrimPath())
            if (const SdfPrimSpecHandle spec = source->GetPrimAtPath(p); spec && spec->GetTypeName() == kBrepArray)
                breps.push_back(p);
    });
    for (const SdfPath& b : breps)
    {
        SdfCreatePrimInLayer(geometry, b.GetParentPath());
        if (!SdfCopySpec(source, b, geometry, b))
        {
            error = "cannot copy the geometry of " + b.GetParentPath().GetString();
            return false;
        }
        if (const SdfPrimSpecHandle parent = design->GetPrimAtPath(b.GetParentPath()))
            parent->RemoveNameChild(design->GetPrimAtPath(b));
    }
    if (const SdfPrimSpecHandle root = design->GetPrimAtPath(m_root))
    {
        root->ClearPayloadList();
        if (!breps.empty())
            root->GetPayloadList().Prepend(SdfPayload("./" + geometryName));
    }
    geometry->SetDefaultPrim(m_root.GetNameToken());
    const std::string geometryPath = (file.parent_path() / geometryName).string();
    if (!breps.empty() && !geometry->Export(geometryPath))
    {
        error = "cannot write " + geometryPath;
        return false;
    }
    if (!design->Export(path))
    {
        error = "cannot write " + path;
        return false;
    }
    m_path = path;
    return true;
}

void Document::Changed()
{
    // Body rest placements are formulas: keep them in step with the parameters.
    for (const UsdPrim& body : m_stage->GetPrimAtPath(m_root).GetChildren())
        if (body.HasAttribute(TfToken("aiad:origin")))
        {
            const kernel::Vec3 o = BodyOrigin(GetString(body, kBody));
            UsdGeomXform xform(body);
            bool reset = false;
            for (const UsdGeomXformOp& op : xform.GetOrderedXformOps(&reset))
                if (op.GetOpName() == TfToken("xformOp:translate:rest"))
                    op.Set(GfVec3d(o.x, o.y, o.z));
        }
    // Joints follow the formulas too.
    if (const UsdPrim joints = m_stage->GetPrimAtPath(m_root.AppendChild(kJoints)))
        for (UsdPrim prim : joints.GetChildren())
            if (prim.HasAttribute(kJointType))
                if (std::optional<JointInfo> joint = FindJoint(GetString(prim, kName)))
                    WriteJointPhysics(prim, *joint);
    ++m_revision;
    if (OnChanged)
        OnChanged();
}

// ---- Parameters ---------------------------------------------------------------------------------------

std::vector<Parameter> Document::Parameters() const
{
    CacheParameters();
    return m_parameterList;
}

void Document::WriteParameters(const std::vector<Parameter>& parameters)
{
    Json j = Json::array();
    for (const Parameter& p : parameters)
        j.push_back(ToJson(p));
    UsdPrim root = m_stage->GetPrimAtPath(m_root);
    SetString(root, kParameters, j.dump(1));
    ++m_parameterRevision;
}

std::optional<Parameter> Document::FindParameter(const std::string& name) const
{
    for (const Parameter& p : Parameters())
        if (p.name == name)
            return p;
    return std::nullopt;
}

// Evaluates a parameter list (formulas over each other), detecting cycles; memoises the values.
struct ParameterSolver
{
    std::map<std::string, Json> values;
    std::map<std::string, double> done;
    std::set<std::string> busy;
    std::string error;

    bool Get(const std::string& name, double& out)
    {
        if (auto it = done.find(name); it != done.end())
        {
            out = it->second;
            return true;
        }
        auto it = values.find(name);
        if (it == values.end())
            return false;
        if (!busy.insert(name).second)
        {
            if (error.empty())
                error = "parameter '" + name + "' depends on itself";
            return false;
        }
        bool ok = false;
        if (it->second.is_number())
        {
            out = it->second.get<double>();
            ok = true;
        }
        else if (it->second.is_string())
        {
            std::string e;
            ok = Evaluate(it->second.get<std::string>(), [this](const std::string& n, double& v) { return Get(n, v); }, out, e);
            if (!ok && error.empty())
                error = "parameter '" + name + "': " + e;
        }
        else if (error.empty())
            error = "parameter '" + name + "' has no value";
        busy.erase(name);
        if (ok)
            done[name] = out;
        return ok;
    }
};

void Document::CacheParameters() const
{
    if (m_cachedParameters == m_parameterRevision)
        return;
    m_parameterList.clear();
    const Json j = ParseJson(GetString(m_stage->GetPrimAtPath(m_root), kParameters));
    if (j.is_array())
        for (const Json& p : j)
            m_parameterList.push_back(FromJson(p));
    m_solver = std::make_shared<ParameterSolver>();
    for (const Parameter& p : m_parameterList)
        m_solver->values[p.name] = p.value;
    m_cachedParameters = m_parameterRevision;
}

bool Document::Value(const std::string& name, double& value, std::string& error) const
{
    CacheParameters();
    if (m_solver->values.contains(name) && m_solver->Get(name, value))
        return true;
    // The message comes from a fresh evaluation (the shared one keeps the first error it met).
    ParameterSolver solver;
    solver.values = m_solver->values;
    if (!solver.values.contains(name))
    {
        error = "unknown parameter '" + name + "'";
        return false;
    }
    if (!solver.Get(name, value))
    {
        error = solver.error;
        return false;
    }
    return true;
}

double Document::Value(const std::string& name) const
{
    double v = 0;
    std::string e;
    return Value(name, v, e) ? v : 0.0;
}

NameLookup Document::Lookup() const
{
    // The cached solver: each parameter is evaluated once per change of the parameter table.
    CacheParameters();
    return [solver = m_solver](const std::string& name, double& value) { return solver->Get(name, value); };
}

std::vector<SdfPath> Document::Dependents(const std::set<std::string>& names) const
{
    // Close over the parameters whose formulas use the changed names.
    std::set<std::string> affected = names;
    const std::vector<Parameter> parameters = Parameters();
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const Parameter& p : parameters)
            if (p.value.is_string() && !affected.contains(p.name))
                for (const std::string& n : ReferencedNames(p.value.get<std::string>()))
                    if (affected.contains(n))
                    {
                        affected.insert(p.name);
                        grew = true;
                        break;
                    }
    }
    std::vector<SdfPath> parts;
    for (const PartInfo& part : Parts())
    {
        std::vector<std::string> strings;
        Strings(part.features, strings);
        bool uses = false;
        for (const std::string& s : strings)
        {
            for (const std::string& n : ReferencedNames(s))
                uses = uses || affected.contains(n);
            if (uses)
                break;
        }
        if (uses)
            parts.push_back(part.path);
    }
    return parts;
}

bool Document::SetParameters(const Json& values, std::string& error, std::vector<std::string>* regenerated)
{
    if (!values.is_object() || values.empty())
    {
        error = "no parameter values given";
        return false;
    }
    const std::vector<Parameter> before = Parameters();
    std::vector<Parameter> after = before;
    std::set<std::string> changed;
    for (const auto& [name, value] : values.items())
    {
        auto it = std::find_if(after.begin(), after.end(), [&](const Parameter& p) { return p.name == name; });
        if (it == after.end())
        {
            error = "unknown parameter '" + name + "' (aiad_parameters_get lists them)";
            return false;
        }
        if (!value.is_number() && !value.is_string())
        {
            error = "parameter '" + name + "' needs a number or a formula";
            return false;
        }
        it->value = value;
        changed.insert(name);
    }
    // Every parameter must still evaluate, and within its range.
    ParameterSolver solver;
    for (const Parameter& p : after)
        solver.values[p.name] = p.value;
    for (const Parameter& p : after)
    {
        double v = 0;
        if (!solver.Get(p.name, v))
        {
            error = solver.error.empty() ? "parameter '" + p.name + "' cannot be evaluated" : solver.error;
            return false;
        }
        if ((p.minimum && v < *p.minimum - 1e-12) || (p.maximum && v > *p.maximum + 1e-12))
        {
            error = std::format("{} = {:g} {} is outside its range [{:g}, {:g}]", p.name, v, p.unit, p.minimum.value_or(-1e300), p.maximum.value_or(1e300));
            return false;
        }
    }
    WriteParameters(after);
    const std::vector<SdfPath> parts = Dependents(changed);
    for (const SdfPath& path : parts)
    {
        std::string e;
        if (!Regenerate(path, e))
        {
            // Atomic: put the old values back and rebuild what was already rebuilt.
            WriteParameters(before);
            for (const SdfPath& p : parts)
            {
                std::string ignore;
                Regenerate(p, ignore);
            }
            error = "not applied: the part '" + ReadPart(m_stage->GetPrimAtPath(path)).name + "' cannot be built with these values (" + e + ")";
            return false;
        }
        if (regenerated)
            regenerated->push_back(ReadPart(m_stage->GetPrimAtPath(path)).name);
    }
    Changed();
    return true;
}

bool Document::DefineParameter(const Parameter& parameter, std::string& error)
{
    if (!ValidName(parameter.name))
    {
        error = "'" + parameter.name + "' is not a valid parameter name (letters, digits, underscore; not starting with a digit)";
        return false;
    }
    std::vector<Parameter> list = Parameters();
    auto it = std::find_if(list.begin(), list.end(), [&](const Parameter& p) { return p.name == parameter.name; });
    if (it != list.end())
        *it = parameter;
    else
        list.push_back(parameter);
    ParameterSolver solver;
    for (const Parameter& p : list)
        solver.values[p.name] = p.value;
    double v = 0;
    if (!solver.Get(parameter.name, v))
    {
        error = solver.error;
        return false;
    }
    const std::vector<Parameter> before = Parameters();
    WriteParameters(list);
    for (const SdfPath& path : Dependents({parameter.name}))
    {
        std::string e;
        if (!Regenerate(path, e))
        {
            WriteParameters(before);
            Regenerate(path, e);
            error = "not applied: " + e;
            return false;
        }
    }
    Changed();
    return true;
}

bool Document::RemoveParameter(const std::string& name, std::string& error)
{
    if (!Dependents({name}).empty())
    {
        error = "parameter '" + name + "' is used by parts; change them first";
        return false;
    }
    std::vector<Parameter> list = Parameters();
    for (const Parameter& p : list)
        if (p.value.is_string() && ReferencedNames(p.value.get<std::string>()).contains(name))
        {
            error = "parameter '" + name + "' is used by parameter '" + p.name + "'";
            return false;
        }
    std::erase_if(list, [&](const Parameter& p) { return p.name == name; });
    WriteParameters(list);
    Changed();
    return true;
}

// ---- Materials --------------------------------------------------------------------------------------

std::vector<Material> Document::Materials() const
{
    if (m_cachedMaterials == m_materialRevision)
        return m_materialList;
    std::vector<Material> list;
    const Json j = ParseJson(GetString(m_stage->GetPrimAtPath(m_root), kMaterials));
    if (j.is_object())
        for (const auto& [name, m] : j.items())
        {
            Material mat;
            mat.name = name;
            mat.density = m.value("density", 1000.0);
            if (m.contains("colour") && m["colour"].is_array() && m["colour"].size() == 3)
                mat.colour = {m["colour"][0].get<double>(), m["colour"][1].get<double>(), m["colour"][2].get<double>()};
            mat.metallic = m.value("metallic", 0.0);
            mat.roughness = m.value("roughness", 0.5);
            mat.emission = m.value("emission", 0.0);
            mat.shearModulus = m.value("shear_modulus", 0.0);
            mat.elasticModulus = m.value("elastic_modulus", 0.0);
            mat.remanence = m.value("remanence", 0.0);
            mat.relativePermeability = m.value("relative_permeability", 0.0);
            list.push_back(mat);
        }
    m_materialList = list;
    m_cachedMaterials = m_materialRevision;
    return list;
}

std::optional<Material> Document::FindMaterial(const std::string& name) const
{
    for (const Material& m : Materials())
        if (m.name == name)
            return m;
    return std::nullopt;
}

bool Document::DefineMaterial(const Material& material, std::string& error)
{
    if (material.name.empty() || material.density < 0)
    {
        error = "a material needs a name and a non-negative density";
        return false;
    }
    UsdPrim root = m_stage->GetPrimAtPath(m_root);
    Json j = ParseJson(GetString(root, kMaterials));
    if (!j.is_object())
        j = Json::object();
    j[material.name] = {{"density", material.density},
                        {"colour", {material.colour[0], material.colour[1], material.colour[2]}},
                        {"metallic", material.metallic},
                        {"roughness", material.roughness},
                        {"emission", material.emission}};
    for (const auto& [key, value] : {std::pair<const char*, double>{"shear_modulus", material.shearModulus}, {"elastic_modulus", material.elasticModulus},
                                     {"remanence", material.remanence},
                                     {"relative_permeability", material.relativePermeability}})
        if (value > 0.0)
            j[material.name][key] = value;
    SetString(root, kMaterials, j.dump(1));
    ++m_materialRevision;
    WriteMaterialPrims();
    Changed();
    return true;
}

void Document::WriteMaterialPrims()
{
    for (const Material& m : Materials())
    {
        const SdfPath path = m_root.AppendChild(kLooks).AppendChild(TfToken(PrimName(m.name)));
        UsdShadeMaterial material = UsdShadeMaterial::Define(m_stage, path);
        UsdShadeShader shader = UsdShadeShader::Define(m_stage, path.AppendChild(TfToken("Surface")));
        shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        const GfVec3f colour(float(m.colour[0]), float(m.colour[1]), float(m.colour[2]));
        shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(colour);
        shader.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(float(m.metallic));
        shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(float(m.roughness));
        shader.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set(colour * float(m.emission));
        material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
    }
}

// ---- Parts --------------------------------------------------------------------------------------------

std::vector<std::string> Document::Bodies() const
{
    std::vector<std::string> list;
    for (const UsdPrim& child : m_stage->GetPrimAtPath(m_root).GetChildren())
        if (child.HasAttribute(kBody))
            list.push_back(GetString(child, kBody));
    return list;
}

SdfPath Document::BodyPath(const std::string& body) const
{
    return m_root.AppendChild(TfToken(PrimName(body)));
}

bool Document::BodyFixed(const std::string& body) const
{
    bool fixed = false;
    if (UsdAttribute a = m_stage->GetPrimAtPath(BodyPath(body)).GetAttribute(kFixed))
        a.Get(&fixed);
    return fixed;
}

bool Document::DefineBody(const std::string& body, const Json& origin, std::string& error, std::optional<bool> fixed)
{
    if (body.empty() || !origin.is_array() || origin.size() != 3)
    {
        error = "a body needs a name and an origin [x, y, z]";
        return false;
    }
    NameLookup lookup = Lookup();
    double v[3];
    for (int i = 0; i < 3; ++i)
    {
        std::string e;
        const Json& c = origin[size_t(i)];
        if (c.is_number())
            v[i] = c.get<double>();
        else if (!c.is_string() || !Evaluate(c.get<std::string>(), lookup, v[i], e))
        {
            error = "body origin: " + (e.empty() ? std::string("expected numbers or formulas") : e);
            return false;
        }
    }
    UsdGeomXform xform = UsdGeomXform::Define(m_stage, BodyPath(body));
    UsdPrim prim = xform.GetPrim();
    SetString(prim, kBody, body);
    SetString(prim, TfToken("aiad:origin"), origin.dump());
    if (fixed)
        prim.CreateAttribute(kFixed, SdfValueTypeNames->Bool, true).Set(*fixed);
    // The rest placement is the body's first transform op; the simulation adds its motion after it (in
    // the session layer, whose op order then wins: an existing body only gets the new rest value).
    if (UsdAttribute rest = prim.GetAttribute(TfToken("xformOp:translate:rest")))
        rest.Set(GfVec3d(v[0], v[1], v[2]));
    else
    {
        xform.ClearXformOpOrder();
        xform.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("rest")).Set(GfVec3d(v[0], v[1], v[2]));
    }
    Changed();
    return true;
}

// ---- Joints -------------------------------------------------------------------------------------------

SdfPath Document::JointPath(const std::string& name) const
{
    return m_root.AppendChild(kJoints).AppendChild(TfToken(PrimName(name)));
}

bool Document::EvaluateVector(const Json& vector, kernel::Vec3& out, std::string& error) const
{
    if (!vector.is_array() || vector.size() != 3)
    {
        error = "expected [x, y, z]";
        return false;
    }
    NameLookup lookup = Lookup();
    double v[3];
    for (int i = 0; i < 3; ++i)
    {
        std::string e;
        const Json& c = vector[size_t(i)];
        if (c.is_number())
            v[i] = c.get<double>();
        else if (!c.is_string() || !Evaluate(c.get<std::string>(), lookup, v[i], e))
        {
            error = e.empty() ? std::string("expected numbers or formulas") : e;
            return false;
        }
    }
    out = {v[0], v[1], v[2]};
    return true;
}

std::vector<JointInfo> Document::Joints() const
{
    std::vector<JointInfo> list;
    const UsdPrim joints = m_stage->GetPrimAtPath(m_root.AppendChild(kJoints));
    if (!joints)
        return list;
    for (const UsdPrim& prim : joints.GetChildren())
    {
        if (!prim.HasAttribute(kJointType))
            continue;
        JointInfo j;
        j.name = GetString(prim, kName);
        j.type = GetString(prim, kJointType);
        j.body0 = GetString(prim, kBody0);
        j.body1 = GetString(prim, kBody1);
        j.point = ParseJson(GetString(prim, kPoint));
        j.axis = ParseJson(GetString(prim, kAxis));
        const Json motor = ParseJson(GetString(prim, kMotor));
        j.motor = motor.is_object() ? motor : Json();
        const Json limits = ParseJson(GetString(prim, kLimits));
        j.limits = limits.is_array() ? limits : Json();
        const Json start = ParseJson(GetString(prim, kStart));
        j.start = start.is_discarded() ? Json() : start;
        list.push_back(std::move(j));
    }
    return list;
}

std::optional<JointInfo> Document::FindJoint(const std::string& name) const
{
    for (JointInfo& j : Joints())
        if (j.name == name)
            return j;
    return std::nullopt;
}

bool Document::DefineJoint(const JointInfo& joint, std::string& error)
{
    const std::vector<std::string> bodies = Bodies();
    auto known = [&](const std::string& b) { return std::find(bodies.begin(), bodies.end(), b) != bodies.end(); };
    if (joint.name.empty())
        error = "a joint needs a name";
    else if (!kJointTypes.contains(joint.type))
        error = "unknown joint type '" + joint.type + "' (revolute, prismatic, cylindrical, spherical, planar, fixed)";
    else if (!known(joint.body0))
        error = "unknown body '" + joint.body0 + "' (aiad_body_define creates bodies)";
    else if (!joint.body1.empty() && !known(joint.body1))
        error = "unknown body '" + joint.body1 + "' (leave body1 empty for the ground)";
    else if (joint.body0 == joint.body1)
        error = "a joint connects two different bodies";
    if (!error.empty())
        return false;
    kernel::Vec3 point, axis;
    std::string e;
    if (!EvaluateVector(joint.point, point, e))
    {
        error = "joint point: " + e;
        return false;
    }
    if (!EvaluateVector(joint.axis, axis, e))
    {
        error = "joint axis: " + e;
        return false;
    }
    if (std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z) < 1e-12)
    {
        error = "the joint axis is zero";
        return false;
    }
    if (!joint.motor.is_null())
    {
        const std::string mode = joint.motor.is_object() ? joint.motor.value("mode", "") : "";
        double value = 0;
        if (mode != "speed" && mode != "torque")
            error = "motor: {\"mode\": \"speed\" | \"torque\", \"value\": number or formula}";
        else if (joint.type != "revolute" && joint.type != "prismatic" && joint.type != "cylindrical")
            error = "a motor drives a revolute, prismatic or cylindrical joint";
        else if (!joint.motor.contains("value") || !EvaluateNumber(joint.motor["value"], value, e))
            error = "motor value: " + (e.empty() ? std::string("expected a number or a formula") : e);
        for (const char* key : {"free_speed", "duration"})
            if (error.empty() && joint.motor.contains(key) && (mode != "torque" || !EvaluateNumber(joint.motor[key], value, e)))
                error = std::format("motor {}: {}", key, mode != "torque" ? std::string("only a torque motor has one") : e);
        if (!error.empty())
            return false;
    }
    if (!joint.limits.is_null())
    {
        double value = 0;
        if (!joint.limits.is_array() || joint.limits.size() != 2)
            error = "limits: [lower, upper], each a number, a formula or null";
        else if (joint.type != "revolute" && joint.type != "prismatic" && joint.type != "cylindrical")
            error = "limits stop a revolute, prismatic or cylindrical joint";
        for (size_t i = 0; error.empty() && i < 2; ++i)
            if (!joint.limits[i].is_null() && !EvaluateNumber(joint.limits[i], value, e))
                error = std::format("limits[{}]: {}", i, e);
        if (!error.empty())
            return false;
    }
    if (!joint.start.is_null())
    {
        double value = 0;
        const bool rotation = joint.type == "revolute" || joint.type == "cylindrical";
        if (!rotation && joint.type != "prismatic")
            error = "start places a revolute, prismatic or cylindrical joint";
        else if (joint.start.is_object())
        {
            if (!rotation)
                error = "start from the local time turns a revolute or cylindrical joint";
            else if (!joint.start.contains("local_time") || !EvaluateNumber(joint.start["local_time"], value, e) || value == 0.0)
                error = "start: {\"local_time\": period [s] (one turn: 60 a seconds hand, 3600 minutes, 43200 hours; negative turns the "
                        "other way), \"offset\": rad (where the coordinate is at 00:00:00)}" + (e.empty() ? std::string() : ": " + e);
            else if (joint.start.contains("offset") && !EvaluateNumber(joint.start["offset"], value, e))
                error = "start offset: " + e;
        }
        else if (!EvaluateNumber(joint.start, value, e))
            error = "start: a number or formula (rad or m), or {\"local_time\": period [s], \"offset\": rad}: " + e;
        if (!error.empty())
            return false;
    }
    UsdGeomScope::Define(m_stage, m_root.AppendChild(kJoints));
    const SdfPath path = JointPath(joint.name);
    m_stage->RemovePrim(path);   // the schema follows the type
    UsdPrim prim;
    if (joint.type == "revolute")
        prim = UsdPhysicsRevoluteJoint::Define(m_stage, path).GetPrim();
    else if (joint.type == "prismatic")
        prim = UsdPhysicsPrismaticJoint::Define(m_stage, path).GetPrim();
    else if (joint.type == "spherical")
        prim = UsdPhysicsSphericalJoint::Define(m_stage, path).GetPrim();
    else if (joint.type == "fixed")
        prim = UsdPhysicsFixedJoint::Define(m_stage, path).GetPrim();
    else
        prim = UsdPhysicsJoint::Define(m_stage, path).GetPrim();   // cylindrical, planar: generic joint
    SetString(prim, kName, joint.name);
    SetString(prim, kJointType, joint.type);
    SetString(prim, kBody0, joint.body0);
    SetString(prim, kBody1, joint.body1);
    SetString(prim, kPoint, joint.point.dump());
    SetString(prim, kAxis, joint.axis.dump());
    SetString(prim, kMotor, joint.motor.is_null() ? std::string() : joint.motor.dump());
    SetString(prim, kLimits, joint.limits.is_null() ? std::string() : joint.limits.dump());
    SetString(prim, kStart, joint.start.is_null() ? std::string() : joint.start.dump());
    WriteJointPhysics(prim, joint);
    Changed();
    return true;
}

bool Document::DeleteJoint(const std::string& name, std::string& error)
{
    if (!m_stage->GetPrimAtPath(JointPath(name)))
    {
        error = "no joint '" + name + "'";
        return false;
    }
    m_stage->RemovePrim(JointPath(name));
    Changed();
    return true;
}

// UsdPhysics attributes of a joint from its definition (formulas evaluated): bodies, the joint frame in
// each body (point relative to the body's rest origin, X axis along the joint axis).
void Document::WriteJointPhysics(UsdPrim& prim, const JointInfo& joint)
{
    kernel::Vec3 point, axis;
    std::string e;
    if (!EvaluateVector(joint.point, point, e) || !EvaluateVector(joint.axis, axis, e))
        return;
    UsdPhysicsJoint physics(prim);
    physics.CreateBody0Rel().SetTargets({BodyPath(joint.body0)});
    if (!joint.body1.empty())
        physics.CreateBody1Rel().SetTargets({BodyPath(joint.body1)});
    else
        physics.CreateBody1Rel().ClearTargets(true);
    const kernel::Vec3 o0 = BodyOrigin(joint.body0), o1 = joint.body1.empty() ? kernel::Vec3{} : BodyOrigin(joint.body1);
    physics.CreateLocalPos0Attr().Set(GfVec3f(float(point.x - o0.x), float(point.y - o0.y), float(point.z - o0.z)));
    physics.CreateLocalPos1Attr().Set(GfVec3f(float(point.x - o1.x), float(point.y - o1.y), float(point.z - o1.z)));
    const GfQuatd q = GfRotation(GfVec3d(1, 0, 0), GfVec3d(axis.x, axis.y, axis.z)).GetQuat();
    const GfQuatf qf(float(q.GetReal()), GfVec3f(q.GetImaginary()));
    physics.CreateLocalRot0Attr().Set(qf);
    physics.CreateLocalRot1Attr().Set(qf);
    // Stops (UsdPhysics: degrees for revolute joints, distance for prismatic ones).
    double limit[2] = {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    for (size_t i = 0; joint.limits.is_array() && i < 2 && i < joint.limits.size(); ++i)
        if (!joint.limits[i].is_null())
            EvaluateNumber(joint.limits[i], limit[i], e);
    if (UsdPhysicsRevoluteJoint revolute{prim})
    {
        revolute.CreateAxisAttr().Set(UsdPhysicsTokens->x);
        revolute.CreateLowerLimitAttr().Set(float(limit[0] * 180.0 / std::numbers::pi));
        revolute.CreateUpperLimitAttr().Set(float(limit[1] * 180.0 / std::numbers::pi));
    }
    if (UsdPhysicsPrismaticJoint prismatic{prim})
    {
        prismatic.CreateAxisAttr().Set(UsdPhysicsTokens->x);
        prismatic.CreateLowerLimitAttr().Set(float(limit[0]));
        prismatic.CreateUpperLimitAttr().Set(float(limit[1]));
    }
}

bool Document::EvaluateNumber(const Json& value, double& out, std::string& error) const
{
    if (value.is_number())
    {
        out = value.get<double>();
        return true;
    }
    if (value.is_string())
        return Evaluate(value.get<std::string>(), Lookup(), out, error);
    error = "expected a number or a formula, got " + value.dump();
    return false;
}

// ---- Elements -----------------------------------------------------------------------------------------

namespace
{

// Fields of each element type: kind (j joint, p part, b body or "ground", e element, n number or formula,
// v [x, y, z], t true/false, P list of parts, V list of valves {joint, part}) and whether it is required.
struct Field
{
    const char* name;
    char kind;
    bool required;
};
const std::map<std::string, std::vector<Field>> kElementFields = {
    {"gear", {{"joint0", 'j', true}, {"joint1", 'j', true}, {"part0", 'p', true}, {"part1", 'p', true}, {"internal", 't', false}}},
    {"belt", {{"joint0", 'j', true}, {"joint1", 'j', true}, {"part0", 'p', true}, {"part1", 'p', true}, {"belt", 'p', false}}},
    {"rack", {{"joint0", 'j', true}, {"joint1", 'j', true}, {"part0", 'p', true}, {"part1", 'p', false}}},
    {"contact", {{"part", 'p', true}, {"body", 'b', true}, {"point", 'v', true}, {"normal", 'v', true}, {"radius", 'n', true}, {"stiffness", 'n', false}}},
    {"spring",
     {{"part", 'p', true},
      {"body0", 'b', true},
      {"point0", 'v', true},
      {"body1", 'b', true},
      {"point1", 'v', true},
      {"free_length", 'n', true},
      {"active_turns", 'n', false},
      {"damping", 'n', false}}},
    {"load", {{"joint", 'j', true}, {"constant", 'n', false}, {"linear", 'n', false}, {"quadratic", 'n', false}}},
    {"torsion", {{"joint", 'j', true}, {"part", 'p', true}, {"free", 'n', false}, {"damping", 'n', false}, {"holder", 'p', false}}},
    {"touch", {{"part0", 'p', true}, {"part1", 'p', true}, {"at", 'n', false}, {"stiffness", 'n', false}, {"damping", 'n', false}}},
    {"chamber",
     {{"piston", 'j', true},
      {"cylinder", 'p', true},
      {"head", 'p', true},
      {"intake", 'V', true},
      {"exhaust", 'V', false},
      {"ignition", 'e', false},
      {"crank", 'j', false},
      {"intake_pressure", 'n', false},
      {"intake_temperature", 'n', false},
      {"exhaust_pressure", 'n', false},
      {"exhaust_temperature", 'n', false},
      {"air_fuel_ratio", 'n', false},
      {"burn_duration", 'n', false}}},
    {"magneto",
     {{"rotor", 'j', true},
      {"magnet", 'p', true},
      {"shoes", 'P', false},
      {"core", 'P', true},
      {"coil", 'p', false},
      {"points_open", 'n', true},
      {"points_close", 'n', false},
      {"primary_turns", 'n', false},
      {"secondary_turns", 'n', false}}}};

}  // namespace

SdfPath Document::ElementPath(const std::string& name) const
{
    return m_root.AppendChild(kElements).AppendChild(TfToken(PrimName(name)));
}

std::vector<ElementInfo> Document::Elements() const
{
    std::vector<ElementInfo> list;
    if (const UsdPrim elements = m_stage->GetPrimAtPath(m_root.AppendChild(kElements)))
        for (const UsdPrim& prim : elements.GetChildren())
        {
            const Json definition = ParseJson(GetString(prim, kDefinition));
            list.push_back({GetString(prim, kName), GetString(prim, kJointType), definition.is_object() ? definition : Json::object()});
        }
    return list;
}

std::optional<ElementInfo> Document::FindElement(const std::string& name) const
{
    for (ElementInfo& e : Elements())
        if (e.name == name)
            return e;
    return std::nullopt;
}

bool Document::DefineElement(const ElementInfo& element, std::string& error)
{
    const auto type = kElementFields.find(element.type);
    if (element.name.empty())
        error = "an element needs a name";
    else if (type == kElementFields.end())
        error = "unknown element type '" + element.type + "' (gear, belt, rack, contact, spring, torsion, touch, load, chamber, magneto)";
    else if (!element.definition.is_object())
        error = "the definition is an object of fields";
    if (!error.empty())
        return false;
    const std::vector<std::string> bodies = Bodies();
    auto name = [](const Json& v) { return v.is_string() ? v.get<std::string>() : std::string(); };
    auto part = [&](const std::string& field, const Json& v) -> std::string {
        std::string e;
        return FindPart(name(v), e) ? "" : field + ": " + (e.empty() ? std::string("expected a part name") : e);
    };
    auto check = [&](const std::string& field, char kind, const Json& v) -> std::string {
        std::string e;
        double number = 0;
        kernel::Vec3 vector;
        switch (kind)
        {
        case 'j': return FindJoint(name(v)) ? "" : std::format("{}: no joint '{}' (aiad_joints_list)", field, name(v));
        case 'p': return part(field, v);
        case 'b':
            return name(v) == "ground" || std::find(bodies.begin(), bodies.end(), name(v)) != bodies.end()
                       ? ""
                       : std::format("{}: no body '{}' (or 'ground')", field, name(v));
        case 'e': return name(v) != element.name && FindElement(name(v)) ? "" : std::format("{}: no element '{}'", field, name(v));
        case 'n': return EvaluateNumber(v, number, e) ? "" : field + ": " + e;
        case 'v': return EvaluateVector(v, vector, e) ? "" : field + ": " + e;
        case 't': return v.is_boolean() ? "" : field + ": expected true or false";
        }
        if (!v.is_array())
            return field + ": expected a list";
        for (size_t i = 0; i < v.size(); ++i)
        {
            const std::string item = std::format("{}[{}]", field, i);
            if (kind == 'P')
            {
                if (std::string m = part(item, v[i]); !m.empty())
                    return m;
            }
            else if (!v[i].is_object() || !v[i].contains("joint") || !v[i].contains("part"))
                return item + ": expected {\"joint\": the valve's joint, \"part\": the valve}";
            else if (!FindJoint(name(v[i]["joint"])))
                return std::format("{}.joint: no joint '{}'", item, name(v[i]["joint"]));
            else if (std::string m = part(item + ".part", v[i]["part"]); !m.empty())
                return m;
        }
        return "";
    };
    for (const auto& [key, value] : element.definition.items())
    {
        const auto field = std::find_if(type->second.begin(), type->second.end(), [&](const Field& f) { return key == f.name; });
        if (field == type->second.end())
        {
            std::string known;
            for (const Field& f : type->second)
                known += (known.empty() ? "" : ", ") + std::string(f.name);
            error = std::format("a {} has no field '{}' (its fields: {})", element.type, key, known);
            return false;
        }
        if (error = check(key, field->kind, value); !error.empty())
            return false;
    }
    for (const Field& f : type->second)
        if (f.required && !element.definition.contains(f.name))
        {
            error = std::format("a {} needs '{}'", element.type, f.name);
            return false;
        }
    UsdGeomScope::Define(m_stage, m_root.AppendChild(kElements));
    UsdPrim prim = UsdGeomScope::Define(m_stage, ElementPath(element.name)).GetPrim();
    SetString(prim, kName, element.name);
    SetString(prim, kJointType, element.type);
    SetString(prim, kDefinition, element.definition.dump());
    Changed();
    return true;
}

bool Document::DeleteElement(const std::string& name, std::string& error)
{
    if (!m_stage->GetPrimAtPath(ElementPath(name)))
    {
        error = "no element '" + name + "'";
        return false;
    }
    m_stage->RemovePrim(ElementPath(name));
    Changed();
    return true;
}


// ---- Instruments ----------------------------------------------------------------------------------------------------

std::vector<InstrumentInfo> Document::Instruments() const
{
    std::vector<InstrumentInfo> list;
    if (const UsdPrim scope = m_stage->GetPrimAtPath(m_root.AppendChild(kInstruments)))
        for (const UsdPrim& prim : scope.GetChildren())
        {
            const Json definition = ParseJson(GetString(prim, kDefinition));
            list.push_back({GetString(prim, kName), GetString(prim, kJointType), definition.is_object() ? definition : Json::object()});
        }
    return list;
}

std::optional<InstrumentInfo> Document::FindInstrument(const std::string& name) const
{
    for (InstrumentInfo& i : Instruments())
        if (i.name == name)
            return i;
    return std::nullopt;
}

bool Document::DefineInstrument(const InstrumentInfo& instrument, std::string& error)
{
    if (!ValidateInstrument(*this, instrument, error))
        return false;
    UsdGeomScope::Define(m_stage, m_root.AppendChild(kInstruments));
    UsdPrim prim = UsdGeomScope::Define(m_stage, m_root.AppendChild(kInstruments).AppendChild(TfToken(PrimName(instrument.name)))).GetPrim();
    SetString(prim, kName, instrument.name);
    SetString(prim, kJointType, instrument.type);
    SetString(prim, kDefinition, instrument.definition.dump());
    if (OnInstrumentsChanged)
        OnInstrumentsChanged();
    return true;
}

bool Document::DeleteInstrument(const std::string& name, std::string& error)
{
    const SdfPath path = m_root.AppendChild(kInstruments).AppendChild(TfToken(PrimName(name)));
    if (!m_stage->GetPrimAtPath(path))
    {
        error = "no instrument '" + name + "'";
        return false;
    }
    m_stage->RemovePrim(path);
    if (OnInstrumentsChanged)
        OnInstrumentsChanged();
    return true;
}

Json Document::BodyOriginDefinition(const std::string& body) const
{
    const Json j = ParseJson(GetString(m_stage->GetPrimAtPath(BodyPath(body)), TfToken("aiad:origin")));
    return j.is_array() ? j : Json::array({0, 0, 0});
}

kernel::Vec3 Document::BodyOrigin(const std::string& body) const
{
    const Json o = BodyOriginDefinition(body);
    NameLookup lookup = Lookup();
    double v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i)
    {
        std::string e;
        if (o[size_t(i)].is_number())
            v[i] = o[size_t(i)].get<double>();
        else if (o[size_t(i)].is_string())
            Evaluate(o[size_t(i)].get<std::string>(), lookup, v[i], e);
    }
    return {v[0], v[1], v[2]};
}

bool Document::SetOrientation(const kernel::Vec3& axis, double angleDeg, std::string& error)
{
    UsdGeomXform root(m_stage->GetPrimAtPath(m_root));
    if (!root)
    {
        error = "no root";
        return false;
    }
    root.ClearXformOpOrder();
    const GfRotation rotation(GfVec3d(axis.x, axis.y, axis.z), angleDeg);
    root.AddOrientOp(UsdGeomXformOp::PrecisionDouble).Set(GfQuatd(rotation.GetQuat()));
    Changed();
    return true;
}

PartInfo Document::ReadPart(const UsdPrim& prim) const
{
    PartInfo p;
    p.path = prim.GetPath();
    p.name = GetString(prim, kName);
    p.body = GetString(prim.GetParent(), kBody);
    p.material = GetString(prim, kMaterial);
    p.role = GetString(prim, kRole);
    bool sectioned = false;
    if (UsdAttribute a = prim.GetAttribute(kSectioned))
        a.Get(&sectioned);
    p.sectioned = sectioned;
    p.features = ParseJson(GetString(prim, kFeatures));
    if (p.features.is_discarded())
        p.features = Json::array();
    p.error = GetString(prim, kError);
    return p;
}

void Document::WritePart(UsdPrim& prim, const PartInfo& part)
{
    SetString(prim, kName, part.name);
    SetString(prim, kMaterial, part.material);
    SetString(prim, kRole, part.role);
    prim.CreateAttribute(kSectioned, SdfValueTypeNames->Bool, true).Set(part.sectioned);
    SetString(prim, kFeatures, part.features.dump(1));
}

std::vector<PartInfo> Document::Parts() const
{
    std::vector<PartInfo> list;
    for (const UsdPrim& body : m_stage->GetPrimAtPath(m_root).GetChildren())
        if (body.HasAttribute(kBody))
            for (const UsdPrim& part : body.GetChildren())
                if (part.HasAttribute(kFeatures))
                    list.push_back(ReadPart(part));
    return list;
}

std::optional<PartInfo> Document::FindPart(const std::string& reference, std::string& error) const
{
    const std::vector<PartInfo> parts = Parts();
    std::vector<const PartInfo*> fragment;
    std::string key = reference;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    for (const PartInfo& p : parts)
    {
        if (p.path.GetString() == reference || p.name == reference)
            return p;
        std::string lower = p.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (!key.empty() && lower.find(key) != std::string::npos)
            fragment.push_back(&p);
    }
    if (fragment.size() == 1)
        return *fragment[0];
    if (fragment.empty())
        error = "no part named '" + reference + "' (aiad_parts_list shows the names)";
    else
    {
        error = "'" + reference + "' matches several parts:";
        for (const PartInfo* p : fragment)
            error += " '" + p->name + "'";
    }
    return std::nullopt;
}

bool Document::BuildGeometry(const SdfPath& path, const PartInfo& part, std::string& error)
{
    // An imported part (first feature "import") has no history to rebuild: its solid is the one stored with it,
    // moved by the "rotate"/"translate" features after the import. Once moved, the solid as imported is kept in
    // an "Imported" child, so that moves never pile up.
    const bool imported = part.features.is_array() && !part.features.empty() && part.features[0].value("op", "") == "import";
    kernel::Solid solid;
    if (imported)
    {
        // The view deactivates stored geometry; it is read with the prim active.
        auto read = [&](const SdfPath& child) {
            std::vector<kernel::Solid> solids;
            if (UsdPrim p = m_stage->GetPrimAtPath(child))
            {
                const bool inactive = !p.IsActive();
                if (inactive)
                    p.SetActive(true);
                std::string e;
                solids = kernel::ReadFromPrim(m_stage->GetPrimAtPath(child), e);
                if (inactive)
                    m_stage->GetPrimAtPath(child).SetActive(false);
            }
            return solids;
        };
        const SdfPath shapePath = path.AppendChild(kShape), importedPath = path.AppendChild(kImported);
        const bool kept = bool(m_stage->GetPrimAtPath(importedPath));
        if (!kept && part.features.size() == 1)
        {
            // Never moved: the stored solid is it.
            if (auto it = m_shapes.find(path); it == m_shapes.end() || !it->second.solid.Valid())
            {
                std::vector<kernel::Solid> solids = read(shapePath);
                if (solids.size() != 1)
                {
                    error = "the imported geometry of this part is missing (import it again)";
                    return false;
                }
                SetShape(path, std::move(solids[0]));
                LoadDerived(path);
            }
            const SdfPath look = m_root.AppendChild(kLooks).AppendChild(TfToken(PrimName(part.material)));
            UsdShadeMaterialBindingAPI::Apply(m_stage->GetPrimAtPath(shapePath)).Bind(UsdShadeMaterial(m_stage->GetPrimAtPath(look)));
            return true;
        }
        std::vector<kernel::Solid> original = read(kept ? importedPath : shapePath);
        if (original.size() != 1)
        {
            error = "the imported geometry of this part is missing (import it again)";
            return false;
        }
        if (!kept)
        {
            UsdPrim keep = m_stage->DefinePrim(importedPath, kBrepArray);
            if (!kernel::WriteToPrim({&original[0]}, keep, error))
                return false;
            UsdGeomImageable(keep).MakeInvisible();
        }
        solid = std::move(original[0]);
        if (!MovePart(part.features, 1, Lookup(), solid, error))
            return false;
        if (part.features.size() == 1)   // the moves were taken back: as imported again
            m_stage->RemovePrim(importedPath);
    }
    else
        solid = BuildPart(part.features, Lookup(), error);
    if (!solid.Valid())
        return false;
    if (!WriteShape(path, part.material, solid, error))
        return false;
    SetShape(path, std::move(solid));
    LoadDerived(path);   // still valid when the solid came out the same
    return true;
}

bool Document::WriteShape(const SdfPath& path, const std::string& material, const kernel::Solid& solid, std::string& error)
{
    // Exact geometry in the document (an old "Section" child from earlier versions is dropped).
    m_stage->RemovePrim(path.AppendChild(kShape));
    m_stage->RemovePrim(path.AppendChild(kSection));
    UsdPrim shape = m_stage->DefinePrim(path.AppendChild(kShape), kBrepArray);
    if (!kernel::WriteToPrim({&solid}, shape, error))
        return false;
    const SdfPath look = m_root.AppendChild(kLooks).AppendChild(TfToken(PrimName(material)));
    UsdShadeMaterialBindingAPI::Apply(shape).Bind(UsdShadeMaterial(m_stage->GetPrimAtPath(look)));
    return true;
}


bool Document::CreatePart(PartInfo part, std::string& error)
{
    if (part.name.empty() || part.body.empty())
    {
        error = "a part needs a name and a body";
        return false;
    }
    if (!FindMaterial(part.material))
    {
        error = "unknown material '" + part.material + "' (aiad_materials_list shows them; aiad_material_define adds one)";
        return false;
    }
    for (const PartInfo& p : Parts())
        if (p.name == part.name)
        {
            error = "a part named '" + part.name + "' already exists";
            return false;
        }
    const SdfPath bodyPath = m_root.AppendChild(TfToken(PrimName(part.body)));
    // Prim names drop punctuation ("+Y" and "-Y" meet): a numbered suffix keeps them apart.
    SdfPath path = bodyPath.AppendChild(TfToken(PrimName(part.name)));
    for (int n = 2; m_stage->GetPrimAtPath(path); ++n)
        path = bodyPath.AppendChild(TfToken(PrimName(part.name) + "_" + std::to_string(n)));
    const bool newBody = !m_stage->GetPrimAtPath(bodyPath);
    UsdPrim body = UsdGeomXform::Define(m_stage, bodyPath).GetPrim();
    SetString(body, kBody, part.body);
    UsdPrim prim = UsdGeomXform::Define(m_stage, path).GetPrim();
    WritePart(prim, part);
    // A part moved to another body or renamed (UpdatePart): its stored geometry comes along.
    if (m_carry)
        for (const TfToken& child : {kShape, kImported})
            if (m_carry->GetPrimAtPath(m_carryFrom.AppendChild(child)))
                SdfCopySpec(m_carry, m_carryFrom.AppendChild(child), m_stage->GetRootLayer(), path.AppendChild(child));
    if (!BuildGeometry(path, part, error))
    {
        m_stage->RemovePrim(path);
        if (newBody)
            m_stage->RemovePrim(bodyPath);
        return false;
    }
    SetString(prim, kError, "");
    Changed();
    return true;
}

bool Document::UpdatePart(const SdfPath& path, const PartInfo& part, std::string& error)
{
    UsdPrim prim = m_stage->GetPrimAtPath(path);
    if (!prim || !prim.HasAttribute(kFeatures))
    {
        error = "no part at " + path.GetString();
        return false;
    }
    if (!FindMaterial(part.material))
    {
        error = "unknown material '" + part.material + "'";
        return false;
    }
    const PartInfo before = ReadPart(prim);
    if (part.body != before.body || part.name != before.name)
    {
        // Move/rename: recreate under the new body (the definition is everything), taking its stored geometry along: an
        // imported part has no history to rebuild it from.
        const Json snapshot = Snapshot(path);
        SdfLayerRefPtr carry = SdfLayer::CreateAnonymous("carry.usda");
        for (const TfToken& child : {kShape, kImported})
            if (m_stage->GetRootLayer()->GetPrimAtPath(path.AppendChild(child)))
            {
                SdfCreatePrimInLayer(carry, path);
                SdfCopySpec(m_stage->GetRootLayer(), path.AppendChild(child), carry, path.AppendChild(child));
            }
        if (!DeletePart(path, error))
            return false;
        m_carry = carry;
        m_carryFrom = path;
        const bool created = CreatePart(part, error);
        m_carry = nullptr;
        if (!created)
        {
            std::string ignore;
            Restore(path, snapshot, ignore);
            return false;
        }
        return true;
    }
    WritePart(prim, part);
    if (!BuildGeometry(path, part, error))
    {
        WritePart(prim, before);
        std::string ignore;
        BuildGeometry(path, before, ignore);
        return false;
    }
    Changed();
    return true;
}

bool Document::DeletePart(const SdfPath& path, std::string& error)
{
    UsdPrim prim = m_stage->GetPrimAtPath(path);
    if (!prim || !prim.HasAttribute(kFeatures))
    {
        error = "no part at " + path.GetString();
        return false;
    }
    const SdfPath body = path.GetParentPath();
    m_stage->RemovePrim(path);
    m_shapes.erase(path);
    // An empty body goes too.
    if (UsdPrim b = m_stage->GetPrimAtPath(body); b && b.GetChildren().empty())
        m_stage->RemovePrim(body);
    Changed();
    return true;
}

Json Document::Snapshot(const SdfPath& path) const
{
    UsdPrim prim = m_stage->GetPrimAtPath(path);
    if (!prim || !prim.HasAttribute(kFeatures))
        return Json();
    const PartInfo p = ReadPart(prim);
    return {{"name", p.name}, {"body", p.body}, {"material", p.material}, {"role", p.role}, {"sectioned", p.sectioned}, {"features", p.features}};
}

bool Document::Restore(const SdfPath& path, const Json& snapshot, std::string& error)
{
    if (m_stage->GetPrimAtPath(path) && !DeletePart(path, error))
        return false;
    if (snapshot.is_null())
        return true;
    PartInfo p;
    p.name = snapshot.value("name", "");
    p.body = snapshot.value("body", "");
    p.material = snapshot.value("material", "");
    p.role = snapshot.value("role", "");
    p.sectioned = snapshot.value("sectioned", false);
    p.features = snapshot.value("features", Json::array());
    return CreatePart(p, error);
}

// ---- Geometry ----------------------------------------------------------------------------------------

void Document::SetShape(const SdfPath& path, kernel::Solid solid)
{
    ShapeEntry& entry = m_shapes[path];
    entry.solid = std::move(solid);
    entry.revision = ++m_shapeCounter;
    entry.mesh.reset();
    entry.fineMesh.reset();
    entry.unitMass.reset();
    entry.bounds.reset();
    entry.cuts.clear();
}

const kernel::Solid* Document::Shape(const SdfPath& part) const
{
    auto it = m_shapes.find(part);
    return it == m_shapes.end() ? nullptr : &it->second.solid;
}

bool Document::Bounds(const SdfPath& part, kernel::Vec3& minimum, kernel::Vec3& maximum) const
{
    auto it = m_shapes.find(part);
    if (it == m_shapes.end())
        return false;
    if (!it->second.bounds)
    {
        kernel::Vec3 lo, hi;
        if (!kernel::BoundingBox(it->second.solid, lo, hi))
            return false;
        it->second.bounds = std::make_pair(lo, hi);
        StoreDerived(part);
    }
    minimum = it->second.bounds->first;
    maximum = it->second.bounds->second;
    return true;
}

double Document::VolumeBeyond(const SdfPath& part, const kernel::Vec3& point, const kernel::Vec3& normal) const
{
    auto it = m_shapes.find(part);
    if (it == m_shapes.end())
        return 0.0;
    const std::array<double, 6> key{point.x, point.y, point.z, normal.x, normal.y, normal.z};
    if (auto c = it->second.cuts.find(key); c != it->second.cuts.end())
        return c->second;
    // KeepBelow keeps the side opposite to its normal.
    std::string error;
    const kernel::Solid beyond = kernel::KeepBelow(it->second.solid, point, {-normal.x, -normal.y, -normal.z}, error);
    kernel::MassProperties mp;
    const double volume = beyond.Valid() && kernel::ComputeMass(beyond, 1.0, mp, error) ? mp.volume : 0.0;
    it->second.cuts[key] = volume;
    return volume;
}

uint64_t Document::ShapeRevision(const SdfPath& part) const
{
    auto it = m_shapes.find(part);
    return it == m_shapes.end() ? 0 : it->second.revision;
}

const kernel::Mesh* Document::DisplayMesh(const SdfPath& part) const
{
    auto it = m_shapes.find(part);
    if (it == m_shapes.end())
        return nullptr;
    if (!it->second.mesh)
    {
        auto mesh = std::make_shared<kernel::Mesh>();
        std::string error;
        kernel::Tessellate(it->second.solid, 5e-5, 8.0, *mesh, error);
        it->second.mesh = mesh;
    }
    return it->second.mesh.get();
}

std::vector<std::vector<kernel::Vec3>> Document::SectionOutline(const SdfPath& part, const kernel::Vec3& point, const kernel::Vec3& normal) const
{
    auto it = m_shapes.find(part);
    if (it == m_shapes.end())
        return {};
    if (!it->second.fineMesh)
    {
        kernel::Vec3 lo, hi;
        const double size = Bounds(part, lo, hi) ? std::hypot(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z) : 0.01;
        auto mesh = std::make_shared<kernel::Mesh>();
        std::string error;
        kernel::Tessellate(it->second.solid, std::max(1e-6, size / 5000.0), 4.0, *mesh, error);
        it->second.fineMesh = mesh;
    }
    return kernel::SectionLoops(*it->second.fineMesh, point, normal);
}

namespace
{

const TfToken kDerived("aiad:derived"), kVertexPositions("brep:vertexPoint:point:position");

// Fingerprint of a stored solid: its vertices (any change of its geometry or place moves or adds some). 0: none.
uint64_t ShapeKey(const UsdPrim& shape)
{
    VtVec3dArray points;
    const UsdAttribute a = shape ? shape.GetAttribute(kVertexPositions) : UsdAttribute();
    if (!a || !a.Get(&points) || points.empty())
        return 0;
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    const auto* bytes = reinterpret_cast<const unsigned char*>(points.cdata());
    for (size_t i = 0; i < points.size() * sizeof(GfVec3d); ++i)
        h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

}  // namespace

void Document::LoadDerived(const SdfPath& part)
{
    auto it = m_shapes.find(part);
    const UsdPrim prim = m_stage->GetPrimAtPath(part);
    if (it == m_shapes.end() || !prim)
        return;
    ShapeEntry& entry = it->second;
    entry.key = ShapeKey(m_stage->GetPrimAtPath(part.AppendChild(kShape)));
    const Json j = ParseJson(GetString(prim, kDerived));
    if (!entry.key || !j.is_object() || j.value("key", "") != std::format("{:016x}", entry.key))
        return;
    auto vec = [](const Json& a) { return kernel::Vec3{a[0].get<double>(), a[1].get<double>(), a[2].get<double>()}; };
    if (j.contains("volume") && j.contains("centroid") && j.contains("inertia"))
    {
        kernel::MassProperties mp;
        mp.volume = mp.mass = j["volume"].get<double>();
        mp.area = j.value("area", 0.0);
        mp.centroid = vec(j["centroid"]);
        for (size_t k = 0; k < 6; ++k)
            mp.inertia[k] = j["inertia"][k].get<double>();
        entry.unitMass = mp;
    }
    if (j.contains("bounds"))
        entry.bounds = std::make_pair(vec(j["bounds"][0]), vec(j["bounds"][1]));
}

void Document::StoreDerived(const SdfPath& part) const
{
    auto it = m_shapes.find(part);
    UsdPrim prim = m_stage->GetPrimAtPath(part);
    if (it == m_shapes.end() || !prim || !it->second.key)
        return;
    const ShapeEntry& entry = it->second;
    Json j = {{"key", std::format("{:016x}", entry.key)}};
    if (entry.unitMass)
    {
        const kernel::MassProperties& mp = *entry.unitMass;
        j["volume"] = mp.volume;
        j["area"] = mp.area;
        j["centroid"] = {mp.centroid.x, mp.centroid.y, mp.centroid.z};
        j["inertia"] = mp.inertia;
    }
    if (entry.bounds)
        j["bounds"] = {{entry.bounds->first.x, entry.bounds->first.y, entry.bounds->first.z},
                       {entry.bounds->second.x, entry.bounds->second.y, entry.bounds->second.z}};
    // In the document itself (the view writes in the session layer): saved with it.
    const UsdEditContext root(m_stage, m_stage->GetRootLayer());
    prim.CreateAttribute(kDerived, SdfValueTypeNames->String, true).Set(j.dump());
}

bool Document::Mass(const SdfPath& part, kernel::MassProperties& out, std::string& error) const
{
    const kernel::Solid* solid = Shape(part);
    UsdPrim prim = m_stage->GetPrimAtPath(part);
    if (!solid || !prim)
    {
        error = "no geometry for " + part.GetString();
        return false;
    }
    const ShapeEntry& entry = m_shapes.find(part)->second;
    if (!entry.unitMass)
    {
        kernel::MassProperties unit;
        if (!kernel::ComputeMass(*solid, 1.0, unit, error))
            return false;
        entry.unitMass = unit;
        StoreDerived(part);
    }
    // Mass and inertia scale with the material's density; massless materials (e.g. the spark) stay 0.
    const std::optional<Material> material = FindMaterial(ReadPart(prim).material);
    const double density = std::max(0.0, material ? material->density : 1000.0);
    out = *entry.unitMass;
    out.mass = entry.unitMass->volume * density;
    for (double& i : out.inertia)
        i *= density;
    return true;
}

bool Document::Regenerate(const SdfPath& part, std::string& error)
{
    UsdPrim prim = m_stage->GetPrimAtPath(part);
    if (!prim)
    {
        error = "no part at " + part.GetString();
        return false;
    }
    const PartInfo info = ReadPart(prim);
    const bool ok = BuildGeometry(part, info, error);
    SetString(prim, kError, ok ? std::string() : error);
    return ok;
}

std::map<std::string, std::string> Document::RegenerateAll()
{
    std::map<std::string, std::string> failures;
    WriteMaterialPrims();
    for (const PartInfo& p : Parts())
    {
        std::string e;
        if (!Regenerate(p.path, e))
            failures[p.name] = e;
    }
    Changed();
    return failures;
}


}  // namespace aiad::doc
