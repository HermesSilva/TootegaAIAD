// App commands on the Command Bus (beside the document's own commands in aiad_doc): the machine study
// derived from the drawing (and its critique), what the view shows (visibility, selection, camera, section,
// snapshot) and the simulation (live and measured jobs).
#pragma once

#include "CommandBus.h"
#include "Session.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aiad::app
{

struct AppContext
{
    Session* session = nullptr;
    OrbitCamera* camera = nullptr;
    int* selected = nullptr;
    // PNG of the viewport as the user sees it now (main thread), empty when there is no viewport.
    std::function<std::vector<uint8_t>()> snapshot;
};

// Simulation runs in the background (async command pattern: job id, progress, result).
struct SimulationJob
{
    int id = 0;
    std::string state = "running";  // running | done | failed
    double seconds = 0.0, progress = 0.0;
    Json result;
    std::string text, error;
};

class SimulationJobs
{
public:
    ~SimulationJobs();
    int Start(const MechanismStudy& study, const std::vector<std::vector<sim::magneto::MagnetoSample>>& tables, double seconds);
    std::optional<SimulationJob> Get(int id) const;

private:
    mutable std::mutex m_mutex;
    std::map<int, std::shared_ptr<SimulationJob>> m_jobs;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stop = false;
    int m_next = 1;
};

void RegisterAppCommands(cmd::CommandBus& bus, AppContext& context, SimulationJobs& jobs);

// Runs the machine for `seconds` and measures it (the job's result): joint positions and speeds (mean over the
// last 0.5 s), chambers (displacement, compression ratio, peak pressure, combustions, indicated power),
// magnetos, timed motors. `progress` (0..1) and `stop` may be null.
Json RunMachine(const MechanismStudy& study, const std::vector<std::vector<sim::magneto::MagnetoSample>>& tables, double seconds, std::string& text,
                std::atomic<bool>* stop = nullptr, std::function<void(double)> progress = {});

}  // namespace aiad::app
