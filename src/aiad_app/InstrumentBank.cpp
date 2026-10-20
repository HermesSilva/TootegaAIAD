#include "InstrumentBank.h"

#include "Document.h"
#include "MechanismStudy.h"

#include "machine/Machine.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace aiad::app
{

namespace
{

constexpr size_t kRawSamples = size_t(1) << 16;   // the last samples, every step
constexpr size_t kHistoryBuckets = 600;           // the chart's resolution over its window
constexpr size_t kEvents = 4096;
constexpr unsigned kPalette[] = {0xFF2AB4FF, 0xFFFFC23A, 0xFF7CE38B, 0xFFFF7B9C, 0xFFB794FF, 0xFF5BE0E8, 0xFFFF9F43, 0xFFE8E86A};

using Source = InstrumentProbe::Source;

template <typename T>
int IndexByName(const std::vector<T>& list, const std::string& name)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].name == name)
            return int(i);
    return -1;
}

double ReadSource(const InstrumentProbe& p, const sim::machine::Machine& machine, const MechanismStudy& study)
{
    const sim::mb::Multibody& mb = machine.Mechanism();
    switch (p.source)
    {
    case Source::JointPosition: return mb.Coordinate(p.index);
    case Source::JointSpeed: return mb.Rate(p.index);
    case Source::JointAcceleration: return mb.Rate(p.index);   // differentiated by the sampler
    case Source::ChamberPressure: return machine.Chamber(size_t(p.index)).pressure;
    case Source::ChamberTemperature: return machine.Chamber(size_t(p.index)).temperature;
    case Source::ChamberVolume: return machine.Chamber(size_t(p.index)).volume;
    case Source::MagnetoVoltage: return machine.Magneto(size_t(p.index)).lastSecondaryKv * 1e3;
    case Source::TouchForce: return mb.TouchForce(p.index);
    case Source::ContactForce: return mb.ContactForce(p.index);
    case Source::SpringForce: return mb.SpringForce(p.index);
    case Source::TorsionTorque:
    {
        const sim::mb::TorsionSpring& t = study.machine.mechanism.torsions[size_t(p.index)];
        return t.stiffness * (t.free - mb.Coordinate(t.joint));
    }
    case Source::DynoPower: return machine.Dyno(size_t(p.index)).power;
    case Source::DynoTorque: return machine.Dyno(size_t(p.index)).torque;
    case Source::DynoSpeed: return machine.Dyno(size_t(p.index)).speed;
    case Source::DynoTarget: return machine.Dyno(size_t(p.index)).target;
    case Source::IndicatedWork:
    {
        // The chambers whose crank (or ignition rotor) is the dynamometer's shaft.
        const sim::machine::Description& d = study.machine;
        const int shaft = d.dynamometers[size_t(p.index)].joint;
        double work = 0.0;
        for (size_t i = 0; i < d.chambers.size(); ++i)
        {
            const sim::machine::Chamber& c = d.chambers[i];
            const int crank = c.crank >= 0 ? c.crank : c.ignition >= 0 ? d.magnetos[size_t(c.ignition)].rotor : -1;
            if (crank == shaft)
                work += machine.Chamber(i).work;
        }
        return work;
    }
    case Source::None: break;
    }
    return 0.0;
}

}  // namespace

// A dynamometer's point of the curve: the brake held the shaft at its speed (within 2 %).
bool DynoHeld(double speed, double target)
{
    return target > 0.0 && std::abs(speed - target) <= 0.02 * target;
}

namespace
{

// The change of a probe from `t0` to its newest sample (false with fewer than two samples there).
bool ChangeSince(const InstrumentProbe& p, double t0, double& change, double& span)
{
    if (p.rawCount < 2)
        return false;
    const InstrumentSample& last = p.raw[(p.rawHead + kRawSamples - 1) % kRawSamples];
    const InstrumentSample* first = nullptr;
    for (size_t k = 1; k < p.rawCount; ++k)
    {
        const InstrumentSample& s = p.raw[(p.rawHead + kRawSamples - 1 - k) % kRawSamples];
        if (s.t < t0)
            break;
        first = &s;
    }
    if (!first || last.t <= first->t)
        return false;
    change = last.v - first->v, span = last.t - first->t;
    return true;
}

// The samples of a probe from the newest back to `t0` (mean, or false when there are none).
bool MeanSince(const InstrumentProbe& p, double t0, double& mean)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t k = 0; k < p.rawCount; ++k)
    {
        const InstrumentSample& s = p.raw[(p.rawHead + kRawSamples - 1 - k) % kRawSamples];
        if (s.t < t0)
            break;
        sum += s.v;
        ++n;
    }
    if (n == 0)
        return false;
    mean = sum / double(n);
    return true;
}

