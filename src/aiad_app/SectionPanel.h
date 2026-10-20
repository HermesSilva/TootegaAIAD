// Section planes on screen: the Sections panel (new planes along the axes or facing the view, through a part;
// orientation, offset, which parts each cuts), their outlines in the view, and the section items of a part's context
// menu. Every change is a Command Bus command (aiad_section_define / aiad_section_delete).
#pragma once

#include "InstrumentPanels.h"

#include <pxr/base/gf/vec3d.h>

namespace aiad::app
{

struct Session;

// facing: the model-frame direction from the scene toward the camera (a plane "facing the view" cuts away the near side).
void DrawSectionsPanel(Session& session, bool& open, int selected, const pxr::GfVec3d& facing, const RunCommand& command);
// The planes' outlines over the model while the panel is open (the one being edited brighter), with handles: the
// arrow's tip turns a plane in 3D (right, up: the camera's axes in the model frame), its centre moves it in depth.
// True while the pointer is on a handle or drags one (the camera leaves it alone).
bool DrawSectionPlanes(Session& session, const Project& project, const pxr::GfVec3d& right, const pxr::GfVec3d& up, bool hovered,
                       const RunCommand& command);
// Inside a part's context menu: cut only this part, keep it whole in a plane, or cut it again.
void SectionPartItems(Session& session, int part, const pxr::GfVec3d& facing, const RunCommand& command);

}  // namespace aiad::app
