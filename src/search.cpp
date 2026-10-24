#include "search.hpp"
#include "unicode.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace search {

namespace {

std::string upper_copy(const std::string& s) {
    std::string o(s);
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}

int score(std::string_view text, const std::string& q) {
    if (text == q) return 100000; // Exact match
    
    int s = 0;
    size_t sub = text.find(q);
    if (sub == 0) {
        s = 10000; // Starts with
    } else if (sub != std::string_view::npos) {
        s = 5000;  // Contains
    }
    
    size_t ti = 0;
    int run = 0;
    int last_match = -1;
    
    for (char qc : q) {
        bool hit = false;
        while (ti < text.size()) {
            if (text[ti] == qc) {
                int gap = (last_match >= 0) ? (int)(ti - last_match - 1) : 0;
                s -= gap * 5;
                if (run == 0) s += 1;
                else          s += 20;
                if (ti == 0 || text[ti - 1] == ' ' || text[ti - 1] == '-')
                    s += 100;
                ++run;
                last_match = (int)ti;
                ++ti;
                hit = true;
                break;
            }
            ++ti;
        }
        if (!hit) return -1;
    }
    s -= (int)text.size();
    return s;
}

} // namespace

Engine::Engine() {
    th_ = std::thread([this] { worker_loop(); });
}

Engine::~Engine() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_one();
    if (th_.joinable()) th_.join();
}

void Engine::start(const std::string& query) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        current_query_ = query;
        ++gen_;
        has_results_ = false; // Invalidate old results immediately
    }
    cv_.notify_one();
}

void Engine::cancel() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        current_query_.clear();
        ++gen_;
        has_results_ = false;
    }
    cv_.notify_one();
}

bool Engine::ready() {
    std::lock_guard<std::mutex> lk(mtx_);
    return has_results_;
}

std::vector<Hit> Engine::take() {
    std::lock_guard<std::mutex> lk(mtx_);
    has_results_ = false;
    return std::move(results_);
}

void Engine::worker_loop() {
    while (true) {
        std::string q;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return stop_ || gen_ != active_gen_; });
            if (stop_) return;
            active_gen_ = gen_;
            q = current_query_;
        }

        if (q.empty()) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (gen_ == active_gen_) {
                results_.clear();
                has_results_ = true;
            }
            continue;
        }

        std::string query_upper = upper_copy(q);
        size_t n = uc::count();
        if (n == 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (gen_ == active_gen_) {
                results_.clear();
                has_results_ = true;
            }
            continue;
        }

        std::vector<std::pair<int, uint32_t>> scored;
        scored.reserve(256);

        for (size_t i = 0; i < n; ++i) {
            if (gen_ != active_gen_) break; // Abort early
            int sc = score(uc::upper_name(i), query_upper);
            if (sc >= 0) scored.emplace_back(sc, uc::cp_at(i));
        }

        if (gen_ != active_gen_) continue;

        constexpr size_t kMax = 500;
        auto cmp = [](const std::pair<int, uint32_t>& a,
                      const std::pair<int, uint32_t>& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        };

        if (scored.size() > kMax) {
            std::partial_sort(scored.begin(), scored.begin() + kMax, scored.end(), cmp);
            scored.resize(kMax);
        } else {
            std::sort(scored.begin(), scored.end(), cmp);
        }

        std::vector<Hit> local_hits;
        local_hits.reserve(scored.size());
        for (auto& p : scored) local_hits.push_back({p.second});

        std::lock_guard<std::mutex> lk(mtx_);
        if (gen_ == active_gen_) {
            results_     = std::move(local_hits);
            has_results_ = true;
        }
    }
}

} // namespace search