Reading Make(const std::string& label, double si, doc::Dimension dimension, const std::string& unit, int digits, bool valid = true)
{
    const doc::Unit& u = doc::FindUnit(dimension, unit);
    return {label, si * u.scale + u.offset, u.label, digits, valid};
}

}  // namespace

std::string Reading::Text() const
{
    if (!valid || !std::isfinite(value))
        return "----";
    return std::format("{:.{}f}", value, std::clamp(digits, 0, 9));
}

void InstrumentBank::Build(const doc::Document& document, const MechanismStudy& study)
{
    m_instruments.clear();
    m_now = 0.0;
    const sim::mb::Model& model = study.machine.mechanism;
    unsigned colour = 0;
    for (const doc::InstrumentInfo& info : document.Instruments())
    {
        LiveInstrument li;
        li.info = info;
        li.type = doc::FindInstrumentType(info.type);
        if (!li.type)
            continue;
        const Json& d = info.definition;
        li.digits = d.value("digits", li.type->defaultDigits);
        li.gate = d.value("gate", info.type == "oscillation" ? 2.0 : 0.5);
        li.window = d.value("window", 10.0);
        li.visible = d.value("visible", true);
        li.mode = d.value("mode", "interval");
        li.edge = d.value("edge", "rise");
        li.reference = d.value("reference", 0.0);
        li.wrap = d.value("wrap", false);
        li.fixedThreshold = d.contains("threshold") && d["threshold"].is_number();
        li.threshold = li.fixedThreshold ? d["threshold"].get<double>() : 0.0;
        li.colour = kPalette[colour++ % std::size(kPalette)];
        for (const Json& pd : d.value("probes", Json::array()))
        {
            InstrumentProbe p;
            std::string kind, error;
            bool rotation = true;
            if (!doc::ResolveProbe(document, info.type, pd, kind, p.target, rotation, p.quantity, p.dimension, error))
                p.problem = error;
            else if (kind == "joint")
            {
                p.index = IndexByName(model.joints, p.target);
                p.source = p.quantity == "speed" ? Source::JointSpeed : p.quantity == "acceleration" ? Source::JointAcceleration : Source::JointPosition;
            }
            else if (kind == "chamber")
            {
                p.index = IndexByName(study.machine.chambers, p.target);
                p.source = p.quantity == "temperature" ? Source::ChamberTemperature : p.quantity == "volume" ? Source::ChamberVolume : Source::ChamberPressure;
            }
            else if (kind == "magneto")
                p.index = IndexByName(study.machine.magnetos, p.target), p.source = Source::MagnetoVoltage;
            else if (kind == "touch")
                p.index = IndexByName(model.touches, p.target), p.source = Source::TouchForce;
            else if (kind == "contact")
                p.index = IndexByName(model.contacts, p.target), p.source = Source::ContactForce;
            else if (kind == "spring")
                p.index = IndexByName(model.springs, p.target), p.source = Source::SpringForce;
            else if (kind == "torsion")
                p.index = IndexByName(model.torsions, p.target), p.source = Source::TorsionTorque;
            else if (kind == "dynamometer")
            {
                p.index = IndexByName(study.machine.dynamometers, p.target);
                p.source = p.quantity == "torque" ? Source::DynoTorque : p.quantity == "speed" ? Source::DynoSpeed : Source::DynoPower;
            }
            if (p.problem.empty() && p.index < 0)
            {
                p.problem = std::format("'{}' is not in the machine (the simulator refused the drawing, or it is not running)", p.target);
                p.source = Source::None;
            }
            p.raw.resize(kRawSamples);
            li.probes.push_back(std::move(p));
        }
        // A dynamometer reads its brake's torque, speed and held speed, and the chambers' work on its shaft, too.
        if (info.type == "dynamometer" && li.probes.size() == 1 && li.probes[0].source == Source::DynoPower)
            for (const auto& [source, dimension] : {std::pair{Source::DynoTorque, doc::Dimension::Torque}, std::pair{Source::DynoSpeed, doc::Dimension::AngularSpeed},
                                                    std::pair{Source::DynoTarget, doc::Dimension::AngularSpeed}, std::pair{Source::IndicatedWork, doc::Dimension::Ratio}})
            {
                InstrumentProbe q;
                q.source = source, q.index = li.probes[0].index, q.dimension = dimension, q.target = li.probes[0].target, q.hidden = true;
                q.raw.resize(kRawSamples);
                li.probes.push_back(std::move(q));
            }
        // The unit shown: the definition's, else the type's default when it fits, else the first of the dimension.
        const doc::Dimension shown = info.type == "oscillation"   ? doc::Dimension::Frequency
                                     : info.type == "timer"       ? (li.mode == "count"       ? doc::Dimension::Count
                                                                     : li.mode == "frequency" ? doc::Dimension::Frequency
                                                                                              : doc::Dimension::Time)
                                     : li.probes.empty()          ? doc::Dimension::Ratio
                                                                  : li.probes.front().dimension;
        const std::string wanted = d.value("unit", std::string(li.type->defaultUnit));
        li.unit = doc::FindUnit(shown, wanted).name;
        m_instruments.push_back(std::move(li));
    }
}

