// The engine simulation study of a document: everything the simulation uses is read from the drawing
// (parameters, the parts of each rigid body, the cam lobes as drawn). Nothing is hand entered.
#pragma once

#include "Document.h"

#include "engine/EngineSimulation.h"
#include "magneto/MagnetoField.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace aiad::app
{

// Mechanism placement read from the drawing, for posing the bodies.
struct EngineLayout
{
    double crankRadius = 0.0, rodLength = 0.0, springLength = 0.0;
};

struct BodyMass
{
    double mass = 0.0;
    kernel::Vec3 centroid;             // body frame
    double inertiaZ = 0.0;             // about the body origin's Z axis (crank axis for the crankshaft)
    double inertiaZCentroid = 0.0;     // about a Z axis through the centroid
};

struct EngineStudy
{
    sim::engine::EngineDesign simulation;
    sim::magneto::MagnetoGeometry magneto;
    EngineLayout layout;
    std::vector<double> intakeLift, exhaustLift;   // measured on the drawn lobes (720 values)
    std::map<std::string, BodyMass> bodies;
};

// Fails with the missing parameter, part role or body when the document is not an engine model.
bool DeriveEngineStudy(const doc::Document& document, EngineStudy& study, std::string& error);

}  // namespace aiad::app
