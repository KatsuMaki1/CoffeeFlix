#include "services/aniskip.hpp"

#include <jansson.h>

#include <map>
#include <mutex>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace aniskip {

namespace {

std::mutex g_mtx;
std::map<std::string, int> g_mal_id_cache;

int lookup_mal_id(const std::string& title) {
    std::string clean = animesama::txt::trim(title);
    if (clean.empty()) return 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_mal_id_cache.find(clean);
        if (it != g_mal_id_cache.end()) return it->second;
    }

    http::Response r = http::get(
        "https://api.jikan.moe/v4/anime?q=" + animesama::txt::url_encode(clean) + "&limit=1", {}, 8);
    if (!r.ok()) return 0;

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return 0;

    int mal_id = 0;
    json_t* data = json_object_get(root, "data");
    if (json_is_array(data) && json_array_size(data) > 0) {
        json_t* first = json_array_get(data, 0);
        json_t* jid = first ? json_object_get(first, "mal_id") : nullptr;
        if (json_is_integer(jid)) mal_id = (int)json_integer_value(jid);
    }
    json_decref(root);

    if (mal_id > 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_mal_id_cache[clean] = mal_id;
    }
    return mal_id;
}

}  // namespace

std::vector<player::SkipSegment> fetch_segments(const std::string& anime_title, int ep_num) {
    std::vector<player::SkipSegment> out;
    if (anime_title.empty() || ep_num <= 0) return out;

    int mal_id = lookup_mal_id(anime_title);
    if (mal_id <= 0) return out;

    std::string url = "https://api.aniskip.com/v2/skip-times/" + std::to_string(mal_id) + "/" +
                      std::to_string(ep_num) + "?types[]=op&types[]=ed&episodeLength=0";
    http::Response r = http::get(url, {}, 8);
    if (!r.ok()) return out;

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return out;

    if (json_is_true(json_object_get(root, "found"))) {
        json_t* results = json_object_get(root, "results");
        if (json_is_array(results)) {
            size_t i;
            json_t* el;
            json_array_foreach(results, i, el) {
                json_t* iv = json_object_get(el, "interval");
                if (!iv) continue;
                double st = json_number_value(json_object_get(iv, "startTime"));
                double en = json_number_value(json_object_get(iv, "endTime"));
                json_t* jtype = json_object_get(el, "skipType");
                std::string stype = json_is_string(jtype) ? json_string_value(jtype) : "op";
                if (en > st + 2.0) {
                    player::SkipSegment seg;
                    seg.start = st;
                    seg.end = en;
                    seg.label = (stype == "ed") ? "Ending passe (AniSkip)" : "Opening passe (AniSkip)";
                    out.push_back(std::move(seg));
                }
            }
        }
    }
    json_decref(root);
    if (!out.empty()) {
        log_message(LOG_OK, "AniSkip", "Found %zu skip segment(s) for MAL #%d Ep %d", out.size(), mal_id, ep_num);
    }
    return out;
}

}  // namespace aniskip