void InstrumentBank::Reset(const std::string& name)
{
    for (LiveInstrument& li : m_instruments)
        if (name.empty() || li.info.name == name)
            for (InstrumentProbe& p : li.probes)
            {
                p.rawCount = p.rawHead = 0;
                p.history.clear();
                p.bucketStart = -1.0;
                p.statCount = 0;
                p.crossings.clear();
                p.extremes.clear();
                p.direction = 0;
                p.armed = false;
                p.cycle = p.lastCycle = ChamberCycle();
                li.shown = Readout();
            }
}

// A chamber's events in one cycle: the intake closing, the ignition, the combustion's peak (and how far after top dead
// centre the crank was then), the exhaust opening, which ends the cycle.
static void SampleCycle(InstrumentProbe& p, double v, const sim::machine::Machine& machine, const MechanismStudy& study)
{
    const sim::machine::ChamberState& s = machine.Chamber(size_t(p.index));
    const int crank = study.machine.chambers[size_t(p.index)].crank;
    const double angle = crank >= 0 ? machine.Mechanism().Coordinate(crank) : NAN;
    const bool intake = s.intakeLift > 1e-6, exhaust = s.exhaustLift > 1e-6;
    if (p.intakeOpen && !intake)
        p.cycle.intakeCloses = v, p.tdcVolume = 1e30;
    if (!intake && !exhaust && s.volume < p.tdcVolume)
        p.tdcVolume = s.volume, p.tdcAngle = angle;
    if (s.combustion && !p.burning)
        p.cycle.ignition = v, p.cycle.peak = v;
    if (std::isfinite(p.cycle.ignition) && !exhaust && v >= p.cycle.peak)
        p.cycle.peak = v, p.cycle.peakAfterTdcDeg = angle;   // the crank's position for now, made relative at the end
    if (!p.exhaustOpen && exhaust)
    {
        p.cycle.exhaustOpens = v;
        if (crank >= 0 && std::isfinite(p.cycle.peakAfterTdcDeg) && p.tdcVolume < 1e30)
            p.cycle.peakAfterTdcDeg = (p.cycle.peakAfterTdcDeg - p.tdcAngle) * (machine.Mechanism().Rate(crank) < 0.0 ? -1.0 : 1.0) * 180.0 / std::numbers::pi;
        else
            p.cycle.peakAfterTdcDeg = NAN;
        p.lastCycle = p.cycle;
        p.cycle = ChamberCycle();
    }
    p.intakeOpen = intake, p.exhaustOpen = exhaust, p.burning = s.combustion;
}

