#include "Instruments.h"

#include "Document.h"

#include <algorithm>
#include <format>
#include <numbers>

namespace aiad::doc
{

namespace
{

constexpr double kTurn = 1.0 / (2.0 * std::numbers::pi), kDeg = 180.0 / std::numbers::pi;

}  // namespace

const std::vector<Unit>& UnitsOf(Dimension d)
{
    static const std::vector<std::vector<Unit>> units = {
        {{"deg", "°", kDeg, 0}, {"rad", "rad", 1, 0}, {"turn", "turn", kTurn, 0}, {"mrad", "mrad", 1e3, 0}, {"arcmin", "'", kDeg * 60, 0}, {"arcsec", "\"", kDeg * 3600, 0}},
        {{"rpm", "rpm", 60 * kTurn, 0}, {"rad/s", "rad/s", 1, 0}, {"deg/s", "°/s", kDeg, 0}, {"Hz", "Hz", kTurn, 0}},
        {{"rad/s2", "rad/s²", 1, 0}, {"deg/s2", "°/s²", kDeg, 0}, {"rpm/s", "rpm/s", 60 * kTurn, 0}},
        {{"mm", "mm", 1e3, 0}, {"um", "µm", 1e6, 0}, {"m", "m", 1, 0}, {"in", "in", 1 / 0.0254, 0}},
        {{"m/s", "m/s", 1, 0}, {"mm/s", "mm/s", 1e3, 0}, {"km/h", "km/h", 3.6, 0}},
        {{"m/s2", "m/s²", 1, 0}, {"g", "g", 1 / 9.80665, 0}, {"mm/s2", "mm/s²", 1e3, 0}},
        {{"bar", "bar", 1e-5, 0}, {"kPa", "kPa", 1e-3, 0}, {"MPa", "MPa", 1e-6, 0}, {"Pa", "Pa", 1, 0}, {"psi", "psi", 1 / 6894.757293168, 0}, {"atm", "atm", 1 / 101325.0, 0}},
        {{"degC", "°C", 1, -273.15}, {"K", "K", 1, 0}, {"degF", "°F", 1.8, -459.67}},
        {{"cm3", "cm³", 1e6, 0}, {"mm3", "mm³", 1e9, 0}, {"L", "L", 1e3, 0}, {"m3", "m³", 1, 0}},
        {{"N", "N", 1, 0}, {"mN", "mN", 1e3, 0}, {"uN", "µN", 1e6, 0}, {"kN", "kN", 1e-3, 0}},
        {{"N.m", "N·m", 1, 0}, {"N.mm", "N·mm", 1e3, 0}, {"mN.m", "mN·m", 1e3, 0}, {"uN.m", "µN·m", 1e6, 0}, {"nN.m", "nN·m", 1e9, 0}},
        {{"kV", "kV", 1e-3, 0}, {"V", "V", 1, 0}},
        {{"s", "s", 1, 0}, {"ms", "ms", 1e3, 0}, {"us", "µs", 1e6, 0}, {"min", "min", 1 / 60.0, 0}},
        {{"Hz", "Hz", 1, 0}, {"vph", "vph", 7200, 0}, {"rpm", "rpm", 60, 0}, {"kHz", "kHz", 1e-3, 0}},
        {{"count", "", 1, 0}},
        {{"%", "%", 100, 0}, {"ratio", "", 1, 0}},
        {{"kW", "kW", 1e-3, 0}, {"W", "W", 1, 0}, {"mW", "mW", 1e3, 0}, {"uW", "µW", 1e6, 0}, {"nW", "nW", 1e9, 0}, {"pW", "pW", 1e12, 0},
         {"hp", "hp", 1 / 745.69987, 0}, {"cv", "cv", 1 / 735.49875, 0}}};
    return units[size_t(d)];
}

const Unit& FindUnit(Dimension d, const std::string& name)
{
    const std::vector<Unit>& list = UnitsOf(d);
    for (const Unit& u : list)
        if (name == u.name || name == u.label)
            return u;
    return list.front();
}

const char* SiUnit(Dimension d)
{
    static const char* si[] = {"rad", "rad/s", "rad/s2", "m", "m/s", "m/s2", "Pa", "K", "m3", "N", "N.m", "V", "s", "Hz", "count", "ratio", "W"};
    return si[size_t(d)];
}

const std::vector<InstrumentType>& InstrumentTypes()
{
    using D = Dimension;
    static const std::vector<InstrumentType> types = {
        {"tachometer", "TC-1", "Tachometer", "speed of a turning or sliding joint, averaged over the gate time", 1, "rpm", 1, {D::AngularSpeed, D::Speed}},
        {"oscillation", "OA-1", "Oscillation analyzer",
         "an oscillating joint (a balance, a pendulum, a valve): frequency, beats per hour, amplitude, beat error, rate against a reference (a "
         "watchmaker's timegrapher)",
         1, "vph", 0, {D::Angle, D::Length}},
        {"manometer", "MN-1", "Manometer", "pressure of a chamber", 1, "bar", 3, {D::Pressure}},
        {"thermometer", "TH-1", "Thermometer", "temperature of a chamber", 1, "degC", 1, {D::Temperature}},
        {"protractor", "PR-1", "Protractor", "angle of a turning joint, unwrapped or within a turn", 1, "deg", 2, {D::Angle}},
        {"dial", "DI-1", "Dial indicator", "displacement of a sliding joint (a valve's lift, a piston's stroke)", 1, "mm", 3, {D::Length}},
        {"gauge", "FG-1", "Force and torque gauge", "force of a spring, a contact or a touch; torque of a spiral spring", 1, "N", 3, {D::Force, D::Torque}},
        {"voltmeter", "VM-1", "Voltmeter", "the spark voltage of a magneto", 1, "kV", 2, {D::Voltage}},
        {"accelerometer", "AC-1", "Accelerometer", "acceleration of a joint", 1, "m/s2", 2, {D::AngularAccel, D::Accel}},
        {"timer", "TI-1", "Timer and counter",
         "events where a probe crosses a threshold (rise, fall or both): the interval between them, their frequency, their count, or the "
         "machine's time (stopwatch)",
         1, "ms", 3, {}},
        {"dynamometer", "DY-1", "Dynamometer",
         "the real (brake) power a machine gives at a shaft, read on a dynamometer element (a bench brake): power, torque, speed, the "
         "indicated power of the chambers turning that shaft and the mechanical efficiency, and the torque and power curve against rpm",
         1, "kW", 2, {D::Power}},
        {"meter", "MM-1", "Universal meter", "any quantity a probe can read, with its statistics", 1, "", 4, {}},
        {"chart", "CR-1", "Chart recorder", "up to four probes drawn over time", 4, "", 4, {}}};
    return types;
}

const InstrumentType* FindInstrumentType(const std::string& type)
{
    for (const InstrumentType& t : InstrumentTypes())
        if (type == t.type)
            return &t;
    return nullptr;
}

bool QuantityDimension(const std::string& targetKind, bool rotation, const std::string& quantity, Dimension& dimension, std::string& known)
{
    struct Q
    {
        const char* kind;
        const char* quantity;
        Dimension turning, sliding;
    };
    using D = Dimension;
    static const Q table[] = {{"joint", "position", D::Angle, D::Length},          {"joint", "speed", D::AngularSpeed, D::Speed},
                              {"joint", "acceleration", D::AngularAccel, D::Accel}, {"chamber", "pressure", D::Pressure, D::Pressure},
                              {"chamber", "temperature", D::Temperature, D::Temperature}, {"chamber", "volume", D::Volume, D::Volume},
                              {"magneto", "voltage", D::Voltage, D::Voltage},       {"touch", "force", D::Force, D::Force},
                              {"contact", "force", D::Force, D::Force},             {"spring", "force", D::Force, D::Force},
                              {"torsion", "torque", D::Torque, D::Torque},
                              {"dynamometer", "power", D::Power, D::Power},       {"dynamometer", "torque", D::Torque, D::Torque},
                              {"dynamometer", "speed", D::AngularSpeed, D::AngularSpeed}};
    known.clear();
    for (const Q& q : table)
        if (targetKind == q.kind)
        {
            if (quantity == q.quantity)
            {
                dimension = rotation ? q.turning : q.sliding;
                return true;
            }
            known += (known.empty() ? "" : ", ") + std::string(q.quantity);
        }
    return false;
}

std::string DefaultQuantity(const std::string& type, const std::string& targetKind)
{
    if (targetKind == "joint")
        return type == "tachometer" ? "speed" : type == "accelerometer" ? "acceleration" : "position";
    static const std::pair<const char*, const char*> byKind[] = {{"chamber", "pressure"}, {"magneto", "voltage"}, {"touch", "force"},
                                                                {"contact", "force"},   {"spring", "force"},    {"torsion", "torque"},
                                                                {"dynamometer", "power"}};
    if (targetKind == "dynamometer" && (type == "gauge" || type == "tachometer"))
        return type == "gauge" ? "torque" : "speed";
    if (targetKind == "chamber" && type == "thermometer")
        return "temperature";
    for (const auto& [kind, quantity] : byKind)
        if (targetKind == kind)
            return quantity;
    return "";
}

bool ResolveProbe(const Document& document, const std::string& type, const Json& probe, std::string& targetKind, std::string& target, bool& rotation,
                  std::string& quantity, Dimension& dimension, std::string& error)
{
    rotation = true;
    if (!probe.is_object() || probe.contains("joint") == probe.contains("element"))
    {
        error = "a probe is {\"joint\": name} or {\"element\": name}, with an optional \"quantity\"";
        return false;
    }
    if (probe.contains("joint"))
    {
        target = probe["joint"].is_string() ? probe["joint"].get<std::string>() : std::string();
        const auto joint = document.FindJoint(target);
        if (!joint)
        {
            error = std::format("no joint '{}' (aiad_joints_list)", target);
            return false;
        }
        if (joint->type != "revolute" && joint->type != "cylindrical" && joint->type != "prismatic")
        {
            error = std::format("the joint '{}' is {}: a probe reads a revolute, cylindrical or prismatic joint", target, joint->type);
            return false;
        }
        targetKind = "joint";
        rotation = joint->type != "prismatic";
    }
    else
    {
        target = probe["element"].is_string() ? probe["element"].get<std::string>() : std::string();
        const auto element = document.FindElement(target);
        if (!element)
        {
            error = std::format("no element '{}' (aiad_elements_list)", target);
            return false;
        }
        targetKind = element->type;
    }
    quantity = probe.value("quantity", DefaultQuantity(type, targetKind));
    std::string known;
    if (!QuantityDimension(targetKind, rotation, quantity, dimension, known))
    {
        error = known.empty() ? std::format("nothing to measure on a {} ('{}')", targetKind, target)
                              : std::format("a {} has no quantity '{}' (it has: {})", targetKind, quantity, known);
        return false;
    }
    return true;
}

bool ValidateInstrument(const Document& document, const InstrumentInfo& instrument, std::string& error)
{
    const InstrumentType* type = FindInstrumentType(instrument.type);
    if (instrument.name.empty())
        error = "an instrument needs a name";
    else if (!type)
    {
        error = "unknown instrument type '" + instrument.type + "' (";
        for (const InstrumentType& t : InstrumentTypes())
            error += std::string(&t == &InstrumentTypes().front() ? "" : ", ") + t.type;
        error += ")";
    }
    else if (!instrument.definition.is_object() || !instrument.definition.contains("probes") || !instrument.definition["probes"].is_array() ||
             instrument.definition["probes"].empty())
        error = "the definition needs \"probes\": [{\"joint\": name} or {\"element\": name}, with an optional \"quantity\"]";
    else if (int(instrument.definition["probes"].size()) > type->maxProbes)
        error = std::format("a {} takes at most {} probe(s)", type->type, type->maxProbes);
    if (!error.empty())
        return false;
    const Json& d = instrument.definition;
    Dimension first = Dimension::Ratio;
    for (size_t i = 0; i < d["probes"].size(); ++i)
    {
        std::string kind, target, quantity, e;
        bool rotation = true;
        Dimension dimension;
        if (!ResolveProbe(document, type->type, d["probes"][i], kind, target, rotation, quantity, dimension, e))
        {
            error = std::format("probes[{}]: {}", i, e);
            return false;
        }
        if (!type->dimensions.empty() && std::find(type->dimensions.begin(), type->dimensions.end(), dimension) == type->dimensions.end())
        {
            error = std::format("probes[{}]: a {} does not measure the {} of '{}' (use a meter or a chart for it)", i, type->title, quantity, target);
            return false;
        }
        if (i == 0)
            first = dimension;
    }
    if (d.contains("unit"))
    {
        const std::string unit = d["unit"].is_string() ? d["unit"].get<std::string>() : std::string();
        const Dimension shown = instrument.type == "timer" ? (d.value("mode", "interval") == "count"       ? Dimension::Count
                                                              : d.value("mode", "interval") == "frequency" ? Dimension::Frequency
                                                                                                           : Dimension::Time)
                              : instrument.type == "oscillation" ? Dimension::Frequency
                                                                 : first;
        bool found = false;
        std::string units;
        for (const Unit& u : UnitsOf(shown))
        {
            found = found || unit == u.name || unit == u.label;
            units += (units.empty() ? "" : ", ") + std::string(u.name);
        }
        if (!found)
        {
            error = std::format("unit '{}' is not one of {}", unit, units);
            return false;
        }
    }
    const int digits = d.value("digits", type->defaultDigits);
    if (digits < 0 || digits > 9)
        error = "digits: 0 to 9 decimals";
    else if (d.value("gate", 0.5) < 0.0 || d.value("window", 10.0) <= 0.0)
        error = "gate: the averaging time, 0 (live) or more [s]; window: the chart's span, > 0 [s]";
    else if (instrument.type == "timer")
    {
        const std::string mode = d.value("mode", "interval"), edge = d.value("edge", "rise");
        if (mode != "interval" && mode != "count" && mode != "frequency" && mode != "stopwatch")
            error = "mode: interval, count, frequency or stopwatch";
        else if (edge != "rise" && edge != "fall" && edge != "both")
            error = "edge: rise, fall or both";
        else if (d.contains("threshold") && !d["threshold"].is_number())
            error = "threshold: a number, in the probe's SI unit";
    }
    else if (instrument.type == "oscillation" && d.contains("reference") && (!d["reference"].is_number() || d["reference"].get<double>() <= 0.0))
        error = "reference: the nominal frequency in beats per hour (e.g. 18000)";
    return error.empty();
}

}  // namespace aiad::doc
