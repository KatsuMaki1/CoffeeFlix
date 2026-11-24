#include "services/soundcloud.hpp"

#include <jansson.h>

#include <cctype>
#include <mutex>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace soundcloud {

namespace {

constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

std::mutex g_cid_mtx;
std::string g_client_id;

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string fetch_client_id(bool force_refresh = false) {
    {
        std::lock_guard<std::mutex> lk(g_cid_mtx);
        if (!force_refresh && !g_client_id.empty()) return g_client_id;
    }

    http::Response hr = http::get("https://soundcloud.com/", {{"User-Agent", UA}}, 12);
    if (!hr.ok() || hr.body.empty()) return "";

    std::vector<std::string> scripts;
    size_t pos = 0;
    while ((pos = hr.body.find("https://a-v2.sndcdn.com/assets/", pos)) != std::string::npos) {
        size_t end = hr.body.find('"', pos);
        if (end == std::string::npos) break;
        std::string u = hr.body.substr(pos, end - pos);
        if (animesama::txt::ends_with(u, ".js")) scripts.push_back(u);
        pos = end + 1;
    }

    std::string found;
    for (auto it = scripts.rbegin(); it != scripts.rend(); ++it) {
        http::Response jr = http::get(*it, {{"User-Agent", UA}}, 12);
        if (!jr.ok() || jr.body.empty()) continue;
        size_t p = jr.body.find("client_id:\"");
        if (p == std::string::npos) continue;
        p += 11;
        size_t e = jr.body.find('"', p);
        if (e != std::string::npos && e > p && (e - p) >= 16 && (e - p) <= 48) {
            found = jr.body.substr(p, e - p);
            break;
        }
    }

    if (!found.empty()) {
        std::lock_guard<std::mutex> lk(g_cid_mtx);
        g_client_id = found;
        log_message(LOG_OK, "SoundCloud", "Discovered client_id");
    }
    return found;
}

http::Response sc_get(const std::string& url_without_cid) {
    std::string cid = fetch_client_id(false);
    if (cid.empty()) return http::Response{};

    char sep = (url_without_cid.find('?') != std::string::npos) ? '&' : '?';
    http::Response r = http::get(url_without_cid + sep + "client_id=" + cid, {{"User-Agent", UA}}, 15);
    if (r.status == 401 || r.status == 403) {
        cid = fetch_client_id(true);
        if (!cid.empty()) {
            r = http::get(url_without_cid + sep + "client_id=" + cid, {{"User-Agent", UA}}, 15);
        }
    }
    return r;
}

bool parse_track(json_t* obj, Track& out) {
    if (!obj || !json_is_object(obj)) return false;
    json_t* jid = json_object_get(obj, "id");
    if (!jid) return false;
    if (json_is_integer(jid)) out.id = std::to_string((long long)json_integer_value(jid));
    else if (json_is_string(jid)) out.id = json_string_value(jid);
    if (out.id.empty()) return false;

    out.title = jstr(obj, "title");
    if (out.title.empty()) return false;

    json_t* user = json_object_get(obj, "user");
    out.artist = jstr(user, "username");
    if (out.artist.empty()) out.artist = "SoundCloud";

    out.artwork = jstr(obj, "artwork_url");
    if (out.artwork.empty()) out.artwork = jstr(user, "avatar_url");
    if (!out.artwork.empty()) {
        animesama::txt::replace_all(out.artwork, "-large.", "-t500x500.");
    }

    json_t* jdur = json_object_get(obj, "duration");
    if (json_is_integer(jdur)) out.duration_sec = (int)(json_integer_value(jdur) / 1000);

    json_t* media = json_object_get(obj, "media");
    json_t* trans = media ? json_object_get(media, "transcodings") : nullptr;
    if (!json_is_array(trans)) return false;

    std::string hls_url;
    size_t i;
    json_t* tr;
    json_array_foreach(trans, i, tr) {
        std::string u = jstr(tr, "url");
        json_t* fmt = json_object_get(tr, "format");
        std::string proto = jstr(fmt, "protocol");
        if (u.empty()) continue;
        if (proto == "progressive") {
            out.transcoding_url = u;
            out.is_progressive = true;
            break;
        }
        if (proto == "hls" && hls_url.empty()) {
            hls_url = u;
        }
    }
    if (out.transcoding_url.empty()) {
        out.transcoding_url = hls_url;
        out.is_progressive = false;
    }
    return !out.transcoding_url.empty();
}

}  // namespace

TrackList charts(const std::string& kind) {
    TrackList res;
    std::string k = (kind == "top") ? "top" : "trending";
    std::string url = "https://api-v2.soundcloud.com/charts?kind=" + k +
                      "&genre=soundcloud%3Agenres%3Aall-music&limit=24";
    http::Response r = sc_get(url);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("SoundCloud HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON SoundCloud invalide";
        return res;
    }

    json_t* col = json_object_get(root, "collection");
    if (json_is_array(col)) {
        size_t i;
        json_t* el;
        json_array_foreach(col, i, el) {
            json_t* tr = json_object_get(el, "track");
            Track t;
            if (parse_track(tr ? tr : el, t)) res.items.push_back(std::move(t));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucune piste trouvee";
    log_message(LOG_OK, "SoundCloud", "charts(%s) -> %d track(s)", k.c_str(), (int)res.items.size());
    return res;
}

TrackList search(const std::string& query) {
    TrackList res;
    std::string q = animesama::txt::trim(query);
    if (q.empty()) return charts("trending");

    std::string url = "https://api-v2.soundcloud.com/search/tracks?q=" + animesama::txt::url_encode(q) + "&limit=24";
    http::Response r = sc_get(url);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("SoundCloud HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON SoundCloud invalide";
        return res;
    }

    json_t* col = json_object_get(root, "collection");
    if (json_is_array(col)) {
        size_t i;
        json_t* el;
        json_array_foreach(col, i, el) {
            Track t;
            if (parse_track(el, t)) res.items.push_back(std::move(t));
        }
    }
    json_decref(root);
    log_message(LOG_OK, "SoundCloud", "search('%s') -> %d track(s)", q.c_str(), (int)res.items.size());
    return res;
}

player::Source make_source(const Track& t) {
    player::Source src;
    src.title = t.title;
    src.subtitle = t.artist;
    src.artwork = t.artwork;
    src.service = "soundcloud";
    src.id = t.id;
    src.user_agent = UA;
    src.remember_position = false;
    src.chunked_http = t.is_progressive;
    std::string turl = t.transcoding_url;
    src.resolve = [turl](player::Source& s, std::string& err) -> bool {
        http::Response r = sc_get(turl);
        if (!r.ok()) {
            err = r.error.empty() ? ("SoundCloud stream HTTP " + std::to_string(r.status)) : r.error;
            return false;
        }
        json_error_t jerr;
        json_t* root = json_loads(r.body.c_str(), 0, &jerr);
        if (!root) {
            err = "Reponse stream SoundCloud invalide";
            return false;
        }
        std::string final_url = jstr(root, "url");
        json_decref(root);
        if (final_url.empty()) {
            err = "URL audio SoundCloud vide";
            return false;
        }
        s.url = final_url;
        return true;
    };
    return src;
}

}  // namespace soundcloud

