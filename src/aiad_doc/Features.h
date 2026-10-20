// Feature history of a part (JSON, stored in the document) -> exact solid (kernel).
//
// A part is an ordered list of features; the first one creates the solid, the next ones add to it,
// cut it or intersect it ("mode": "add" | "cut" | "intersect"). Every number may be a formula over
// the document parameters ("bore / 2 + 0.0035").
//
//   {"op":"extrude", "plane":P, "profile":S, "distance":d, "start":s}      sketch S on plane P, from s to s+d along its normal
//   {"op":"revolve", "plane":P, "profile":S, "axis":[u,v,du,dv], "angle":a, "start_angle":a0}
//   {"op":"cylinder", "base":[x,y,z], "axis":[x,y,z], "radius":r, "height":h}
//   {"op":"box", "corner":[x,y,z], "size":[x,y,z]}        {"op":"sphere", "center":[x,y,z], "radius":r}
//   {"op":"helix_pipe", "base":[..], "axis":[..], "coil_radius":r, "height":h, "turns":n, "wire_radius":w}
//   {"op":"pipe", "path":[[x,y,z],...], "radius":r}
//   {"op":"pattern_linear", "count":n, "step":[x,y,z]}           repeats the previous feature n times
//   {"op":"pattern_polar", "count":n, "axis_point":[..], "axis":[..], "step_angle":a}
//   {"op":"rotate", "pivot":[..], "axis":[..], "angle":a}        {"op":"translate", "offset":[..]}   (whole part)
//
// Planes: "XY", "YZ", "ZX", "XZ", "YX", "ZY" (first letter = sketch u axis, second = v axis), or
// {"origin":[..], "x":[..], "y":[..]}; "offset": d moves a named plane along its normal.
// Sketches: [[u,v], [u,v,bulge], ...] (bulge = tan(arc sweep / 4), + = counter-clockwise) or a tool:
//   {"circle": {"center":[u,v], "radius":r}}           {"rect": {"min":[u,v], "max":[u,v]}}
//   {"polygon": {"center":[u,v], "across_flats":f, "sides":n, "rotation":a}}
//   {"rounded_rect": {"min":[u,v], "max":[u,v], "radius":r}}
//   {"sprocket": {"root_radius":r, "tip_radius":t, "teeth":n}}
//   {"gear": {"module":m, "teeth":z, "pressure_angle":20, "center":[u,v], "rotation":a}}   involute spur gear
//   {"hull": {"circles":[{"center":[u,v], "radius":r, "from":a, "to":b}, ...]}}   convex hull (arcs + tangents)
//   {"cam": {"base_radius":r, "lift":h, "open":a, "close":b}}   flat-follower cam, sin^2 lift law, crank angles of
//                                                                the 720 cycle; follower on -u, cam turns +n at half speed
//   {"spiral": {"center":[u,v], "inner_radius":r, "pitch":p, "turns":n, "thickness":t, "rotation":a}}   a strip wound
//                                                                as an Archimedean spiral (hairspring, mainspring)
#pragma once

#include "Expression.h"
#include "Json.h"
#include "Kernel.h"

#include <array>
#include <string>
#include <vector>

namespace aiad::doc
{

kernel::Solid BuildPart(const Json& features, const NameLookup& lookup, std::string& error);

// The rigid moves ("rotate", "translate") of features[from..] applied to a solid: an imported part (its first
// feature "import") moved into place.
bool MovePart(const Json& features, size_t from, const NameLookup& lookup, kernel::Solid& solid, std::string& error);

// The closed outline of a sketch, densely sampled (measurements on the drawing, e.g. cam lift).
bool SketchOutline(const Json& sketch, const NameLookup& lookup, std::vector<std::array<double, 2>>& points, std::string& error);

// Follower lift per cycle degree (720 values) of a cam outline: flat follower on -u, cam turning at
// half crank speed (cycle angle c faces the follower at cam angle c / 2).
std::vector<double> MeasureFlatFollowerLift(const std::vector<std::array<double, 2>>& outline, double baseRadius);

// What the simulator measures on a part's features (part frame; the "rotate"/"translate" features after the
// measured one are applied).
//
// A wheel: the first extruded gear, sprocket or circle (a pulley). Pitch radius: m z / 2 (gear), the mean of
// root and tip (sprocket), the radius (circle; no teeth).
struct Wheel
{
    int teeth = 0;
    double pitchRadius = 0.0, rootRadius = 0.0, tipRadius = 0.0, module = 0.0;
    kernel::Vec3 centre, axis;   // on the mid plane of the extrusion
    double width = 0.0;
};
bool FindWheel(const Json& features, const NameLookup& lookup, Wheel& wheel, std::string& error);

// The outline of the first extruded sketch (a cam lobe, a rocker): points on its mid plane, the extrusion
// axis and the extent along it (from the mid plane, +/- width / 2).
struct ExtrudedProfile
{
    std::vector<kernel::Vec3> points;
    kernel::Vec3 centre, axis;   // sketch origin on the mid plane; the extrusion direction
    double width = 0.0;
};
bool FindExtrudedProfile(const Json& features, const NameLookup& lookup, ExtrudedProfile& profile, std::string& error);

// A coil spring: the first helix_pipe.
struct Helix
{
    kernel::Vec3 base, axis;
    double coilRadius = 0.0, height = 0.0, turns = 0.0, wireRadius = 0.0;
};
bool FindHelix(const Json& features, const NameLookup& lookup, Helix& helix, std::string& error);

}  // namespace aiad::doc