void InstrumentBank::Sample(const sim::machine::Machine& machine, const MechanismStudy& study, double timeScale)
{
    const double t = machine.Time();
    m_now = t;
    m_scale = timeScale;
    for (LiveInstrument& li : m_instruments)
        for (InstrumentProbe& p : li.probes)
        {
            if (p.source == Source::None)
                continue;
            double v = ReadSource(p, machine, study);
            if (p.source == Source::JointAcceleration)
            {
                const double rate = v;
                v = p.lastT >= 0.0 && t > p.lastT ? (rate - p.lastRate) / (t - p.lastT) : 0.0;
                p.lastRate = rate;
            }
            if (p.source == Source::ChamberPressure || p.source == Source::ChamberTemperature)
                SampleCycle(p, v, machine, study);
            // Raw samples and statistics.
            p.raw[p.rawHead] = {t, v};
            p.rawHead = (p.rawHead + 1) % kRawSamples;
            p.rawCount = std::min(p.rawCount + 1, kRawSamples);
            if (p.statCount++ == 0)
                p.statMin = p.statMax = v, p.statSum = p.statSquares = 0.0;
            p.statMin = std::min(p.statMin, v), p.statMax = std::max(p.statMax, v);
            p.statSum += v, p.statSquares += v * v;
            // The chart's buckets.
            const double window = Window(li), bucket = window / double(kHistoryBuckets);
            if (p.bucketStart < 0.0)
                p.bucketStart = t, p.bucketMin = p.bucketMax = v, p.bucketSum = 0.0, p.bucketCount = 0;
            p.bucketMin = std::min(p.bucketMin, v), p.bucketMax = std::max(p.bucketMax, v), p.bucketSum += v, ++p.bucketCount;
            if (t - p.bucketStart >= bucket)
            {
                p.history.push_back({t, p.bucketMin, p.bucketMax, p.bucketSum / p.bucketCount});
                while (p.history.size() > 2 * kHistoryBuckets)
                    p.history.pop_front();
                // The swing over the window: its middle is the level events are timed at (bounces on a stop do not move it).
                double lo = p.history.back().min, hi = p.history.back().max;
                for (auto it = p.history.rbegin(); it != p.history.rend() && it->t >= t - window; ++it)
                    lo = std::min(lo, it->min), hi = std::max(hi, it->max);
                p.level = 0.5 * (lo + hi), p.span = hi - lo, p.levelKnown = true;
                p.bucketStart = t, p.bucketMin = p.bucketMax = v, p.bucketSum = 0.0, p.bucketCount = 0;
            }
            // Turning points: where the motion turns back by more than a tenth of its swing (a bounce on a stop is none).
            const double swing = p.levelKnown ? p.span : std::max(p.statMax - p.statMin, 0.0);
            const double hysteresis = std::max(swing * 0.1, 1e-15);
            if (p.direction >= 0 && v > p.runExtreme)
                p.runExtreme = v, p.direction = 1;
            else if (p.direction <= 0 && v < p.runExtreme)
                p.runExtreme = v, p.direction = -1;
            if (p.lastT < 0.0)
                p.runExtreme = v, p.direction = 0;
            else if (p.direction == 1 && v < p.runExtreme - hysteresis)
            {
                p.extremes.push_back({p.lastT, p.runExtreme}), p.direction = -1, p.runExtreme = v;
            }
            else if (p.direction == -1 && v > p.runExtreme + hysteresis)
            {
                p.extremes.push_back({p.lastT, p.runExtreme}), p.direction = 1, p.runExtreme = v;
            }
            else if (p.direction == 0 && std::abs(v - p.lastV) > 0.0)
                p.direction = v > p.lastV ? 1 : -1;
            while (p.extremes.size() > kEvents)
                p.extremes.pop_front();
            // Crossings of the threshold (given, or the middle of the last swing), timed between the steps; armed again
            // only once the signal has gone a tenth of the swing beyond it (no chatter).
            if (p.lastT >= 0.0)
            {
                double level = li.threshold;
                bool known = li.fixedThreshold;
                if (!known && p.levelKnown && p.span > 0.0)
                    level = p.level, known = true;
                if (known)
                {
                    const double arm = std::max(0.1 * swing, 1e-15);
                    if (std::abs(v - level) > arm)
                        p.armed = true;
                    if (p.armed && (p.lastV - level) * (v - level) < 0.0)
                    {
                        const double tc = p.lastT + (level - p.lastV) / (v - p.lastV) * (t - p.lastT);
                        p.crossings.push_back({tc, v > p.lastV});
                        p.armed = false;
                        while (p.crossings.size() > kEvents)
                            p.crossings.pop_front();
                    }
                }
            }
            p.lastT = t;
            p.lastV = v;
        }
}

