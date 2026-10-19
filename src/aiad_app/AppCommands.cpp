#include "AppCommands.h"

#include <cmath>
#include <format>
#include <numbers>

namespace aiad::app
{

namespace
{

Json NoArgs()
{
    return Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
}

// A scene part by index or name (exact, then unique case-insensitive fragment).
int FindPart(const DocumentScene& scene, const Json& ref, std::string& error)
{
    const auto& parts = scene.Parts();
    if (ref.is_number_integer())
    {
        const int i = ref.get<int>();
        if (i >= 0 && i < int(parts.size()))
            return i;
        error = std::format("part index {} out of range (0..{})", i, parts.size() - 1);
        return -1;
    }
    const std::string name = ref.is_string() ? ref.get<std::string>() : std::string();
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    std::vector<int> partial;
    for (int i = 0; i < int(parts.size()); ++i)
    {
        if (parts[size_t(i)].name == name)
            return i;
        std::string lower = parts[size_t(i)].name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (!key.empty() && lower.find(key) != std::string::npos)
            partial.push_back(i);
    }
    if (partial.size() == 1)
        return partial[0];
    std::string candidates;
    for (const int i : partial)
        candidates += (candidates.empty() ? "" : ", ") + parts[size_t(i)].name;
    error = partial.empty() ? std::format("no part named '{}' (aiad_parts_list shows the names)", name)
                            : std::format("'{}' matches several parts: {}", name, candidates);
    return -1;
}

bool Rotational(sim::mb::JointType t)
{
    return t == sim::mb::JointType::Revolute || t == sim::mb::JointType::Cylindrical;
}

// The machine of the open document as it is now (a fresh study) with the field tables it needs: the
// session's, when they were computed for the same drawn magneto geometry.
bool MachineToRun(const Session& session, MechanismStudy& study, std::string& error)
{
    if (!session.document)
    {
        error = "No document is open.";
        return false;
    }
    if (!DeriveMechanismStudy(*session.document, study))
    {
        error = "The document has no joints: it is not a mechanism yet (aiad_mechanism_check says what to add).";
        return false;
    }
    if (!study.Runnable())
    {
        error = std::format("The machine cannot run; the simulator found {} problem(s) in the drawing:\n{}", study.Errors(), study.Report());
        return false;
    }
    bool tables = session.tableGeometry.size() == study.machine.magnetos.size() && session.magnetoTables.size() == study.machine.magnetos.size();
    for (size_t i = 0; tables && i < study.machine.magnetos.size(); ++i)
        tables = std::memcmp(&session.tableGeometry[i], &study.machine.magnetos[i].geometry, sizeof(sim::magneto::MagnetoGeometry)) == 0;
    if (!tables)
    {
        error = "The magneto field table for this drawing is being computed (MFEM, about a minute the first time): try again shortly.";
        return false;
    }
    return true;
}

}  // namespace

Json RunMachine(const MechanismStudy& study, const std::vector<std::vector<sim::magneto::MagnetoSample>>& tables, double seconds, std::string& text,
                std::atomic<bool>* stop, std::function<void(double)> progress)
{
    sim::machine::Machine machine(study.machine, tables);
    const sim::mb::Model& model = study.machine.mechanism;
    const sim::mb::Multibody& mb = machine.Mechanism();
    const double dt = machine.StepSize(), window = std::min(0.5, seconds / 2.0), windowStart = seconds - window;
    const size_t joints = model.joints.size(), chambers = study.machine.chambers.size(), magnetos = study.machine.magnetos.size();
    std::vector<double> lo(joints, 1e300), hi(joints, -1e300), rate(joints, 0.0), workAt(chambers, 0.0), maxKv(magnetos, 0.0);
    std::vector<int> combustionsAt(chambers, 0);
    int samples = 0;
    bool inWindow = false;
    for (int step = 0; machine.Time() < seconds - dt / 2.0 && !(stop && *stop); ++step)
    {
        machine.Step(dt);
        for (size_t j = 0; j < joints; ++j)
        {
            lo[j] = std::min(lo[j], mb.Coordinate(int(j)));
            hi[j] = std::max(hi[j], mb.Coordinate(int(j)));
        }
        for (size_t g = 0; g < magnetos; ++g)
            maxKv[g] = std::max(maxKv[g], machine.Magneto(g).lastSecondaryKv);
        if (machine.Time() >= windowStart)
        {
            if (!inWindow)
                for (size_t c = 0; c < chambers; ++c)
                    workAt[c] = machine.Chamber(c).work, combustionsAt[c] = machine.Chamber(c).combustions;
            inWindow = true;
            for (size_t j = 0; j < joints; ++j)
                rate[j] += mb.Rate(int(j));
            ++samples;
        }
        if (progress && (step & 2047) == 0)
            progress(machine.Time() / seconds);
    }
    const double t = machine.Time();
    text = std::format("Simulated {:.3f} s (step {:.1e} s).", t, dt);
    Json jointList = Json::array(), chamberList = Json::array(), magnetoList = Json::array(), bodies = Json::array();
    for (size_t j = 0; j < joints; ++j)
    {
        const sim::mb::Joint& joint = model.joints[j];
        const double mean = samples ? rate[j] / samples : 0.0;
        Json item = {{"name", joint.name}, {"coordinate", mb.Coordinate(int(j))}, {"min", lo[j]}, {"max", hi[j]}, {"mean_rate", mean}};
        if (Rotational(joint.type))
        {
            item["mean_rpm"] = mean * 30.0 / std::numbers::pi;
            text += std::format(" '{}': {:.0f} rpm (mean of the last {:.2f} s), turned {:.1f} rev.", joint.name, mean * 30.0 / std::numbers::pi, window,
                                mb.Coordinate(int(j)) / (2.0 * std::numbers::pi));
        }
        else if (joint.type == sim::mb::JointType::Prismatic)
            text += std::format(" '{}': {:.2f} .. {:.2f} mm.", joint.name, lo[j] * 1e3, hi[j] * 1e3);
        jointList.push_back(item);
    }
    for (size_t c = 0; c < chambers; ++c)
    {
        const sim::machine::ChamberState& s = machine.Chamber(c);
        const double displacement = s.maxVolume - s.minVolume, ratio = s.minVolume > 0.0 ? s.maxVolume / s.minVolume : 0.0;
        const double power = window > 0.0 ? (s.work - workAt[c]) / window : 0.0;
        chamberList.push_back({{"name", study.machine.chambers[c].name},
                               {"displacement_cm3", displacement * 1e6},
                               {"compression_ratio", ratio},
                               {"peak_pressure_bar", s.peakPressure / 1e5},
                               {"combustions", s.combustions},
                               {"combustions_last_window", s.combustions - combustionsAt[c]},
                               {"indicated_power_kw", power / 1e3}});
        text += std::format(" Chamber '{}': displacement {:.1f} cm^3, compression ratio {:.2f}, peak {:.1f} bar, {} combustions ({} in the last {:.2f} s), "
                            "indicated power {:.2f} kW.",
                            study.machine.chambers[c].name, displacement * 1e6, ratio, s.peakPressure / 1e5, s.combustions, s.combustions - combustionsAt[c], window,
                            power / 1e3);
    }
    for (size_t g = 0; g < magnetos; ++g)
    {
        magnetoList.push_back({{"name", study.machine.magnetos[g].name}, {"sparks", machine.Magneto(g).sparks}, {"max_secondary_kv", maxKv[g]}});
        text += std::format(" Magneto '{}': {} sparks, secondary up to {:.1f} kV.", study.machine.magnetos[g].name, machine.Magneto(g).sparks, maxKv[g]);
    }
    for (size_t b = 0; b < model.bodies.size(); ++b)
    {
        if (model.bodies[b].fixed)
            continue;
        const sim::mb::BodyState st = mb.State(int(b));
        const auto& c = model.bodies[b].centroid;
        bodies.push_back({{"body", model.bodies[b].name},
                          {"displacement_m", std::hypot(st.centroid[0] - c[0], st.centroid[1] - c[1], st.centroid[2] - c[2])},
                          {"rotation_rad", 2.0 * std::acos(std::clamp(std::abs(st.rotation[0]), 0.0, 1.0))}});
    }
    // Timed motors (starters): does the machine keep going once they stop?
    Json result = {{"time_s", t}, {"step_s", dt}, {"degrees_of_freedom", study.mobility}, {"joints", jointList}, {"chambers", chamberList},
                   {"magnetos", magnetoList}, {"bodies", bodies}};
    bool timed = false, keeps = true;
    for (size_t j = 0; j < joints; ++j)
    {
        const sim::mb::Motor& motor = model.joints[j].motor;
        if (motor.mode != sim::mb::Motor::Mode::Torque || motor.duration <= 0.0)
            continue;
        timed = true;
        const double rpm = samples ? std::abs(rate[j] / samples) * 30.0 / std::numbers::pi : 0.0;
        const bool after = windowStart >= motor.duration;
        keeps = keeps && after && rpm > 300.0;
        text += after ? std::format(" After its motor stopped at {:.2f} s, '{}' {} ({:.0f} rpm).", motor.duration, model.joints[j].name,
                                    rpm > 300.0 ? "keeps turning on its own" : "does NOT keep turning", rpm)
                      : std::format(" The motor of '{}' runs until {:.2f} s: simulate longer to see whether it keeps turning without it.", model.joints[j].name,
                                    motor.duration);
    }
    if (timed)
        result["runs_on_its_own"] = keeps;
    return result;
}

SimulationJobs::~SimulationJobs()
{
    m_stop = true;
    for (auto& t : m_threads)
        if (t.joinable())
            t.join();
}

int SimulationJobs::Start(const MechanismStudy& study, const std::vector<std::vector<sim::magneto::MagnetoSample>>& tables, double seconds)
{
    auto job = std::make_shared<SimulationJob>();
    {
        std::lock_guard lock(m_mutex);
        job->id = m_next++;
        job->seconds = seconds;
        m_jobs[job->id] = job;
    }
    m_threads.emplace_back([this, job, study, tables, seconds] {
        try
        {
            std::string text;
            Json result = RunMachine(study, tables, seconds, text, &m_stop, [&](double progress) {
                std::lock_guard lock(m_mutex);
                job->progress = progress;
            });
            std::lock_guard lock(m_mutex);
            job->result = std::move(result);
            job->text = std::move(text);
            job->progress = 1.0;
            job->state = "done";
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(m_mutex);
            job->state = "failed";
            job->error = e.what();
        }
    });
    return job->id;
}

std::optional<SimulationJob> SimulationJobs::Get(int id) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_jobs.find(id);
    return it == m_jobs.end() ? std::nullopt : std::optional<SimulationJob>(*it->second);
}

