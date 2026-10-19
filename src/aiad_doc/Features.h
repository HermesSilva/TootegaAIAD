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
//   {"hull": {"circles":[{"center":[u,v], "radius":r, "from":a, "to":b}, ...]}}   convex hull (arcs + tangents)
//   {"cam": {"base_radius":r, "lift":h, "open":a, "close":b}}   flat-follower cam, sin^2 lift law, crank angles of
//                                                                the 720 cycle; follower on -u, cam turns +n at half speed
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

// The closed outline of a sketch, densely sampled (measurements on the drawing, e.g. cam lift).
bool SketchOutline(const Json& sketch, const NameLookup& lookup, std::vector<std::array<double, 2>>& points, std::string& error);

// Follower lift per cycle degree (720 values) of a cam outline: flat follower on -u, cam turning at
// half crank speed (cycle angle c faces the follower at cam angle c / 2).
std::vector<double> MeasureFlatFollowerLift(const std::vector<std::array<double, 2>>& outline, double baseRadius);

}  // namespace aiad::doc
