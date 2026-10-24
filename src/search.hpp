#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace search {

struct Hit { uint32_t cp; };

class Engine {
public:
    Engine();
    ~Engine();

    void start(const std::string& query);
    bool ready();
    std::vector<Hit> take();
    void cancel();

private:
    void worker_loop();

    std::thread             th_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       stop_{false};
    std::atomic<uint64_t>   gen_{0};

    // Worker state
    std::string current_query_;
    uint64_t    active_gen_ = 0;

    // Results
    std::vector<Hit> results_;
    bool             has_results_ = false;
};

} // namespace search