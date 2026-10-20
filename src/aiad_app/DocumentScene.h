// The view of a document: every object shown is a part of the document, displayed from its exact
// B-rep (tessellated by the kernel: smooth normals inside faces, sharp edges between them). What only
// the view needs (display meshes, the section switch, selection highlight, simulation poses) lives in
// the stage's session layer and is never saved with the drawing.
#pragma once

#include "Document.h"
#include "Kernel.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/usd/attribute.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aiad::app
{

struct ScenePart
{
    pxr::SdfPath path;          // the document part
    std::string name, body, material, role;
    bool sectioned = false;
    bool visible = true;
    kernel::Mesh mesh, section; // body frame (picking, bounds)
    uint64_t revision = 0;      // the document's shape revision shown
    bool dirty = true;          // its display is still to be (re)written
};

struct PickResult
{
    int part = -1;
    double distance = 0.0;
};

// Motion of a body from its rest placement, in the mechanism frame: p = rest origin + transform(p_body)
// (rigid for rigid bodies; a drawn spring is also stretched).
struct BodyMotion
{
    std::string body;
    pxr::GfMatrix4d transform{1.0};
};

class DocumentScene
{
public:
    DocumentScene(doc::Document& document, const std::string& environmentHdr);

    // Re-reads the document (after any change): the part list at once; the parts whose solid changed are
    // marked and their display is written by Update, a slice per frame (the view never blocks the UI).
    void Refresh();
    // Writes the display of changed parts for up to `budgetMs`; true when nothing is left.
    bool Update(double budgetMs);
    bool Pending() const;
    size_t PendingCount() const;   // parts whose display is still to be written
    const std::vector<ScenePart>& Parts() const { return m_parts; }
    int Find(const std::string& name) const;

    void SetMotion(const std::vector<BodyMotion>& motion);
    void SetSparkVisible(bool visible);
    void SetSectionView(bool section);
    bool SectionView() const { return m_section; }
    void SetVisible(int part, bool visible);
    void SetHighlight(int part);
    PickResult Pick(const pxr::GfVec3d& origin, const pxr::GfVec3d& direction) const;
    pxr::GfRange3d WorldBounds() const;
    // Model frame (joints, the study) to the world the camera sees (the document's orientation).
    pxr::GfMatrix4d ModelToWorld() const;
    double TessellationSeconds() const { return m_tessellationSeconds; }

private:
    void WriteDisplay(ScenePart& part);
    void ApplySection(const ScenePart& part);
    void BindLook(int part, bool highlighted);
    pxr::GfMatrix4d PartToWorld(const ScenePart& part) const;

    doc::Document& m_document;
    std::vector<ScenePart> m_parts;
    std::map<std::string, pxr::UsdAttribute> m_motion;
    pxr::UsdAttribute m_sparkScale;
    bool m_section = true;
    int m_highlight = -1;
    double m_tessellationSeconds = 0.0;
};

// Writes a procedural studio environment (lat-long Radiance HDR: dark gradient and soft boxes): the
// lighting of the view, not an object of it.
void WriteStudioEnvironment(const std::string& path);

}  // namespace aiad::app
