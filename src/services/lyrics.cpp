#include "services/lyrics.hpp"

#include <jansson.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace lyrics {

namespace {

std::mutex g_mtx;
TrackLyrics g_cur;

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string clean_track_title(std::string s) {
    // Strip common YouTube/SoundCloud suffixes like "(Official Video)", "[Lyrics]", etc.
    for (const char* open : {"(", "["}) {
        char close = (open[0] == '(') ? ')' : ']';
        size_t p = 0;
        while ((p = s.find(open, p)) != std::string::npos) {
            size_t e = s.find(close, p + 1);
            if (e == std::string::npos) break;
            std::string inner = animesama::txt::lower(s.substr(p + 1, e - p - 1));
            if (inner.find("official") != std::string::npos || inner.find("lyric") != std::string::npos ||
                inner.find("audio") != std::string::npos || inner.find("video") != std::string::npos ||
                inner.find("clip") != std::string::npos || inner.find("hd") != std::string::npos) {
                s.erase(p, e - p + 1);
            } else {
                p = e + 1;
            }
        }
    }
    return animesama::txt::collapse_ws(s);
}

std::vector<Line> parse_lrc(const std::string& lrc) {
    std::vector<Line> out;
    std::istringstream iss(lrc);
    std::string raw;
    while (std::getline(iss, raw)) {
        raw = animesama::txt::trim(raw);
        if (raw.empty() || raw[0] != '[') continue;
        size_t close = raw.find(']');
        if (close == std::string::npos) continue;
        std::string ts = raw.substr(1, close - 1);
        size_t colon = ts.find(':');
        if (colon == std::string::npos || !std::isdigit((unsigned char)ts[0])) continue;
        int mins = std::atoi(ts.substr(0, colon).c_str());
        double secs = std::atof(ts.substr(colon + 1).c_str());
        double t = mins * 60.0 + secs;
        std::string text = animesama::txt::trim(raw.substr(close + 1));
        if (text.empty()) continue;
        out.push_back({t, std::move(text)});
    }
    std::sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.time < b.time; });
    return out;
}

std::vector<Line> parse_plain(const std::string& plain) {
    std::vector<Line> out;
    std::istringstream iss(plain);
    std::string raw;
    double t = 0.0;
    while (std::getline(iss, raw)) {
        raw = animesama::txt::trim(raw);
        if (raw.empty()) continue;
        out.push_back({t, std::move(raw)});
        t += 4.0;
    }
    return out;
}

void fetch_async(std::string key, std::string title, std::string artist, double duration) {
    std::thread([key = std::move(key), title = std::move(title), artist = std::move(artist), duration]() {
        std::string ct = clean_track_title(title);
        std::string ca = animesama::txt::trim(artist);

        // If title contains "Artist - Track", split it when artist looks generic
        size_t dash = ct.find(" - ");
        if (dash != std::string::npos && (ca.empty() || animesama::txt::contains_ci(ca, "topic") ||
                                          animesama::txt::contains_ci(ca, "vevo"))) {
            ca = animesama::txt::trim(ct.substr(0, dash));
            ct = animesama::txt::trim(ct.substr(dash + 3));
        }

        std::string synced_lrc, plain_lrc;
        auto try_extract = [&](json_t* obj) {
            if (!obj || !json_is_object(obj)) return false;
            std::string sl = jstr(obj, "syncedLyrics");
            std::string pl = jstr(obj, "plainLyrics");
            if (!sl.empty()) {
                synced_lrc = std::move(sl);
                return true;
            }
            if (!pl.empty() && plain_lrc.empty()) plain_lrc = std::move(pl);
            return false;
        };

        // 1. Exact /api/get
        std::string url = "https://lrclib.net/api/get?track_name=" + animesama::txt::url_encode(ct) +
                          "&artist_name=" + animesama::txt::url_encode(ca);
        if (duration > 5.0) url += "&duration=" + std::to_string((int)duration);
        http::Response r = http::get(url, {{"User-Agent", "CoffeeFlix/1.0"}}, 10);
        if (r.ok()) {
            json_error_t jerr;
            json_t* root = json_loads(r.body.c_str(), 0, &jerr);
            if (root) {
                try_extract(root);
                json_decref(root);
            }
        }

        // 2. Fallback /api/search
        if (synced_lrc.empty()) {
            std::string q = ca.empty() ? ct : (ca + " " + ct);
            http::Response sr =
                http::get("https://lrclib.net/api/search?q=" + animesama::txt::url_encode(q),
                          {{"User-Agent", "CoffeeFlix/1.0"}}, 10);
            if (sr.ok()) {
                json_error_t jerr;
                json_t* arr = json_loads(sr.body.c_str(), 0, &jerr);
                if (json_is_array(arr)) {
                    size_t i;
                    json_t* el;
                    json_array_foreach(arr, i, el) {
                        if (try_extract(el)) break;
                    }
                }
                if (arr) json_decref(arr);
            }
        }

        TrackLyrics res;
        res.key = key;
        res.loading = false;
        if (!synced_lrc.empty()) {
            res.lines = parse_lrc(synced_lrc);
            res.synced = !res.lines.empty();
            res.found = !res.lines.empty();
        } else if (!plain_lrc.empty()) {
            res.lines = parse_plain(plain_lrc);
            res.synced = false;
            res.found = !res.lines.empty();
        }

        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_cur.key == key) {
            g_cur = std::move(res);
            log_message(LOG_OK, "Lyrics", "Loaded %zu lines (synced=%d) for %s", g_cur.lines.size(),
                        (int)g_cur.synced, ct.c_str());
        }
    }).detach();
}

}  // namespace

TrackLyrics get(const std::string& id, const std::string& title, const std::string& artist, double duration) {
    std::string key = id + "|" + title;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_cur.key != key) {
        g_cur = TrackLyrics{};
        g_cur.key = key;
        g_cur.loading = true;
        if (!title.empty()) fetch_async(key, title, artist, duration);
        else g_cur.loading = false;
    }
    return g_cur;
}

}  // namespace lyrics

