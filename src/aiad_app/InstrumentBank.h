// The measuring instruments of the document, live: each probe reads the machine at every solver step (ideal: it
// changes nothing), keeps the raw samples, a history for the chart (min, max and mean per bucket), its statistics
// and its events (crossings of its midline or threshold, turning points), timed between steps by interpolation.
// Readings are computed from them for the panels and the agent. Sampling runs on the simulation thread under the
// session's lock; the UI reads under the same lock.
#pragma once

#include "Instruments.h"
#include "Json.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace aiad::doc
{
class Document;
}
namespace aiad::sim::machine
{
class Machine;
}

namespace aiad::app
{

struct MechanismStudy;

struct Reading
{
    std::string label;       // "Amplitude"
    double value = 0.0;      // in `unit`
    std::string unit;        // shown unit label
    int digits = 3;
    bool valid = false;
    std::string Text() const;   // value with its decimals (or "----")
};

struct Readout
{
    Reading main;
    std::vector<Reading> secondary;
    std::string status;      // "no signal", "waiting for two cycles"...
};

struct InstrumentSample
{
    double t, v;   // s, SI
};

struct InstrumentEvent
{
    double t;
    bool rising;
};

// What a chamber's gas was at the events of one cycle (NaN: not seen).
struct ChamberCycle
{
    double intakeCloses = NAN, ignition = NAN, peak = NAN, peakAfterTdcDeg = NAN, exhaustOpens = NAN;
};

struct InstrumentProbe
{
    // What it reads.
    enum class Source
    {
        None,
        JointPosition,
        JointSpeed,
        JointAcceleration,
        ChamberPressure,
        ChamberTemperature,
        ChamberVolume,
        MagnetoVoltage,
        TouchForce,
        ContactForce,
        SpringForce,
        TorsionTorque,
        DynoPower,
        DynoTorque,
        DynoSpeed,
        DynoTarget,
        IndicatedWork   // the work the chambers turning a dynamometer's shaft did since the start [J]
    } source = Source::None;
    bool hidden = false;   // read for the instrument's own readouts (a dynamometer's torque, speed...), not shown as a probe
    int index = -1;                  // the joint, chamber, magneto, touch, contact, spring or torsion
    doc::Dimension dimension = doc::Dimension::Ratio;
    std::string target, quantity, problem;   // problem: why it reads nothing
    // Raw samples (every step, the last ones), history buckets (the chart), statistics since the last reset.
    std::vector<InstrumentSample> raw;
    size_t rawHead = 0, rawCount = 0;
    struct Bucket
    {
        double t, min, max, mean;
    };
    std::deque<Bucket> history;
    double bucketStart = -1.0, bucketMin = 0.0, bucketMax = 0.0, bucketSum = 0.0;
    int bucketCount = 0;
    double statMin = 0.0, statMax = 0.0, statSum = 0.0, statSquares = 0.0;
    long long statCount = 0;
    // Events: crossings of the threshold (or of the swing's middle), and turning points.
    std::deque<InstrumentEvent> crossings;
    std::deque<InstrumentSample> extremes;   // turning points (alternating highs and lows)
    double level = 0.0, span = 0.0;           // the swing's middle and size over the chart's window (from the history)
    bool levelKnown = false;
    double lastT = -1.0, lastV = 0.0, lastRate = 0.0;
    double runExtreme = 0.0;
    int direction = 0;                        // +1 rising, -1 falling, 0 unknown
    bool armed = false;
    // A chamber's pressure or temperature: the cycle being seen and the last complete one.
    ChamberCycle cycle, lastCycle;
    bool intakeOpen = false, exhaustOpen = false, burning = false;
    double tdcVolume = 1e30, tdcAngle = 0.0;
};

struct LiveInstrument
{
    doc::InstrumentInfo info;
    const doc::InstrumentType* type = nullptr;
    std::vector<InstrumentProbe> probes;
    // Settings (from the definition).
    std::string unit, mode, edge;
    int digits = 3;
    double gate = 0.5, window = 10.0, reference = 0.0;
    bool visible = true, wrap = false, fixedThreshold = false;
    double threshold = 0.0;
    // The panel's state.
    bool hold = false;
    Readout shown;
    double refreshedAt = -1.0;
    bool detached = false;   // in a window of its own, out of the main one (square corners there)
    unsigned colour = 0;
};

// A dynamometer's point of its curve: the brake held the shaft at the speed it was set to (within 2 %).
bool DynoHeld(double speed, double target);

class InstrumentBank
{
public:
    // From the document's instruments and the machine's study: each probe finds what it reads (or says why not).
    void Build(const doc::Document& document, const MechanismStudy& study);
    // Every solver step (simulation thread, under the session's lock). timeScale: machine seconds per wall second.
    void Sample(const sim::machine::Machine& machine, const MechanismStudy& study, double timeScale);
    // Gate and chart window are what the user watches, in wall time: in slow motion they cover that much less machine
    // time (at 0.002x a 500 ms gate averages 1 ms of the machine, so a reading follows each moment of a cycle).
    double Gate(const LiveInstrument& li) const { return li.gate * Slow(); }
    double Window(const LiveInstrument& li) const { return li.window * Slow(); }
    double Slow() const { return m_scale > 0.0 && m_scale < 1.0 ? m_scale : 1.0; }
    // Statistics, history and events of one instrument (or all) from now on.
    void Reset(const std::string& name = std::string());
    // The readings at the machine's time `now` (computed from the samples; HOLD keeps the panel's).
    Readout Measure(const LiveInstrument& instrument, double now) const;
    Json Read(const std::string& name, double now) const;   // for the agent: readings, statistics, status
    std::vector<LiveInstrument>& Instruments() { return m_instruments; }
    const std::vector<LiveInstrument>& Instruments() const { return m_instruments; }
    double Now() const { return m_now; }

private:
    std::vector<LiveInstrument> m_instruments;
    double m_now = 0.0, m_scale = 1.0;
};

}  // namespace aiad::app
