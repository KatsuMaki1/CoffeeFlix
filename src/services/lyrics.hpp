#pragma once

// Live synced lyrics & Karaoke client powered by LRCLIB (https://lrclib.net).

#include <string>
#include <vector>

namespace lyrics {

struct Line {
    double time = 0.0;  // seconds
    std::string text;
};

struct TrackLyrics {
    std::string key;
    bool loading = false;
    bool found = false;
    bool synced = false;
    std::vector<Line> lines;
};

// Non-blocking: triggers background fetch on first call for (id/title/artist), returns current state.
TrackLyrics get(const std::string& id, const std::string& title, const std::string& artist, double duration);

}  // namespace lyrics

