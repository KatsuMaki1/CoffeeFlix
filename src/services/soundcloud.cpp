#include "services/soundcloud.hpp"

#include <jansson.h>

#include <cctype>
#include <mutex>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace soundcloud {

namespace {

constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

constexpr const char* KEY_USER_ID = "sc_user_id";
constexpr const char* KEY_USERNAME = "sc_username";

std::mutex g_cid_mtx;
std::string g_client_id = "Hpp4j7peQTPimFlKMjTX3y3mK2946b7y";

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
    if (!hr.ok() || hr.body.empty()) {
        std::lock_guard<std::mutex> lk(g_cid_mtx);
        return g_client_id;
    }

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
        std::string tail;
        http::Request req;
        req.url = *it;
        req.timeout = 12;
        req.headers.push_back({"User-Agent", UA});
        req.on_data = [&tail, &found](const http::Response&, const char* data, size_t size) -> bool {
            tail.append(data, size);
            size_t p = tail.find("client_id:\"");
            if (p != std::string::npos) {
                size_t s = p + 11;
                size_t e = tail.find('"', s);
                if (e != std::string::npos && (e - s) >= 16 && (e - s) <= 48) {
                    found = tail.substr(s, e - s);
                    return false;  // stop transfer immediately
                }
            }
            if (tail.size() > 128) tail.erase(0, tail.size() - 128);
            return true;
        };
        http::perform(req);
        if (!found.empty()) break;
    }

    if (!found.empty()) {
        std::lock_guard<std::mutex> lk(g_cid_mtx);
        g_client_id = found;
        log_message(LOG_OK, "SoundCloud", "Discovered client_id");
    }
    std::lock_guard<std::mutex> lk(g_cid_mtx);
    return g_client_id;
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

bool signed_in() { return !store::get_str(KEY_USER_ID, "").empty(); }

std::string username() { return store::get_str(KEY_USERNAME, ""); }

bool sign_in_username(const std::string& user_or_url, std::string& err) {
    std::string u = animesama::txt::trim(user_or_url);
    if (u.empty()) {
        err = "Pseudo SoundCloud vide";
        return false;
    }
    if (!animesama::txt::starts_with(u, "http")) {
        while (!u.empty() && u.front() == '@') u.erase(u.begin());
        u = "https://soundcloud.com/" + u;
    }

    http::Response r = sc_get("https://api-v2.soundcloud.com/resolve?url=" + animesama::txt::url_encode(u));
    if (!r.ok()) {
        // Fallback: search by display name if permalink/URL returned 404
        std::string q = animesama::txt::trim(user_or_url);
        http::Response sr = sc_get("https://api-v2.soundcloud.com/search/users?q=" + animesama::txt::url_encode(q) + "&limit=1");
        if (sr.ok()) {
            json_error_t jerr;
            json_t* sroot = json_loads(sr.body.c_str(), 0, &jerr);
            json_t* col = sroot ? json_object_get(sroot, "collection") : nullptr;
            if (json_is_array(col) && json_array_size(col) > 0) {
                json_t* u0 = json_array_get(col, 0);
                json_t* jid = json_object_get(u0, "id");
                std::string uid;
                if (json_is_integer(jid)) uid = std::to_string((long long)json_integer_value(jid));
                else if (json_is_string(jid)) uid = json_string_value(jid);
                std::string uname = jstr(u0, "username");
                json_decref(sroot);
                if (!uid.empty()) {
                    store::set_str(KEY_USER_ID, uid);
                    store::set_str(KEY_USERNAME, uname.empty() ? q : uname);
                    log_message(LOG_OK, "SoundCloud", "Connected via user search: %s (id=%s)", uname.c_str(), uid.c_str());
                    return true;
                }
            } else if (sroot) {
                json_decref(sroot);
            }
        }
        err = r.error.empty() ? ("Profil introuvable (HTTP " + std::to_string(r.status) + ")") : r.error;
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON SoundCloud invalide";
        return false;
    }

    json_t* jid = json_object_get(root, "id");
    std::string uid;
    if (json_is_integer(jid)) uid = std::to_string((long long)json_integer_value(jid));
    else if (json_is_string(jid)) uid = json_string_value(jid);

    std::string uname = jstr(root, "username");
    if (uname.empty()) uname = jstr(root, "permalink");
    json_decref(root);

    if (uid.empty()) {
        err = "Utilisateur SoundCloud introuvable";
        return false;
    }

    store::set_str(KEY_USER_ID, uid);
    store::set_str(KEY_USERNAME, uname.empty() ? u : uname);
    log_message(LOG_OK, "SoundCloud", "Connected profile %s (id=%s)", uname.c_str(), uid.c_str());
    return true;
}

void sign_out() {
    store::set_str(KEY_USER_ID, "");
    store::set_str(KEY_USERNAME, "");
}

TrackList user_likes() {
    TrackList res;
    std::string uid = store::get_str(KEY_USER_ID, "");
    if (uid.empty()) {
        res.error = "Connecte d'abord ton pseudo SoundCloud en haut a droite";
        return res;
    }

    std::string url = "https://api-v2.soundcloud.com/users/" + uid + "/track_likes?limit=50";
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
    if (res.items.empty()) res.error = "Aucun titre like public trouve sur ce profil";
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

