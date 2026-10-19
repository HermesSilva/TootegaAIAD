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
