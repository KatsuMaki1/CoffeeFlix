#include "services/lastfm.hpp"

#include <jansson.h>

#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#include "core/http.hpp"
#include "core/md5.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace lastfm {

namespace {

constexpr const char* ENDPOINT = "https://ws.audioscrobbler.com/2.0/";
constexpr const char* KEY_API = "lastfm_api_key";
constexpr const char* KEY_SECRET = "lastfm_secret";
constexpr const char* KEY_SK = "lastfm_session_key";
constexpr const char* KEY_USER = "lastfm_user";
constexpr const char* KEY_ENABLED = "lastfm_enabled";

std::mutex g_mtx;
std::string g_cur_id;
bool g_now_playing_sent = false;
bool g_scrobbled = false;
int64_t g_started_at = 0;

std::string build_signed_body(const std::map<std::string, std::string>& params, const std::string& secret) {
    std::string sig_base;
    std::string form;
    for (const auto& kv : params) {
        sig_base += kv.first + kv.second;
        if (!form.empty()) form += '&';
        form += animesama::txt::url_encode(kv.first) + "=" + animesama::txt::url_encode(kv.second);
    }
    sig_base += secret;
    std::string sig = md5::hash(sig_base);
    form += "&api_sig=" + sig + "&format=json";
    return form;
}

http::Response post_lastfm(const std::map<std::string, std::string>& params, const std::string& secret) {
    http::Request req;
    req.method = "POST";
    req.url = ENDPOINT;
    req.body = build_signed_body(params, secret);
    req.timeout = 12;
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    return http::perform(req);
}

void send_async(std::map<std::string, std::string> params, std::string secret, const char* tag) {
    std::thread([params = std::move(params), secret = std::move(secret), tag]() {
        http::Response r = post_lastfm(params, secret);
        if (!r.ok()) {
            log_message(LOG_WARNING, "LastFM", "%s failed: HTTP %ld (%s)", tag, r.status, r.error.c_str());
        } else {
            log_message(LOG_OK, "LastFM", "%s ok", tag);
        }
    }).detach();
}

}  // namespace

bool enabled() { return store::get_bool(KEY_ENABLED, true) && signed_in(); }

void set_enabled(bool on) { store::set_bool(KEY_ENABLED, on); }

bool signed_in() { return !store::get_str(KEY_SK, "").empty(); }

std::string username() { return store::get_str(KEY_USER, ""); }

std::string api_key() { return store::get_str(KEY_API, ""); }

std::string api_secret() { return store::get_str(KEY_SECRET, ""); }

void set_api_credentials(const std::string& key, const std::string& secret) {
    store::set_str(KEY_API, animesama::txt::trim(key));
    store::set_str(KEY_SECRET, animesama::txt::trim(secret));
}

bool authenticate(const std::string& user, const std::string& password, std::string& err) {
    std::string key = api_key();
    std::string secret = api_secret();
    if (key.empty() || secret.empty()) {
        err = "Configure d'abord ton API Key et API Secret Last.fm";
        return false;
    }

    std::map<std::string, std::string> params = {
        {"api_key", key},
        {"method", "auth.getMobileSession"},
        {"password", password},
        {"username", user},
    };
    http::Response r = post_lastfm(params, secret);
    if (!r.ok() && r.body.empty()) {
        err = r.error.empty() ? ("Last.fm HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON Last.fm invalide";
        return false;
    }

    json_t* msg = json_object_get(root, "message");
    if (json_is_string(msg)) {
        err = json_string_value(msg);
        json_decref(root);
        return false;
    }

    json_t* sess = json_object_get(root, "session");
    json_t* sk = sess ? json_object_get(sess, "key") : nullptr;
    json_t* name = sess ? json_object_get(sess, "name") : nullptr;
    if (!json_is_string(sk)) {
        err = "Cle de session Last.fm absente";
        json_decref(root);
        return false;
    }

    store::set_str(KEY_SK, json_string_value(sk));
    store::set_str(KEY_USER, json_is_string(name) ? json_string_value(name) : user);
    store::set_bool(KEY_ENABLED, true);
    json_decref(root);
    log_message(LOG_OK, "LastFM", "Signed in as %s", username().c_str());
    return true;
}

void sign_out() {
    store::set_str(KEY_SK, "");
    store::set_str(KEY_USER, "");
}

void on_playback(const std::string& id, const std::string& title, const std::string& artist, double pos,
                 double duration, bool paused) {
    if (paused || id.empty() || title.empty() || artist.empty() || !enabled()) return;

    std::string key = api_key();
    std::string secret = api_secret();
    std::string sk = store::get_str(KEY_SK, "");
    if (key.empty() || secret.empty() || sk.empty()) return;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (id != g_cur_id) {
        g_cur_id = id;
        g_now_playing_sent = false;
        g_scrobbled = false;
        g_started_at = (int64_t)std::time(nullptr);
    }

    if (!g_now_playing_sent) {
        g_now_playing_sent = true;
        std::map<std::string, std::string> params = {
            {"api_key", key},
            {"artist", artist},
            {"method", "track.updateNowPlaying"},
            {"sk", sk},
            {"track", title},
        };
        if (duration > 1.0) params["duration"] = std::to_string((int)duration);
        send_async(std::move(params), secret, "updateNowPlaying");
    }

    double threshold = (duration > 30.0) ? std::min(240.0, duration * 0.5) : 30.0;
    if (!g_scrobbled && pos >= threshold) {
        g_scrobbled = true;
        std::map<std::string, std::string> params = {
            {"api_key", key},
            {"artist", artist},
            {"method", "track.scrobble"},
            {"sk", sk},
            {"timestamp", std::to_string(g_started_at)},
            {"track", title},
        };
        if (duration > 1.0) params["duration"] = std::to_string((int)duration);
        send_async(std::move(params), secret, "scrobble");
    }
}

}  // namespace lastfm

