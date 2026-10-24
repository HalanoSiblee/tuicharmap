#pragma once
#include <string>

namespace clip {
    // Fire-and-forget. Safe to call from any thread. Rapid calls
    // coalesce — only the most recent string is guaranteed to land.
    void copy(const std::string& utf8);

    // Block until the worker has finished its last copy. Call on shutdown.
    void flush();
}