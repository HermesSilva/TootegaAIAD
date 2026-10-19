#include "Document.h"

#include "Features.h"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <format>
#include <set>

namespace aiad::doc
{

using namespace pxr;

namespace
{

const TfToken kParameters("aiad:parameters"), kMaterials("aiad:materials"), kBody("aiad:body"), kName("aiad:name"),
    kMaterial("aiad:material"), kRole("aiad:role"), kSectioned("aiad:sectioned"), kFeatures("aiad:features"), kError("aiad:error");
const TfToken kShape("Shape"), kSection("Section"), kLooks("Looks"), kBrepArray("BrepArray");

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
    SetString(root, kMaterials, "{}");
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
    // Exact geometry back from the BrepArray prims; a part without readable geometry is regenerated.
    for (const PartInfo& part : doc->Parts())
    {
        std::string e;
        std::vector<kernel::Solid> solids = kernel::ReadFromPrim(doc->m_stage->GetPrimAtPath(part.path.AppendChild(kShape)), e);
        if (solids.size() == 1)
        {
            doc->m_shapes[part.path] = std::move(solids[0]);
        }
        else
            doc->Regenerate(part.path, e);
    }
    return doc;
}

bool Document::Save(const std::string& path, std::string& error)
{
    if (!m_stage->GetRootLayer()->Export(path))
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
    ++m_revision;
    if (OnChanged)
        OnChanged();
}

// ---- Parameters ---------------------------------------------------------------------------------------

std::vector<Parameter> Document::Parameters() const
{
    std::vector<Parameter> list;
    const Json j = ParseJson(GetString(m_stage->GetPrimAtPath(m_root), kParameters));
    if (j.is_array())
        for (const Json& p : j)
            list.push_back(FromJson(p));
    return list;
}

void Document::WriteParameters(const std::vector<Parameter>& parameters)
{
    Json j = Json::array();
    for (const Parameter& p : parameters)
        j.push_back(ToJson(p));
    UsdPrim root = m_stage->GetPrimAtPath(m_root);
    SetString(root, kParameters, j.dump(1));
}

std::optional<Parameter> Document::FindParameter(const std::string& name) const
{
    for (const Parameter& p : Parameters())
        if (p.name == name)
            return p;
    return std::nullopt;
}

namespace
{
// Evaluates a parameter list (formulas over each other), detecting cycles.
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
}  // namespace

bool Document::Value(const std::string& name, double& value, std::string& error) const
{
    ParameterSolver solver;
    for (const Parameter& p : Parameters())
        solver.values[p.name] = p.value;
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
    // One solver per lookup: parameters are evaluated once per regeneration.
    auto solver = std::make_shared<ParameterSolver>();
    for (const Parameter& p : Parameters())
        solver->values[p.name] = p.value;
    return [solver](const std::string& name, double& value) { return solver->Get(name, value); };
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
            list.push_back(mat);
        }
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
    SetString(root, kMaterials, j.dump(1));
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

bool Document::DefineBody(const std::string& body, const Json& origin, std::string& error)
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
    // The rest placement is the body's first transform op; the simulation adds its motion after it.
    xform.ClearXformOpOrder();
    xform.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("rest")).Set(GfVec3d(v[0], v[1], v[2]));
    Changed();
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
    kernel::Solid solid = BuildPart(part.features, Lookup(), error);
    if (!solid.Valid())
        return false;
    // Exact geometry in the document (an old "Section" child from earlier versions is dropped).
    m_stage->RemovePrim(path.AppendChild(kShape));
    m_stage->RemovePrim(path.AppendChild(kSection));
    UsdPrim shape = m_stage->DefinePrim(path.AppendChild(kShape), kBrepArray);
    std::string e;
    if (!kernel::WriteToPrim({&solid}, shape, e))
    {
        error = e;
        return false;
    }
    const SdfPath look = m_root.AppendChild(kLooks).AppendChild(TfToken(PrimName(part.material)));
    UsdShadeMaterialBindingAPI::Apply(shape).Bind(UsdShadeMaterial(m_stage->GetPrimAtPath(look)));
    m_shapes[path] = std::move(solid);
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
        // Move/rename: recreate under the new body (the definition is everything).
        const Json snapshot = Snapshot(path);
        if (!DeletePart(path, error))
            return false;
        if (!CreatePart(part, error))
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

const kernel::Solid* Document::Shape(const SdfPath& part) const
{
    auto it = m_shapes.find(part);
    return it == m_shapes.end() ? nullptr : &it->second;
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
    const std::optional<Material> material = FindMaterial(ReadPart(prim).material);
    const double density = material ? material->density : 1000.0;
    if (density <= 0.0)  // massless (e.g. the spark)
    {
        out = kernel::MassProperties();
        return kernel::ComputeMass(*solid, 1.0, out, error) && (out.mass = 0.0, out.inertia = {}, true);
    }
    return kernel::ComputeMass(*solid, density, out, error);
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
