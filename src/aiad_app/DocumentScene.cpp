#include "DocumentScene.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <chrono>
#include <stb_image_write.h>
#include <cmath>

namespace aiad::app
{

using namespace pxr;

namespace
{

const TfToken kDisplay("Display"), kDisplaySection("DisplaySection"), kDisplayFill("DisplayFill");

UsdShadeMaterial ViewMaterial(const UsdStageRefPtr& stage, const SdfPath& path, GfVec3f colour, float roughness, float emission)
{
    UsdShadeMaterial material = UsdShadeMaterial::Define(stage, path);
    UsdShadeShader shader = UsdShadeShader::Define(stage, path.AppendChild(TfToken("Surface")));
    shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(colour);
    shader.CreateInput(TfToken("metallic"), SdfValueTypeNames->Float).Set(0.0f);
    shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(roughness);
    shader.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set(colour * emission);
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
    return material;
}

void WriteMesh(const UsdStageRefPtr& stage, const SdfPath& path, const kernel::Mesh& m, const std::vector<int>& triangles, const SdfPath& material)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, path);
    VtVec3fArray points, normals;
    VtIntArray counts, indices;
    // Each triangle keeps its own corners: the kernel's normals are per face corner.
    for (const int t : triangles)
        for (const int v : m.triangles[size_t(t)])
        {
            indices.push_back(int(points.size()));
            points.push_back(GfVec3f(m.points[size_t(v)][0], m.points[size_t(v)][1], m.points[size_t(v)][2]));
            normals.push_back(GfVec3f(m.normals[size_t(v)][0], m.normals[size_t(v)][1], m.normals[size_t(v)][2]));
        }
    counts.assign(triangles.size(), 3);
    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateNormalsAttr(VtValue(normals));
    mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(UsdShadeMaterial(stage->GetPrimAtPath(material)));
}

std::vector<int> All(const kernel::Mesh& m)
{
    std::vector<int> t(m.triangles.size());
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = int(i);
    return t;
}

}  // namespace

DocumentScene::DocumentScene(doc::Document& document, const std::string& environmentHdr) : m_document(document)
{
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    const SdfPath looks("/View/Looks");
    ViewMaterial(stage, looks.AppendChild(TfToken("SectionFill")), GfVec3f(0.72f, 0.33f, 0.27f), 0.65f, 0.0f);   // CAD section fill
    ViewMaterial(stage, looks.AppendChild(TfToken("Selection")), GfVec3f(1.0f, 0.55f, 0.08f), 0.45f, 0.35f);     // CAD orange
    UsdLuxDomeLight dome = UsdLuxDomeLight::Define(stage, SdfPath("/View/Studio"));
    dome.CreateTextureFileAttr(VtValue(SdfAssetPath(environmentHdr)));
    dome.CreateIntensityAttr(VtValue(1.0f));
    Refresh();
}

void DocumentScene::Refresh()
{
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    // Parts whose solid, material, role and section switch are unchanged keep their display.
    std::map<std::string, ScenePart> previous;
    for (ScenePart& p : m_parts)
        previous.emplace(p.path.GetString(), std::move(p));
    m_parts.clear();
    bool sparkKept = false;
    for (const doc::PartInfo& info : m_document.Parts())
    {
        ScenePart part{info.path, info.name, info.body, info.material, info.role, info.sectioned};
        part.revision = m_document.ShapeRevision(info.path);
        if (auto it = previous.find(info.path.GetString()); it != previous.end())
        {
            ScenePart& old = it->second;
            part.visible = old.visible;
            if (!old.dirty && old.revision == part.revision && old.material == part.material && old.role == part.role && old.sectioned == part.sectioned)
            {
                part.mesh = std::move(old.mesh);
                part.section = std::move(old.section);
                part.dirty = false;
                sparkKept = sparkKept || part.role == "spark";
            }
        }
        m_parts.push_back(std::move(part));
    }
    if (!sparkKept)
        m_sparkScale = UsdAttribute();
    // Body motion op (after the document's rest placement).
    m_motion.clear();
    for (const std::string& body : m_document.Bodies())
    {
        UsdGeomXform xform(stage->GetPrimAtPath(m_document.BodyPath(body)));
        if (!xform)
            continue;
        bool reset = false;
        std::vector<UsdGeomXformOp> ops;
        for (const UsdGeomXformOp& op : xform.GetOrderedXformOps(&reset))
            if (op.GetOpName() == TfToken("xformOp:translate:rest"))
                ops.push_back(op);
        UsdGeomXformOp motion(xform.GetPrim().GetAttribute(TfToken("xformOp:transform:motion")));
        if (!motion)
            motion = xform.AddTransformOp(UsdGeomXformOp::PrecisionDouble, TfToken("motion"));
        ops.push_back(motion);
        xform.SetXformOpOrder(ops);
        m_motion[body] = motion.GetAttr();
        m_motion[body].Set(GfMatrix4d(1.0));
    }
    const int highlight = m_highlight;
    m_highlight = -1;
    SetHighlight(highlight < int(m_parts.size()) ? highlight : -1);
}

