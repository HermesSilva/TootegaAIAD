// Work posted by other threads (bridge requests, agent events, finished jobs) and run on the main
// thread, where all UsdStage mutation happens.
#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace aiad
{

class MainThreadQueue
{
public:
    void Post(std::function<void()> work)
    {
        std::lock_guard lock(m_mutex);
        m_work.push_back(std::move(work));
    }

    // Main thread, once per frame.
    void Drain()
    {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard lock(m_mutex);
            work.swap(m_work);
        }
        for (auto& w : work)
            w();
    }

private:
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_work;
};

}  // namespace aiad
