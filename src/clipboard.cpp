#include "clipboard.hpp"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace clip {

    static const char* kCmd =
#if defined(__APPLE__)
        "pbcopy";
#elif defined(_WIN32)
        "clip";
#elif defined(__ANDROID__)
        // If compiled directly on Termux, we know for sure it's Android
        "termux-clipboard-set 2>/dev/null";
#else
        // Linux fallback: check if $PREFIX specifically points to Termux's path
        // or if $TERMUX_VERSION is set.
        "if [ -n \"$TERMUX_VERSION\" ] || [ \"$PREFIX\" = \"/data/data/com.termux/files/usr\" ]; then "
            "termux-clipboard-set 2>/dev/null; "
        "else "
            "(wl-copy 2>/dev/null || xclip -selection clipboard 2>/dev/null || xsel --clipboard 2>/dev/null); "
        "fi";
#endif

    static bool run_copy(const std::string& s) {
        FILE* p = popen(kCmd, "w");
        if (!p) return false;
        fwrite(s.data(), 1, s.size(), p);
        return pclose(p) == 0;
    }

    class Worker {
    public:
        static Worker& instance() { static Worker w; return w; }

        void post(std::string s) {
            {
                std::lock_guard<std::mutex> lk(m_);
                pending_ = std::move(s);
                has_ = true;
            }
            cv_.notify_one();
        }

        void wait_idle() {
            std::unique_lock<std::mutex> lk(m_);
            done_.wait(lk, [&]{ return !busy_ && !has_; });
        }

    private:
        Worker() : t_([this]{ loop(); }) {}
        ~Worker() {
            { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
            cv_.notify_all();
            t_.join();
        }

        void loop() {
            for (;;) {
                std::string s;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&]{ return stop_ || has_; });
                    if (stop_) return;
                    s = std::move(pending_);
                    has_ = false;
                    busy_ = true;
                }
                run_copy(s);
                {
                    std::lock_guard<std::mutex> lk(m_);
                    busy_ = false;
                }
                done_.notify_all();
            }
        }

        std::thread             t_;
        std::mutex              m_;
        std::condition_variable cv_, done_;
        std::string             pending_;
        bool has_  = false;
        bool busy_ = false;
        bool stop_ = false;
    };

    void copy(const std::string& s) { Worker::instance().post(s); }
    void flush()                    { Worker::instance().wait_idle(); }
}