void RegisterAppCommands(cmd::CommandBus& bus, AppContext& context, SimulationJobs& jobs)
{
    using cmd::CommandOutput;
    AppContext* ctx = &context;
    auto ready = [ctx]() { return ctx->session && ctx->session->scene; };
    auto notReady = [] { return CommandOutput::Fail("The model is still loading (view, magneto field table). Try again in a few seconds."); };

    // ---- Machine (generic: any machine, from the bodies, joints and elements of the document) -------------
    bus.Register({"aiad_mechanism_check", "Check the machine",
                  "Criticises the machine of the drawing (any machine): bodies (mass from their parts), joints, what holds it to the ground, "
                  "bodies not connected, joints outside the parts they join, degrees of freedom, nothing driving it; and every element: gears that "
                  "do not mesh, belts that miss their wheels, cams that never reach (or never release) their followers, springs that do not fit "
                  "their seats, valves without seat, spring or drive, chambers and magnetos that do not match the drawing. Every problem comes "
                  "with its fix, and everything measured on the drawing is listed. The machine runs only when no error is left.",
                  NoArgs(), {true, false, true}, [ctx](const Json&) {
                      if (!ctx->session || !ctx->session->document)
                          return CommandOutput::Fail("No document is open.");
                      MechanismStudy s;
                      CommandOutput o;
                      if (!DeriveMechanismStudy(*ctx->session->document, s))
                      {
                          o.result = {{"mechanism", false}};
                          o.text = "The document has no joints, so it is not a mechanism yet: every body is free. Make the base fixed "
                                   "(aiad_body_define fixed: true), join the bodies (aiad_joint_define) and add what else it is made of "
                                   "(aiad_element_define: gears, belts, cams, springs, loads, chambers, magnetos).";
                          return o;
                      }
                      Json findings = Json::array(), bodies = Json::array();
                      for (const StudyFinding& f : s.findings)
                          findings.push_back({{"error", f.error}, {"problem", f.problem}, {"fix", f.fix}});
                      for (const sim::mb::Body& b : s.machine.mechanism.bodies)
                          bodies.push_back({{"name", b.name}, {"fixed", b.fixed}, {"mass_kg", b.mass}, {"centroid_m", b.centroid}});
                      const sim::mb::Model& m = s.machine.mechanism;
                      o.result = {{"mechanism", true},
                                  {"runnable", s.Runnable()},
                                  {"degrees_of_freedom", s.mobility},
                                  {"bodies", bodies},
                                  {"couplings", m.couplings.size()},
                                  {"contacts", m.contacts.size()},
                                  {"springs", m.springs.size()},
                                  {"chambers", s.machine.chambers.size()},
                                  {"magnetos", s.machine.magnetos.size()},
                                  {"measured", s.measured},
                                  {"findings", findings}};
                      std::string measured;
                      for (const std::string& line : s.measured)
                          measured += "- " + line + "\n";
                      o.text = std::format("{} bodies, {} joints, {} couplings, {} contacts, {} springs, {} chambers, {} magnetos; {} degree(s) of freedom.\n{}{}",
                                           m.bodies.size(), m.joints.size(), m.couplings.size(), m.contacts.size(), m.springs.size(), s.machine.chambers.size(),
                                           s.machine.magnetos.size(), s.mobility, measured.empty() ? "" : "Measured on the drawing:\n" + measured,
                                           s.findings.empty() ? std::string("No problem found: it runs (aiad_mechanism_run, aiad_sim_run).")
                                           : s.Runnable()     ? "It runs; notes:\n" + s.Report()
                                                              : std::format("It will not run until these {} problem(s) are fixed:\n{}", s.Errors(), s.Report()));
                      return o;
                  }});
    bus.Register({"aiad_mechanism_run", "Run the machine",
                  "Simulates the machine of the drawing (any machine: bodies, joints, motors, couplings, contacts, springs, loads, chambers, "
                  "magnetos) for a short time and returns where every joint ended (its range and mean speed), the chambers and magnetos, and how "
                  "far every moving body moved. For long runs use aiad_sim_run (a background job).",
                  Json{{"type", "object"},
                       {"properties", {{"seconds", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 5}, {"description", "simulated time [s]"}}}}},
                       {"required", {"seconds"}},
                       {"additionalProperties", false}},
                  {true, false, true}, [ctx](const Json& args) {
                      MechanismStudy s;
                      std::string error;
                      if (!ctx->session || !MachineToRun(*ctx->session, s, error))
                          return CommandOutput::Fail(error.empty() ? std::string("No document is open.") : error);
                      // The run itself needs no document: it may go off the main thread (the bridge runs it).
                      CommandOutput o;
                      o.background = [s, tables = ctx->session->magnetoTables, seconds = args["seconds"].get<double>()](CommandOutput& out) {
                          out.result = RunMachine(s, tables, seconds, out.text);
                      };
                      return o;
                  }});
    // ---- View --------------------------------------------------------------------------------------
    bus.Register({"aiad_part_set_visible", "Show / hide part", "Shows or hides a part in the viewport (by index or name). Undoable.",
                  Json{{"type", "object"},
                       {"properties", {{"part", {{"type", {"integer", "string"}}, {"description", "index or name"}}}, {"visible", {{"type", "boolean"}}}}},
                       {"required", {"part", "visible"}}},
                  {false, false, true}, [ctx, ready, notReady](const Json& args) {
                      if (!ready())
                          return notReady();
                      std::string error;
                      const int part = FindPart(*ctx->session->scene, args["part"], error);
                      if (part < 0)
                          return CommandOutput::Fail(error);
                      const std::string name = ctx->session->scene->Parts()[size_t(part)].name;
                      const bool before = ctx->session->scene->Parts()[size_t(part)].visible, after = args["visible"];
                      ctx->session->scene->SetVisible(part, after);
                      CommandOutput o;
                      o.label = (after ? "Show " : "Hide ") + name;
                      o.text = o.label + ".";
                      // By name: the part index changes when the document changes.
                      auto set = [ctx, name](bool visible) {
                          if (ctx->session->scene)
                              ctx->session->scene->SetVisible(ctx->session->scene->Find(name), visible);
                      };
                      o.undo = [set, before] { set(before); };
                      o.redo = [set, after] { set(after); };
                      return o;
                  }});

    bus.Register({"aiad_selection_set", "Select part", "Selects (highlights) a part by index or name; null clears the selection.",
                  Json{{"type", "object"}, {"properties", {{"part", {{"type", {"integer", "string", "null"}}}}}}, {"required", {"part"}}}, {false, false, true},
                  [ctx, ready, notReady](const Json& args) {
                      if (!ready())
                          return notReady();
                      CommandOutput o;
                      if (args["part"].is_null())
                      {
                          *ctx->selected = -1;
                          o.text = "Selection cleared.";
                          return o;
                      }
                      std::string error;
                      const int part = FindPart(*ctx->session->scene, args["part"], error);
                      if (part < 0)
                          return CommandOutput::Fail(error);
                      *ctx->selected = part;
                      o.text = "Selected " + ctx->session->scene->Parts()[size_t(part)].name + ".";
                      return o;
                  }});

    bus.Register({"aiad_view_set", "Set view",
                  "Standard view (iso, front, back, right, left, top), fit all, section view on/off. Only the camera and display change.",
                  Json{{"type", "object"},
                       {"properties",
                        {{"view", {{"type", "string"}, {"enum", {"iso", "front", "back", "right", "left", "top"}}}},
                         {"fit", {{"type", "boolean"}}},
                         {"section", {{"type", "boolean"}, {"description", "cut the housings to show the inside"}}}}},
                       {"additionalProperties", false}},
                  {false, false, true}, [ctx, ready, notReady](const Json& args) {
                      if (!ready())
                          return notReady();
                      static const std::map<std::string, std::pair<double, double>> views = {{"iso", {-45.0, 35.264}}, {"front", {-90.0, 0.0}},
                                                                                             {"back", {90.0, 0.0}},     {"right", {0.0, 0.0}},
                                                                                             {"left", {180.0, 0.0}},    {"top", {-90.0, 89.9}}};
                      if (args.contains("view"))
                      {
                          const auto& [yaw, pitch] = views.at(args["view"].get<std::string>());
                          ctx->camera->yaw = yaw;
                          ctx->camera->pitch = pitch;
                      }
                      if (args.value("fit", false))
                          ctx->camera->Fit(ctx->session->scene->WorldBounds());
                      if (args.contains("section"))
                          ctx->session->scene->SetSectionView(args["section"].get<bool>());
                      CommandOutput o;
                      o.text = "View updated.";
                      return o;
                  }});

    bus.Register({"aiad_viewport_snapshot", "Viewport snapshot",
                  "PNG of the 3D viewport exactly as the user sees it now (path traced). Use it to check a change.", NoArgs(), {true, false, false},
                  [ctx, ready, notReady](const Json&) {
                      if (!ready())
                          return notReady();
                      std::vector<uint8_t> png = ctx->snapshot ? ctx->snapshot() : std::vector<uint8_t>();
                      if (png.empty())
                          return CommandOutput::Fail("The viewport has no image yet (is the Viewport panel visible?).");
                      CommandOutput o;
                      o.text = "Viewport snapshot.";
                      o.images.push_back({"image/png", std::move(png)});
                      return o;
                  }});

    // ---- Simulation ----------------------------------------------------------------------------
    bus.Register({"aiad_sim_start", "Start simulation (live)", "Starts (or restarts) the live simulation of the machine shown in the viewport (slow motion).",
                  NoArgs(), {}, [ctx, ready, notReady](const Json&) {
                      if (!ready())
                          return notReady();
                      if (ctx->session->studyStale)
                          ctx->session->Build();
                      if (!ctx->session->studyError.empty())
                          return CommandOutput::Fail("The machine cannot run: " + ctx->session->studyError);
                      if (!ctx->session->TablesReady())
                          return notReady();
                      ctx->session->Restart();
                      ctx->session->running = true;
                      CommandOutput o;
                      o.text = "Running (live view). aiad_sim_status reads it; aiad_sim_run for a measured run.";
                      return o;
                  }});
    bus.Register({"aiad_sim_stop", "Stop simulation (live)", "Stops the live simulation.", NoArgs(), {false, false, true}, [ctx](const Json&) {
                      ctx->session->running = false;
                      CommandOutput o;
                      o.text = "Stopped.";
                      return o;
                  }});
    bus.Register({"aiad_sim_status", "Live simulation status",
                  "State of the live simulation: time, every joint (position and speed), chambers (pressure, volume, combustions), magnetos.",
                  NoArgs(), {true, false, false}, [ctx, ready, notReady](const Json&) {
                      if (!ready())
                          return notReady();
                      const Session& s = *ctx->session;
                      const auto lock = s.Lock();
                      CommandOutput o;
                      Json joints = Json::array(), chambers = Json::array(), magnetos = Json::array();
                      std::string text = std::format("{}; t = {:.4f} s.", s.running ? "Running" : "Stopped", s.Time());
                      if (s.machine)
                      {
                          const sim::mb::Multibody& mb = s.machine->Mechanism();
                          const auto& m = s.study.machine.mechanism;
                          for (size_t j = 0; j < m.joints.size(); ++j)
                          {
                              joints.push_back({{"name", m.joints[j].name}, {"coordinate", mb.Coordinate(int(j))}, {"rate", mb.Rate(int(j))}});
                              if (Rotational(m.joints[j].type))
                                  text += std::format(" '{}' {:.1f} deg at {:.0f} rpm;", m.joints[j].name, mb.Coordinate(int(j)) * 180.0 / std::numbers::pi,
                                                      mb.Rate(int(j)) * 30.0 / std::numbers::pi);
                          }
                          for (size_t c = 0; c < s.study.machine.chambers.size(); ++c)
                          {
                              const auto& st = s.machine->Chamber(c);
                              chambers.push_back({{"name", s.study.machine.chambers[c].name}, {"pressure_bar", st.pressure / 1e5}, {"volume_cm3", st.volume * 1e6},
                                                  {"combustions", st.combustions}});
                              text += std::format(" chamber '{}' {:.1f} bar, {} combustions;", s.study.machine.chambers[c].name, st.pressure / 1e5, st.combustions);
                          }
                          for (size_t g = 0; g < s.study.machine.magnetos.size(); ++g)
                              magnetos.push_back({{"name", s.study.machine.magnetos[g].name}, {"last_kv", s.machine->Magneto(g).lastSecondaryKv},
                                                  {"sparks", s.machine->Magneto(g).sparks}});
                      }
                      o.result = {{"running", s.running.load()}, {"time_s", s.Time()}, {"joints", joints}, {"chambers", chambers}, {"magnetos", magnetos}};
                      o.text = text;
                      return o;
                  }});
    bus.Register({"aiad_sim_run", "Run simulation (measured)",
                  "Runs the machine of the current drawing in the background for the given simulated seconds (motors, gas, sparks included) "
                  "and measures it: joint speeds, chambers (displacement, compression ratio, peak pressure, combustions, indicated power), "
                  "magnetos, and whether it keeps going after its timed motors stop. Returns a job id; poll aiad_sim_job_status.",
                  Json{{"type", "object"},
                       {"properties", {{"seconds", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 30}, {"description", "simulated time [s]"}}}}},
                       {"additionalProperties", false}},
                  {true, false, false}, [ctx, &jobs](const Json& args) {
                      MechanismStudy s;
                      std::string error;
                      if (!ctx->session || !MachineToRun(*ctx->session, s, error))
                          return CommandOutput::Fail(error.empty() ? std::string("No document is open.") : error);
                      const double seconds = args.value("seconds", 3.0);
                      const int id = jobs.Start(s, ctx->session->magnetoTables, seconds);
                      CommandOutput o;
                      o.result = {{"job", id}};
                      o.text = std::format("Simulation job {} started ({:.1f} s simulated). Poll aiad_sim_job_status with job = {}.", id, seconds, id);
                      return o;
                  }});
    bus.Register({"aiad_sim_job_status", "Simulation job status", "Progress and, when done, the results of a simulation job.",
                  Json{{"type", "object"}, {"properties", {{"job", {{"type", "integer"}, {"minimum", 1}}}}}, {"required", {"job"}}}, {true, false, true},
                  [&jobs](const Json& args) {
                      const auto job = jobs.Get(args["job"].get<int>());
                      if (!job)
                          return CommandOutput::Fail(std::format("No simulation job {}.", args["job"].get<int>()));
                      CommandOutput o;
                      o.result = {{"job", job->id}, {"state", job->state}, {"progress", job->progress}, {"result", job->result}};
                      o.text = job->state == "running" ? std::format("Job {} running: {:.0f}%.", job->id, job->progress * 100)
                               : job->state == "done"  ? job->text
                                                       : "Job failed: " + job->error;
                      if (job->state == "failed")
                          return CommandOutput::Fail(o.text);
                      return o;
                  }});
}

}  // namespace aiad::app