Readout InstrumentBank::Measure(const LiveInstrument& li, double now) const
{
    Readout r;
    if (li.probes.empty())
    {
        r.status = "no probe";
        return r;
    }
    const InstrumentProbe& p = li.probes.front();
    if (p.source == Source::None)
    {
        r.status = p.problem;
        r.main = {"", 0.0, "", li.digits, false};
        return r;
    }
    const std::string type = li.info.type;
    const doc::Dimension dim = p.dimension;
    const bool samples = p.rawCount > 0;
    auto stats = [&](const InstrumentProbe& q, const std::string& unit, int digits) {
        if (q.statCount == 0)
            return;
        r.secondary.push_back(Make("Min", q.statMin, q.dimension, unit, digits));
        r.secondary.push_back(Make("Max", q.statMax, q.dimension, unit, digits));
        r.secondary.push_back(Make("Mean", q.statSum / double(q.statCount), q.dimension, unit, digits));
    };
    if (type == "oscillation")
    {
        // Crossings of the swing's middle: two per period. The period from same-direction crossings over the gate (at least
        // one cycle); beat error from the two half-periods; amplitude from the last high and low.
        std::vector<double> c;
        for (const InstrumentEvent& e : p.crossings)
            if (e.t >= now - std::max(Gate(li), 1e-9))
                c.push_back(e.t);
        if (c.size() < 3)
        {
            const size_t n = p.crossings.size();
            c.clear();
            for (size_t k = n >= 3 ? n - 3 : 0; k < n; ++k)
                c.push_back(p.crossings[k].t);
        }
        const std::string angleUnit = dim == doc::Dimension::Angle ? "deg" : "mm";
        if (c.size() < 3)
        {
            r.main = {"Beat rate", 0.0, doc::FindUnit(doc::Dimension::Frequency, li.unit).label, li.digits, false};
            r.status = "waiting for a full oscillation";
            return r;
        }
        const size_t cycles = (c.size() - 1) / 2;
        const double period = (c[2 * cycles] - c[0]) / double(cycles), f = 1.0 / period;
        r.main = Make(li.unit == "vph" ? "Beat rate" : "Frequency", f, doc::Dimension::Frequency, li.unit, li.digits);
        r.secondary.push_back(Make("Frequency", f, doc::Dimension::Frequency, "Hz", 5));
        r.secondary.push_back(Make("Period", period, doc::Dimension::Time, "ms", 3));
        const size_t n = c.size();
        const double h1 = c[n - 2] - c[n - 3], h2 = c[n - 1] - c[n - 2];
        r.secondary.push_back(Make("Beat error", std::abs(h1 - h2) / 2.0, doc::Dimension::Time, "ms", 2));
        if (p.extremes.size() >= 2)
        {
            const double a = p.extremes.back().v, b = p.extremes[p.extremes.size() - 2].v;
            r.secondary.push_back(Make("Amplitude", std::abs(a - b) / 2.0, dim, angleUnit, 1));
            r.secondary.push_back(Make("Centre", (a + b) / 2.0, dim, angleUnit, 2));
        }
        if (li.reference > 0.0)
            r.secondary.push_back({"Rate", (f * 7200.0 / li.reference - 1.0) * 86400.0, "s/day", 1, true});
        return r;
    }
    if (type == "dynamometer" && li.probes.size() >= 5)
    {
        // Brake power and torque over the gate; the indicated power from the chambers' work over the same time.
        const InstrumentProbe &torque = li.probes[1], &speed = li.probes[2], &target = li.probes[3], &work = li.probes[4];
        const double gate = std::max(Gate(li), 1e-9);
        double power = 0.0, t = 0.0, w = 0.0, held = 0.0, dw = 0.0, span = 0.0;
        const bool ok = samples && MeanSince(p, now - gate, power);
        r.main = Make("Brake power", power, doc::Dimension::Power, li.unit, li.digits, ok);
        r.secondary.push_back(Make("Torque", t, doc::Dimension::Torque, "N.m", 2, MeanSince(torque, now - gate, t)));
        r.secondary.push_back(Make("Speed", w, doc::Dimension::AngularSpeed, "rpm", 0, MeanSince(speed, now - gate, w)));
        r.secondary.push_back(Make("Held at", held, doc::Dimension::AngularSpeed, "rpm", 0, MeanSince(target, now - gate, held) && held > 0.0));
        const bool indicated = ChangeSince(work, now - gate, dw, span) && dw != 0.0;
        r.secondary.push_back(Make("Indicated", indicated ? dw / span : 0.0, doc::Dimension::Power, li.unit, li.digits, indicated));
        r.secondary.push_back({"Mech. efficiency", indicated && ok ? 100.0 * power / (dw / span) : 0.0, "%", 1, indicated && ok && dw > 0.0});
        // The best of the curve so far (the chart's buckets: power and speed side by side), where the brake held the
        // speed (within 2 %): the moment it engages it also takes the flywheel's energy, which is not the machine's power.
        double best = -1.0, bestAt = 0.0, most = -1.0, mostAt = 0.0;
        for (size_t k = 0; k < p.history.size() && k < speed.history.size() && k < torque.history.size() && k < target.history.size(); ++k)
        {
            if (!DynoHeld(speed.history[k].mean, target.history[k].mean))
                continue;
            if (p.history[k].mean > best)
                best = p.history[k].mean, bestAt = speed.history[k].mean;
            if (torque.history[k].mean > most)
                most = torque.history[k].mean, mostAt = speed.history[k].mean;
        }
        if (best > 0.0)
        {
            const doc::Unit& u = doc::FindUnit(doc::Dimension::Power, li.unit);
            r.secondary.push_back({std::format("Peak power (at {:.0f} rpm)", bestAt * 60.0 / (2.0 * std::numbers::pi)), best * u.scale + u.offset, u.label, li.digits, true});
            r.secondary.push_back({std::format("Peak torque (at {:.0f} rpm)", mostAt * 60.0 / (2.0 * std::numbers::pi)), most, "N·m", 2, true});
        }
        if (!ok)
            r.status = "no signal yet: run the machine";
        else if (held <= 0.0 && t <= 0.0)
            r.status = "not braking: the shaft is below the held speed";
        return r;
    }
    if (type == "timer")
    {
        std::vector<double> e;
        long long count = 0;
        for (const InstrumentEvent& ev : p.crossings)
            if (li.edge == "both" || ev.rising == (li.edge == "rise"))
            {
                ++count;
                if (ev.t >= now - Gate(li))
                    e.push_back(ev.t);
            }
        if (li.mode == "stopwatch")
            r.main = Make("Time", now, doc::Dimension::Time, li.unit, li.digits);
        else if (li.mode == "count")
            r.main = Make("Events", double(count), doc::Dimension::Count, li.unit, 0);
        else
        {
            if (e.size() < 2)
            {
                // Over the gate there were not two events: the last two seen.
                e.clear();
                for (auto it = p.crossings.rbegin(); it != p.crossings.rend() && e.size() < 2; ++it)
                    if (li.edge == "both" || it->rising == (li.edge == "rise"))
                        e.insert(e.begin(), it->t);
            }
            const bool ok = e.size() >= 2;
            const double interval = ok ? (e.back() - e.front()) / double(e.size() - 1) : 0.0;
            r.main = li.mode == "frequency" ? Make("Frequency", ok ? 1.0 / interval : 0.0, doc::Dimension::Frequency, li.unit, li.digits, ok)
                                            : Make("Interval", interval, doc::Dimension::Time, li.unit, li.digits, ok);
            if (!ok)
                r.status = "waiting for two events";
        }
        r.secondary.push_back({"Events", double(count), "", 0, true});
        if (!p.crossings.empty())
            r.secondary.push_back(Make("Last at", p.crossings.back().t, doc::Dimension::Time, "s", 4));
        return r;
    }
    // The meters: the mean over the gate (the last value with no gate); the voltmeter shows the last spark.
    double value = 0.0;
    const bool ok = samples && (type == "voltmeter" || li.gate <= 0.0 || !MeanSince(p, now - Gate(li), value) ? (value = p.lastV, true) : true);
    if (li.wrap && dim == doc::Dimension::Angle)
        value = std::fmod(std::fmod(value, 2.0 * std::numbers::pi) + 2.0 * std::numbers::pi, 2.0 * std::numbers::pi);
    static const std::pair<const char*, const char*> labels[] = {{"tachometer", "Speed"},   {"manometer", "Pressure"},  {"thermometer", "Temperature"},
                                                                {"protractor", "Angle"},   {"dial", "Displacement"},   {"gauge", "Force"},
                                                                {"voltmeter", "Spark"},    {"accelerometer", "Acceleration"}};
    std::string label = p.quantity;
    for (const auto& [t, l] : labels)
        if (type == t)
            label = l;
    if (type == "gauge" && dim == doc::Dimension::Torque)
        label = "Torque";
    r.main = Make(label, value, dim, li.unit, li.digits, ok);
    if (!ok)
        r.status = "no signal yet: run the machine";
    stats(p, li.unit, li.digits);
    // A chamber's gas at the events of its last cycle: what a reading at one moment cannot show at full speed.
    if ((p.source == Source::ChamberPressure || p.source == Source::ChamberTemperature) && type != "chart")
    {
        const ChamberCycle& c = p.lastCycle;
        r.secondary.push_back(Make("Intake closes", c.intakeCloses, dim, li.unit, li.digits, std::isfinite(c.intakeCloses)));
        r.secondary.push_back(Make("Ignition", c.ignition, dim, li.unit, li.digits, std::isfinite(c.ignition)));
        r.secondary.push_back(Make("Combustion peak", c.peak, dim, li.unit, li.digits, std::isfinite(c.peak)));
        r.secondary.push_back({"Peak after TDC", c.peakAfterTdcDeg, "°", 1, std::isfinite(c.peakAfterTdcDeg)});
        r.secondary.push_back(Make("Exhaust opens", c.exhaustOpens, dim, li.unit, li.digits, std::isfinite(c.exhaustOpens)));
    }
    if (type == "chart")
    {
        r.secondary.clear();
        for (size_t k = 0; k < li.probes.size(); ++k)
        {
            const InstrumentProbe& q = li.probes[k];
            double m = 0.0;
            const bool has = q.source != Source::None && MeanSince(q, now - Gate(li), m);
            r.secondary.push_back(Make(q.target + " " + q.quantity, m, q.dimension, k == 0 ? li.unit : std::string(), li.digits, has));
        }
    }
    return r;
}

Json InstrumentBank::Read(const std::string& name, double now) const
{
    Json list = Json::array();
    for (const LiveInstrument& li : m_instruments)
    {
        if (!name.empty() && li.info.name != name)
            continue;
        const Readout r = Measure(li, now);
        auto reading = [](const Reading& x) { return Json{{"label", x.label}, {"value", x.valid ? Json(x.value) : Json()}, {"unit", x.unit}, {"text", x.Text()}}; };
        Json secondary = Json::array();
        for (const Reading& x : r.secondary)
            secondary.push_back(reading(x));
        Json probes = Json::array();
        for (const InstrumentProbe& p : li.probes)
            if (!p.hidden)
                probes.push_back({{"target", p.target}, {"quantity", p.quantity}, {"unit_si", doc::SiUnit(p.dimension)}, {"samples", p.statCount},
                              {"problem", p.problem}});
        list.push_back({{"name", li.info.name}, {"type", li.info.type}, {"model", li.type->model}, {"visible", li.visible}, {"time_s", now},
                        {"main", reading(r.main)}, {"secondary", secondary}, {"status", r.status}, {"probes", probes}});
    }
    return list;
}

}  // namespace aiad::app
