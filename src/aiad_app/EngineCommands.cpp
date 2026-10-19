#include "EngineCommands.h"

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

}  // namespace

SimulationJobs::~SimulationJobs()
{
    m_stop = true;
    for (auto& t : m_threads)
        if (t.joinable())
            t.join();
}

int SimulationJobs::Start(const sim::engine::EngineDesign& design, const std::vector<sim::magneto::MagnetoSample>& magneto, double seconds)
{
    auto job = std::make_shared<SimulationJob>();
    {
        std::lock_guard lock(m_mutex);
        job->id = m_next++;
        job->seconds = seconds;
        m_jobs[job->id] = job;
    }
    m_threads.emplace_back([this, job, design, magneto, seconds] {
        try
        {
            sim::engine::EngineSimulation simulation(design, magneto);
            const double dt = 2.0e-5;
            double peakPressure = 0.0, rpmSum = 0.0, torqueSum = 0.0, maxKv = 0.0;
            int samples = 0, combustions = 0;
            bool burning = false;
            sim::engine::EngineSample s;
            while (s.time < seconds && !m_stop)
            {
                s = simulation.Step(dt);
                peakPressure = std::max(peakPressure, s.pressure);
                maxKv = std::max(maxKv, s.secondaryPeak / 1e3);
                if (s.combustion && !burning)
                    ++combustions;
                burning = s.combustion;
                if (s.time > seconds - 0.5)
                {
                    rpmSum += s.rpm;
                    torqueSum += s.gasTorque;
                    ++samples;
                }
                if ((int(s.time / dt) & 1023) == 0)
                {
                    std::lock_guard lock(m_mutex);
                    job->progress = s.time / seconds;
                }
            }
            const double meanRpm = samples ? rpmSum / samples : 0.0, meanTorque = samples ? torqueSum / samples : 0.0;
            const double powerKw = meanTorque * meanRpm * 2.0 * std::numbers::pi / 60.0 / 1e3;
            const bool running = seconds > design.starterDuration + 0.3 && meanRpm > 1000.0;
            std::lock_guard lock(m_mutex);
            job->result = {{"simulated_seconds", s.time},  {"final_rpm", s.rpm},        {"mean_rpm_last_0_5s", meanRpm},
                           {"mean_gas_torque_nm", meanTorque}, {"indicated_power_kw", powerKw}, {"peak_pressure_bar", peakPressure / 1e5},
                           {"combustions", combustions},   {"magneto_peak_kv", maxKv},  {"runs_on_its_own", running}};
            job->text = std::format("Simulated {:.2f} s: {} ({:.0f} rpm at the end, mean {:.0f} rpm over the last 0.5 s). Indicated power {:.2f} kW, "
                                    "mean gas torque {:.2f} N m, peak cylinder pressure {:.1f} bar, {} combustions, magneto secondary up to {:.1f} kV.",
                                    s.time, running ? "the engine runs on its own" : "the engine does NOT sustain itself", s.rpm, meanRpm, powerKw,
                                    meanTorque, peakPressure / 1e5, combustions, maxKv);
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

void RegisterEngineCommands(cmd::CommandBus& bus, AppContext& context, SimulationJobs& jobs)
{
    using cmd::CommandOutput;
    AppContext* ctx = &context;
    auto ready = [ctx]() { return ctx->session && ctx->session->scene; };
    auto notReady = [] { return CommandOutput::Fail("The model is still loading (magneto field table). Try again in a few seconds."); };
    auto noStudy = [ctx]() { return CommandOutput::Fail("The document cannot be simulated: " + ctx->session->studyError); };

    bus.Register({"aiad_engine_get_derived", "Get derived physics",
                  "What the simulation uses, all read from the drawing: displacement, chamber volume, masses and inertias of the moving bodies "
                  "(from the parts and their materials), valve lift measured on the drawn cam lobes, magneto timing.",
                  NoArgs(), {true, false, true}, [ctx, ready, notReady, noStudy](const Json&) {
                      if (!ready())
                          return notReady();
                      if (!ctx->session->studyError.empty())
                          return noStudy();
                      const auto& sd = ctx->session->study.simulation;
                      const auto& g = sd.cylinder.geometry;
                      const double inLift = *std::max_element(sd.intakeLiftTable.begin(), sd.intakeLiftTable.end());
                      const double exLift = *std::max_element(sd.exhaustLiftTable.begin(), sd.exhaustLiftTable.end());
                      CommandOutput o;
                      o.result = {{"displacement_cm3", g.DisplacedVolume() * 1e6},
                                  {"clearance_volume_cm3", g.DisplacedVolume() / (g.compressionRatio - 1.0) * 1e6},
                                  {"crankshaft_inertia_kgm2", sd.crankshaftInertia},
                                  {"rod_mass_kg", sd.rodMass},
                                  {"rod_com_from_crank_pin_mm", sd.rodCentreOfMass * 1e3},
                                  {"piston_mass_kg", sd.pistonMass},
                                  {"max_intake_lift_mm", inLift * 1e3},
                                  {"max_exhaust_lift_mm", exLift * 1e3},
                                  {"points_open_deg", sd.ignition.pointsOpenDeg},
                                  {"magnet_mount_deg", sd.magnetMountDeg}};
                      o.text = std::format("Displacement {:.1f} cm^3 (bore {:.1f} x stroke {:.1f} mm, CR {:.1f}). Crankshaft + flywheel + rotor {:.4f} kg m^2; "
                                           "rod {:.3f} kg (CoM {:.1f} mm from the crank pin); piston group {:.3f} kg. Valve lift measured on the drawn lobes: "
                                           "intake {:.2f} mm, exhaust {:.2f} mm. Magneto points open at {:+.0f} deg, magnet at {:.0f} deg.",
                                           g.DisplacedVolume() * 1e6, g.bore * 1e3, g.stroke * 1e3, g.compressionRatio, sd.crankshaftInertia, sd.rodMass,
                                           sd.rodCentreOfMass * 1e3, sd.pistonMass, inLift * 1e3, exLift * 1e3, sd.ignition.pointsOpenDeg, sd.magnetMountDeg);
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
    bus.Register({"aiad_sim_start", "Start engine (live)", "Starts (or restarts) the live simulation shown in the viewport (slow motion).", NoArgs(), {},
                  [ctx, ready, notReady, noStudy](const Json&) {
                      if (!ready())
                          return notReady();
                      if (!ctx->session->studyError.empty())
                          return noStudy();
                      ctx->session->Restart();
                      ctx->session->running = true;
                      CommandOutput o;
                      o.text = "Engine started (live view). Use aiad_sim_status to read it, aiad_sim_run for a measured run.";
                      return o;
                  }});
    bus.Register({"aiad_sim_stop", "Stop engine (live)", "Stops the live simulation.", NoArgs(), {false, false, true}, [ctx](const Json&) {
                      ctx->session->running = false;
                      CommandOutput o;
                      o.text = "Engine stopped.";
                      return o;
                  }});
    bus.Register({"aiad_sim_status", "Live simulation status", "State of the live simulation: time, speed, crank angle, cylinder pressure, valve lifts.",
                  NoArgs(), {true, false, false}, [ctx, ready, notReady](const Json&) {
                      if (!ready())
                          return notReady();
                      const auto& s = ctx->session->last;
                      CommandOutput o;
                      o.result = {{"running", ctx->session->running}, {"time_s", s.time}, {"rpm", s.rpm}, {"crank_deg", std::fmod(s.crankDeg, 720.0)},
                                  {"pressure_bar", s.pressure / 1e5}, {"combustions", ctx->session->combustions},
                                  {"magneto_last_kv", ctx->session->lastSecondaryKv}};
                      o.text = std::format("{}; t = {:.3f} s, {:.0f} rpm, crank {:.1f} deg, cylinder {:.1f} bar, {} combustions.",
                                           ctx->session->running ? "Running" : "Stopped", s.time, s.rpm, std::fmod(s.crankDeg, 720.0), s.pressure / 1e5,
                                           ctx->session->combustions);
                      return o;
                  }});
    bus.Register({"aiad_sim_run", "Run simulation (measured)",
                  "Runs the coupled simulation (Chrono mechanism + 0D Otto cycle + magneto) of the current drawing in the background for the "
                  "given simulated seconds, starter included. Returns a job id; poll aiad_sim_job_status.",
                  Json{{"type", "object"},
                       {"properties", {{"seconds", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 10}, {"description", "simulated time [s]"}}}}},
                       {"additionalProperties", false}},
                  {true, false, false}, [ctx, &jobs, ready, notReady, noStudy](const Json& args) {
                      if (!ready())
                          return notReady();
                      if (!ctx->session->studyError.empty())
                          return noStudy();
                      const double seconds = args.value("seconds", 3.0);
                      const int id = jobs.Start(ctx->session->study.simulation, ctx->session->magnetoTable, seconds);
                      CommandOutput o;
                      o.result = {{"job", id}};
                      o.text = std::format("Simulation job {} started ({:.1f} s simulated, about {:.0f} s of computing). Poll aiad_sim_job_status with job = {}.", id,
                                           seconds, seconds * 2.0, id);
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
