#include "services/mal.hpp"

#include <jansson.h>

#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace mal {

namespace {

constexpr const char* API_BASE = "https://api.myanimelist.net/v2";
constexpr const char* TOKEN_URL = "https://myanimelist.net/v1/oauth2/token";

constexpr const char* KEY_CLIENT_ID = "mal_client_id";
constexpr const char* KEY_ACCESS = "mal_access_token";
constexpr const char* KEY_REFRESH = "mal_refresh_token";
constexpr const char* KEY_USER = "mal_user";
constexpr const char* KEY_ENABLED = "mal_enabled";

std::mutex g_mtx;
std::string g_synced_id;
std::string g_inflight_id;
std::map<std::string, int> g_anime_id_cache;

http::Response mal_request(const std::string& method, const std::string& url, const std::string& token,
                           const std::string& body = "") {
    http::Request req;
    req.method = method;
    req.url = url;
    req.timeout = 12;
    req.headers.push_back({"Authorization", "Bearer " + token});
    if (!body.empty()) {
        req.body = body;
        req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    }
    return http::perform(req);
}

bool refresh_access_token(std::string& token_out) {
    std::string cid = store::get_str(KEY_CLIENT_ID, "");
    std::string rtok = store::get_str(KEY_REFRESH, "");
    if (cid.empty() || rtok.empty()) return false;

    http::Request req;
    req.method = "POST";
    req.url = TOKEN_URL;
    req.timeout = 12;
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body = "client_id=" + animesama::txt::url_encode(cid) +
               "&grant_type=refresh_token&refresh_token=" + animesama::txt::url_encode(rtok);

    http::Response r = http::perform(req);
    if (!r.ok()) {
        log_message(LOG_WARNING, "MAL", "Token refresh failed: HTTP %ld", r.status);
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* at = json_object_get(root, "access_token");
    json_t* rt = json_object_get(root, "refresh_token");
    if (!json_is_string(at)) {
        json_decref(root);
        return false;
    }
    token_out = json_string_value(at);
    store::set_str(KEY_ACCESS, token_out);
    if (json_is_string(rt)) store::set_str(KEY_REFRESH, json_string_value(rt));
    json_decref(root);
    log_message(LOG_OK, "MAL", "Access token refreshed");
    return true;
}

http::Response mal_request_auth(const std::string& method, const std::string& url, const std::string& body = "") {
    std::string tok = store::get_str(KEY_ACCESS, "");
    if (tok.empty()) return http::Response{};
    http::Response r = mal_request(method, url, tok, body);
    if (r.status == 401 && refresh_access_token(tok)) {
        r = mal_request(method, url, tok, body);
    }
    return r;
}

// Extract base anime title from "Title - Ep 12"
std::string parse_base_title(const std::string& title) {
    size_t pos = title.rfind(" - Ep ");
    if (pos != std::string::npos) return animesama::txt::trim(title.substr(0, pos));
    return animesama::txt::trim(title);
}

// Extract episode number from id ("...#12") or title ("... - Ep 12")
int parse_episode_number(const std::string& id, const std::string& title) {
    size_t hash = id.rfind('#');
    if (hash != std::string::npos) {
        int n = std::atoi(id.substr(hash + 1).c_str());
        if (n > 0) return n;
    }
    size_t pos = title.rfind(" - Ep ");
    if (pos != std::string::npos) {
        int n = std::atoi(title.substr(pos + 6).c_str());
        if (n > 0) return n;
    }
    return 0;
}

// Extract season number if subtitle looks like "Saison 2" or "Season 3"
int parse_season_number(const std::string& subtitle) {
    std::string low = animesama::txt::lower(subtitle);
    for (size_t i = 0; i < low.size(); i++) {
        if (std::isdigit((unsigned char)low[i])) {
            int n = std::atoi(low.c_str() + i);
            if (n >= 1 && n <= 30) return n;
        }
    }
    return 1;
}

bool search_anime(const std::string& query, int& mal_id, int& total_eps, int& watched_eps) {
    std::string url = std::string(API_BASE) + "/anime?q=" + animesama::txt::url_encode(query) +
                      "&limit=5&fields=id,title,num_episodes,my_list_status";
    http::Response r = mal_request_auth("GET", url);
    if (!r.ok()) return false;

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* data = json_object_get(root, "data");
    if (!json_is_array(data) || json_array_size(data) == 0) {
        json_decref(root);
        return false;
    }

    json_t* first = json_array_get(data, 0);
    json_t* node = first ? json_object_get(first, "node") : nullptr;
    json_t* jid = node ? json_object_get(node, "id") : nullptr;
    if (!json_is_integer(jid)) {
        json_decref(root);
        return false;
    }

    mal_id = (int)json_integer_value(jid);
    json_t* jnum = json_object_get(node, "num_episodes");
    total_eps = json_is_integer(jnum) ? (int)json_integer_value(jnum) : 0;

    watched_eps = 0;
    json_t* myst = json_object_get(node, "my_list_status");
    if (json_is_object(myst)) {
        json_t* jw = json_object_get(myst, "num_watched_episodes");
        if (json_is_integer(jw)) watched_eps = (int)json_integer_value(jw);
    }

    json_decref(root);
    return true;
}

bool fetch_anime_details(int mal_id, int& total_eps, int& watched_eps) {
    std::string url = std::string(API_BASE) + "/anime/" + std::to_string(mal_id) +
                      "?fields=id,num_episodes,my_list_status";
    http::Response r = mal_request_auth("GET", url);
    if (!r.ok()) return false;

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return false;

    json_t* jnum = json_object_get(root, "num_episodes");
    total_eps = json_is_integer(jnum) ? (int)json_integer_value(jnum) : 0;

    watched_eps = 0;
    json_t* myst = json_object_get(root, "my_list_status");
    if (json_is_object(myst)) {
        json_t* jw = json_object_get(myst, "num_watched_episodes");
        if (json_is_integer(jw)) watched_eps = (int)json_integer_value(jw);
    }

    json_decref(root);
    return true;
}

void sync_episode_async(std::string id, std::string base_title, std::string subtitle, int ep_num) {
    std::thread([id = std::move(id), base_title = std::move(base_title), subtitle = std::move(subtitle), ep_num]() {
        int mal_id = 0;
        int total_eps = 0;
        int watched_eps = 0;
        int season_num = parse_season_number(subtitle);
        std::string cache_key = base_title + "|" + std::to_string(season_num);

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_anime_id_cache.find(cache_key);
            if (it != g_anime_id_cache.end()) mal_id = it->second;
        }

        bool found = false;
        if (mal_id > 0) {
            found = fetch_anime_details(mal_id, total_eps, watched_eps);
        }
        if (!found) {
            if (season_num >= 2) {
                found = search_anime(base_title + " Season " + std::to_string(season_num), mal_id, total_eps, watched_eps);
            }
            if (!found) {
                found = search_anime(base_title, mal_id, total_eps, watched_eps);
            }
            if (found && mal_id > 0) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_anime_id_cache[cache_key] = mal_id;
            }
        }

        if (!found || mal_id <= 0) {
            log_message(LOG_WARNING, "MAL", "Anime not found on MAL: %s (%s)", base_title.c_str(), subtitle.c_str());
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_inflight_id == id) g_inflight_id.clear();
            return;
        }

        if (ep_num > watched_eps) {
            std::string status = (total_eps > 0 && ep_num >= total_eps) ? "completed" : "watching";
            std::string body = "status=" + status + "&num_watched_episodes=" + std::to_string(ep_num);
            std::string url = std::string(API_BASE) + "/anime/" + std::to_string(mal_id) + "/my_list_status";
            http::Response r = mal_request_auth("PATCH", url, body);
            if (r.ok()) {
                log_message(LOG_OK, "MAL", "Synced %s Ep %d (MAL #%d, %s)", base_title.c_str(), ep_num, mal_id,
                            status.c_str());
            } else {
                log_message(LOG_WARNING, "MAL", "Update failed for MAL #%d: HTTP %ld (%s)", mal_id, r.status,
                            r.error.c_str());
            }
        } else {
            log_message(LOG_DEBUG, "MAL", "%s Ep %d already synced (watched=%d)", base_title.c_str(), ep_num, watched_eps);
        }

        std::lock_guard<std::mutex> lk(g_mtx);
        g_synced_id = id;
        if (g_inflight_id == id) g_inflight_id.clear();
    }).detach();
}

}  // namespace

