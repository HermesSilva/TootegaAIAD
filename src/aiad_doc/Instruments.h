// Measuring instruments: document entities that observe the machine. An instrument has probes plugged into what
// it measures (a joint, an element); a probe is ideal: it reads the solver's state and changes nothing (no mass,
// no force, no step). This is the catalogue: the instrument types, what a probe can read on each kind of target,
// and the units of each dimension.
#pragma once

#include "Json.h"

#include <string>
#include <vector>

namespace aiad::doc
{

class Document;

// A stored instrument: {probes: [{joint | element, quantity}], unit?, digits?, gate?, window?, visible?, ...}.
struct InstrumentInfo
{
    std::string name, type;
    Json definition = Json::object();
};

// What a probe measures: its dimension gives the units an instrument can show it in.
enum class Dimension
{
    Angle,          // rad
    AngularSpeed,   // rad/s
    AngularAccel,   // rad/s^2
    Length,         // m
    Speed,          // m/s
    Accel,          // m/s^2
    Pressure,       // Pa
    Temperature,    // K
    Volume,         // m^3
    Force,          // N
    Torque,         // N m
    Voltage,        // V
    Time,           // s
    Frequency,      // Hz
    Count,          // events
    Ratio           // dimensionless
};

struct Unit
{
    const char* name;       // as written in a definition ("degC", "um")
    const char* label;      // as shown ("°C", "µm")
    double scale, offset;   // shown = SI * scale + offset
};
const std::vector<Unit>& UnitsOf(Dimension d);
const Unit& FindUnit(Dimension d, const std::string& name);   // the first unit of d when unknown
const char* SiUnit(Dimension d);

struct InstrumentType
{
    const char* type;     // "tachometer"
    const char* model;    // "TC-1"
    const char* title;    // "Tachometer"
    const char* purpose;  // one line for the catalogue and the agent
    int maxProbes;        // 1, or more (the chart recorder)
    const char* defaultUnit;
    int defaultDigits;    // decimals
    std::vector<Dimension> dimensions;   // what its probe may read; empty: anything
};
const std::vector<InstrumentType>& InstrumentTypes();
const InstrumentType* FindInstrumentType(const std::string& type);

// The quantity a probe reads when its definition names none ("speed" for a tachometer on a joint...).
std::string DefaultQuantity(const std::string& type, const std::string& targetKind);
// A probe of a definition, resolved: its target kind ("joint" or the element's type), target name, whether a joint
// turns, its quantity and dimension.
bool ResolveProbe(const Document& document, const std::string& type, const Json& probe, std::string& targetKind, std::string& target, bool& rotation,
                  std::string& quantity, Dimension& dimension, std::string& error);

// The dimension of a probe's quantity on its target kind: a joint ("revolute" turns, "prismatic" slides) or an element
// type ("chamber", "magneto", "touch", "contact", "spring", "torsion"). False when the target has no such quantity;
// `known` lists the ones it has.
bool QuantityDimension(const std::string& targetKind, bool rotation, const std::string& quantity, Dimension& dimension, std::string& known);

// Checks a definition against the document (types, probes on existing joints and elements, quantities, units).
bool ValidateInstrument(const Document& document, const InstrumentInfo& instrument, std::string& error);

}  // namespace aiad::doc
