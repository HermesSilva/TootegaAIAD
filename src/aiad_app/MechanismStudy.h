// The machine of a document, for the generic simulation: rigid bodies (mass properties from their parts x
// materials), the joints, and the elements tied to them (gears, belts, racks, cam/pad contacts, springs,
// loads, chambers, magnetos), every physical input measured on the drawn parts. No machine is known here;
// the study also criticises the drawing (what does not fit, touch, mesh, seat or move), each finding with
// its fix.
#pragma once

#include "Document.h"
#include "StudyFinding.h"

#include "machine/Machine.h"

#include <map>
#include <string>
#include <vector>

namespace aiad::app
{

struct MechanismStudy
{
    sim::machine::Description machine;
    std::vector<std::string> bodies;               // the names of machine.mechanism.bodies, same order
    std::map<std::string, kernel::Vec3> origins;   // rest origin of each body (model frame)
    // Bodies that only show a drawn spring: stretched between the spring's ends in the view.
    struct SpringBody
    {
        std::string body;
        int spring = -1;
    };
    std::vector<SpringBody> springBodies;
    // Adjusters: joints with a range (limits) that nothing in the machine drives (no motor, gear, belt, spring, contact or
    // touch on them): set by hand, as in real life (a watch's regulator, an idle screw, a setting lever), and held where
    // they are set while the machine runs. Their position is the joint's start.
    struct Adjuster
    {
        int joint = -1;                 // in machine.mechanism.joints
        std::string name, body;         // the joint, the body that is moved
        bool rotation = true;           // turns (rad) or slides (m)
        double lower = 0.0, upper = 0.0, value = 0.0;
    };
    std::vector<Adjuster> adjusters;
    int mobility = 0;                              // degrees of freedom left (Gruebler-Kutzbach)
    std::vector<std::string> measured;             // what was read from the drawing, one line each
    std::vector<StudyFinding> findings;            // errors stop the simulation; notes inform

    bool Runnable() const;
    std::string Report() const;   // findings as a Markdown list with their fixes
    int Errors() const;
};

// False when the document has no joints (it is not a mechanism yet). Otherwise fills the study; the
// simulation may run when it has no error finding.
bool DeriveMechanismStudy(const doc::Document& document, MechanismStudy& study);

}  // namespace aiad::app