bool enabled() { return store::get_bool(KEY_ENABLED, true) && signed_in(); }

void set_enabled(bool on) { store::set_bool(KEY_ENABLED, on); }

bool signed_in() { return !store::get_str(KEY_ACCESS, "").empty(); }

std::string username() { return store::get_str(KEY_USER, ""); }

std::string client_id() { return store::get_str(KEY_CLIENT_ID, ""); }

void set_client_id(const std::string& id) { store::set_str(KEY_CLIENT_ID, animesama::txt::trim(id)); }

bool authenticate(const std::string& access_token, const std::string& refresh_token, std::string& err) {
    std::string tok = animesama::txt::trim(access_token);
    std::string rtok = animesama::txt::trim(refresh_token);
    if (tok.empty()) {
        err = "Access Token MyAnimeList vide";
        return false;
    }

    http::Response r = mal_request("GET", std::string(API_BASE) + "/users/@me", tok);
    if (!r.ok()) {
        err = r.error.empty() ? ("MyAnimeList HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON MyAnimeList invalide";
        return false;
    }

    json_t* jname = json_object_get(root, "name");
    std::string user = json_is_string(jname) ? json_string_value(jname) : "MyAnimeList";
    json_decref(root);

    store::set_str(KEY_ACCESS, tok);
    if (!rtok.empty()) store::set_str(KEY_REFRESH, rtok);
    store::set_str(KEY_USER, user);
    store::set_bool(KEY_ENABLED, true);
    log_message(LOG_OK, "MAL", "Signed in as %s", user.c_str());
    return true;
}

void sign_out() {
    store::set_str(KEY_ACCESS, "");
    store::set_str(KEY_REFRESH, "");
    store::set_str(KEY_USER, "");
    std::lock_guard<std::mutex> lk(g_mtx);
    g_synced_id.clear();
    g_inflight_id.clear();
}

void on_progress(const std::string& service, const std::string& id, const std::string& title,
                 const std::string& subtitle, double pos, double duration) {
    if (service != "animesama" || id.empty() || title.empty() || !enabled()) return;

    // Sync once the user has watched >= 80% of the episode (or >= 10 min if duration unknown).
    bool reached = (duration > 60.0) ? (pos >= duration * 0.80) : (pos >= 600.0);
    if (!reached) return;

    int ep_num = parse_episode_number(id, title);
    std::string base_title = parse_base_title(title);
    if (ep_num <= 0 || base_title.empty()) return;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_synced_id == id || g_inflight_id == id) return;
        g_inflight_id = id;
    }

    sync_episode_async(id, std::move(base_title), subtitle, ep_num);
}

}  // namespace mal