bool DocumentScene::Pending() const
{
    return std::any_of(m_parts.begin(), m_parts.end(), [](const ScenePart& p) { return p.dirty; });
}

bool DocumentScene::Update(double budgetMs)
{
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    for (size_t i = 0; i < m_parts.size() && elapsed() < budgetMs; ++i)
    {
        ScenePart& part = m_parts[i];
        if (!part.dirty)
            continue;
        WriteDisplay(part);
        kernel::MassProperties mp;   // warm the mass cache: the study and the panels read it next
        std::string error;
        m_document.Mass(part.path, mp, error);
        ApplySection(part);
        if (int(i) == m_highlight)
            BindLook(int(i), true);
        part.dirty = false;
    }
    m_tessellationSeconds += elapsed() / 1e3;
    return !Pending();
}

void DocumentScene::WriteDisplay(ScenePart& part)
{
    const UsdStageRefPtr& stage = m_document.Stage();
    // The exact geometry stays in the document; the view shows its tessellation.
    for (const char* child : {"Shape", "Section"})
        if (UsdPrim p = stage->GetPrimAtPath(part.path.AppendChild(TfToken(child))))
            p.SetActive(false);
    for (const TfToken& t : {kDisplay, kDisplaySection, kDisplayFill})
        stage->RemovePrim(part.path.AppendChild(t));
    const kernel::Mesh* mesh = m_document.DisplayMesh(part.path);
    if (!mesh)
        return;
    part.mesh = *mesh;
    const SdfPath look = m_document.Root().AppendChild(TfToken("Looks")).AppendChild(TfToken(doc::PrimName(part.material)));
    WriteMesh(stage, part.path.AppendChild(kDisplay), part.mesh, All(part.mesh), look);
    if (part.role == "spark")
    {
        // Always present, switched by scale about its centre: toggling visibility makes the renderer
        // remove and re-create the instance (~110 ms per toggle).
        UsdGeomMesh mesh(stage->GetPrimAtPath(part.path.AppendChild(kDisplay)));
        GfVec3d centre(0.0);
        for (const auto& p : part.mesh.points)
            centre += GfVec3d(p[0], p[1], p[2]);
        centre /= double(std::max<size_t>(part.mesh.points.size(), 1));
        mesh.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("pivot")).Set(centre);
        m_sparkScale = mesh.AddScaleOp().GetAttr();
        mesh.AddTranslateOp(UsdGeomXformOp::PrecisionDouble, TfToken("pivot"), true);
        m_sparkScale.Set(GfVec3f(1e-4f));
    }
    if (!part.sectioned)
        return;
    // Section view (display only): the part's mesh clipped at the plane y = 0 of the mechanism frame,
    // keeping the back half (y >= 0), and the cut closed by the CAD section fill.
    const kernel::Vec3 origin = m_document.BodyOrigin(part.body);
    kernel::Mesh cap;
    kernel::SectionMesh(part.mesh, {-origin.x, -origin.y, -origin.z}, {0, -1, 0}, part.section, cap);
    if (part.section.triangles.empty())
        return;
    auto all = [](const kernel::Mesh& m) {
        std::vector<int> t(m.triangles.size());
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = int(i);
        return t;
    };
    WriteMesh(stage, part.path.AppendChild(kDisplaySection), part.section, all(part.section), look);
    if (!cap.triangles.empty())
        WriteMesh(stage, part.path.AppendChild(kDisplayFill), cap, all(cap), SdfPath("/View/Looks/SectionFill"));
}
int DocumentScene::Find(const std::string& name) const
{
    for (size_t i = 0; i < m_parts.size(); ++i)
        if (m_parts[i].name == name)
            return int(i);
    return -1;
}

void DocumentScene::SetMotion(const std::vector<BodyMotion>& motion)
{
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    for (const BodyMotion& m : motion)
        if (auto it = m_motion.find(m.body); it != m_motion.end())
            it->second.Set(m.transform);
}

void DocumentScene::SetSparkVisible(bool visible)
{
    if (!m_sparkScale)
        return;
    const UsdEditContext session(m_document.Stage(), m_document.Stage()->GetSessionLayer());
    m_sparkScale.Set(GfVec3f(visible ? 1.0f : 1e-4f));
}

void DocumentScene::SetSectionView(bool section)
{
    m_section = section;
    for (const ScenePart& p : m_parts)
        ApplySection(p);
}

void DocumentScene::ApplySection(const ScenePart& p)
{
    if (!p.sectioned)
        return;
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    auto set = [&](const TfToken& child, bool shown) {
        if (UsdPrim prim = stage->GetPrimAtPath(p.path.AppendChild(child)))
            UsdGeomImageable(prim).CreateVisibilityAttr().Set(shown ? UsdGeomTokens->inherited : UsdGeomTokens->invisible);
    };
    // A part with nothing behind the plane has no section display: it stays whole.
    const bool hasSection = bool(stage->GetPrimAtPath(p.path.AppendChild(kDisplaySection)));
    set(kDisplay, !m_section || !hasSection);
    set(kDisplaySection, m_section);
    set(kDisplayFill, m_section);
}

void DocumentScene::SetVisible(int part, bool visible)
{
    if (part < 0 || part >= int(m_parts.size()))
        return;
    m_parts[size_t(part)].visible = visible;
    const UsdEditContext session(m_document.Stage(), m_document.Stage()->GetSessionLayer());
    UsdGeomImageable(m_document.Stage()->GetPrimAtPath(m_parts[size_t(part)].path))
        .CreateVisibilityAttr()
        .Set(visible ? UsdGeomTokens->inherited : UsdGeomTokens->invisible);
}

void DocumentScene::SetHighlight(int part)
{
    if (part == m_highlight)
        return;
    BindLook(m_highlight, false);
    m_highlight = part;
    BindLook(part, true);
}

void DocumentScene::BindLook(int index, bool highlighted)
{
    if (index < 0 || index >= int(m_parts.size()))
        return;
    const UsdStageRefPtr& stage = m_document.Stage();
    const UsdEditContext session(stage, stage->GetSessionLayer());
    const ScenePart& p = m_parts[size_t(index)];
    const SdfPath look = highlighted ? SdfPath("/View/Looks/Selection")
                                     : m_document.Root().AppendChild(TfToken("Looks")).AppendChild(TfToken(doc::PrimName(p.material)));
    for (const TfToken& child : {kDisplay, kDisplaySection})
        if (UsdPrim prim = stage->GetPrimAtPath(p.path.AppendChild(child)))
            UsdShadeMaterialBindingAPI(prim).Bind(UsdShadeMaterial(stage->GetPrimAtPath(look)));
}

GfMatrix4d DocumentScene::PartToWorld(const ScenePart& part) const
{
    UsdGeomXformCache cache;
    return cache.GetLocalToWorldTransform(m_document.Stage()->GetPrimAtPath(part.path));
}

PickResult DocumentScene::Pick(const GfVec3d& origin, const GfVec3d& direction) const
{
    PickResult best;
    best.distance = 1e30;
    for (size_t i = 0; i < m_parts.size(); ++i)
    {
        const ScenePart& p = m_parts[i];
        if (!p.visible || p.role == "spark")
            continue;
        const kernel::Mesh& mesh = p.sectioned && m_section && !p.section.triangles.empty() ? p.section : p.mesh;
        const GfMatrix4d inverse = PartToWorld(p).GetInverse();
        const GfVec3d o = inverse.Transform(origin), d = inverse.TransformDir(direction);
        // Moller-Trumbore; t is the same ray parameter in world space.
        for (const auto& tri : mesh.triangles)
        {
            auto P = [&](int k) { return GfVec3d(mesh.points[size_t(tri[size_t(k)])][0], mesh.points[size_t(tri[size_t(k)])][1], mesh.points[size_t(tri[size_t(k)])][2]); };
            const GfVec3d a = P(0), e1 = P(1) - a, e2 = P(2) - a, pv = GfCross(d, e2);
            const double det = GfDot(e1, pv);
            if (std::abs(det) < 1e-18)
                continue;
            const double inv = 1.0 / det;
            const GfVec3d tv = o - a;
            const double u = GfDot(tv, pv) * inv;
            if (u < 0.0 || u > 1.0)
                continue;
            const GfVec3d qv = GfCross(tv, e1);
            const double v = GfDot(d, qv) * inv;
            if (v < 0.0 || u + v > 1.0)
                continue;
            const double t = GfDot(e2, qv) * inv;
            if (t > 1e-9 && t < best.distance)
            {
                best.distance = t;
                best.part = int(i);
            }
        }
    }
    return best;
}

GfRange3d DocumentScene::WorldBounds() const
{
    GfRange3d box;
    for (const ScenePart& p : m_parts)
    {
        if (!p.visible || p.role == "spark")
            continue;
        const GfMatrix4d world = PartToWorld(p);
        for (const auto& point : p.mesh.points)
            box.UnionWith(world.Transform(GfVec3d(point[0], point[1], point[2])));
    }
    return box;
}

void WriteStudioEnvironment(const std::string& path)
{
    // Lat-long, +Z up: dark grey gradient (lighter above the horizon) with a large key soft box, a fill
    // and two rim lights, for readable metal reflections.
    const int w = 1024, h = 512;
    std::vector<float> rgb(size_t(w) * h * 3);
    struct Box
    {
        double azimuth, elevation, halfWidth, halfHeight, intensity;
    };
    const Box boxes[] = {{-50, 35, 28, 18, 9.0}, {140, 20, 35, 14, 3.0}, {60, 55, 18, 10, 6.0}, {-140, 70, 25, 15, 2.5}};
    for (int y = 0; y < h; ++y)
    {
        const double elevation = 90.0 - 180.0 * (y + 0.5) / h;
        for (int x = 0; x < w; ++x)
        {
            const double azimuth = 360.0 * (x + 0.5) / w - 180.0;
            const double t = std::clamp((elevation + 10.0) / 100.0, 0.0, 1.0);
            double v = elevation < -5.0 ? 0.035 : 0.06 + 0.22 * t;
            for (const Box& b : boxes)
            {
                double da = std::abs(azimuth - b.azimuth);
                da = std::min(da, 360.0 - da);
                const double de = std::abs(elevation - b.elevation);
                const double fa = std::clamp((b.halfWidth - da) / 6.0, 0.0, 1.0), fe = std::clamp((b.halfHeight - de) / 6.0, 0.0, 1.0);
                v += b.intensity * fa * fe;
            }
            float* p = &rgb[(size_t(y) * w + x) * 3];
            p[0] = float(v * 0.98);
            p[1] = float(v);
            p[2] = float(v * 1.04);
        }
    }
    stbi_write_hdr(path.c_str(), w, h, 3, rgb.data());
}

}  // namespace aiad::app
