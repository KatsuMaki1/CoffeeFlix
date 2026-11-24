#include "services/watchtogether.hpp"

#include <jansson.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/http.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace watchtogether {

namespace {

std::mutex g_mtx;
std::string g_room;
std::string g_sender;
std::string g_media_title;
std::string g_last_sent_id;
bool g_last_sent_paused = false;
double g_last_sent_at = 0.0;
double g_remote_applied_at = 0.0;
std::string g_last_seen_msg_id;

bool g_has_pending = false;
SyncEvent g_pending;

std::atomic<bool> g_running{false};
std::thread g_thread;

double mono_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

void ensure_sender() {
    if (!g_sender.empty()) return;
    unsigned seed = (unsigned)std::time(nullptr) ^ (unsigned)(uintptr_t)&g_sender;
    std::srand(seed);
    g_sender = "wiiu-" + std::to_string(10000 + (std::rand() % 90000));
}

std::string normalize_code(std::string c) {
    std::string out;
    for (char ch : c) {
        if (std::isalnum((unsigned char)ch)) out += (char)std::toupper((unsigned char)ch);
    }
    return out;
}

void stop_thread() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

void poll_loop(std::string code) {
    std::string since = "30s";
    while (g_running) {
        std::string url = "https://ntfy.sh/cfwt-" + code + "/json?poll=1&since=" + since;
        http::Response r = http::get(url, {{"User-Agent", "CoffeeFlix/1.0"}}, 8);
        if (!g_running) break;

        if (r.ok() && !r.body.empty()) {
            std::istringstream iss(r.body);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                json_error_t jerr;
                json_t* env = json_loads(line.c_str(), 0, &jerr);
                if (!env) continue;
                std::string ev = jstr(env, "event");
                std::string mid = jstr(env, "id");
                std::string msg = jstr(env, "message");
                json_decref(env);

                if (ev != "message" || mid.empty() || msg.empty()) continue;
                since = mid;

                json_t* payload = json_loads(msg.c_str(), 0, &jerr);
                if (!payload) continue;

                SyncEvent se;
                se.sender = jstr(payload, "sender");
                se.service = jstr(payload, "service");
                se.id = jstr(payload, "id");
                se.title = jstr(payload, "title");
                se.subtitle = jstr(payload, "subtitle");
                se.image = jstr(payload, "image");
                se.extra = jstr(payload, "extra");
                se.url = jstr(payload, "url");
                se.pos = json_number_value(json_object_get(payload, "pos"));
                se.paused = json_is_true(json_object_get(payload, "paused"));
                se.has_video = !json_is_false(json_object_get(payload, "has_video"));
                json_decref(payload);

                std::lock_guard<std::mutex> lk(g_mtx);
                if (se.sender != g_sender && mid != g_last_seen_msg_id) {
                    g_last_seen_msg_id = mid;
                    g_pending = std::move(se);
                    g_has_pending = true;
                    if (!g_pending.title.empty()) g_media_title = g_pending.title;
                }
            }
        }

        for (int i = 0; i < 15 && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void start_room(const std::string& code) {
    stop_thread();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ensure_sender();
        g_room = code;
        g_media_title.clear();
        g_last_sent_id.clear();
        g_last_seen_msg_id.clear();
        g_has_pending = false;
    }
    g_running = true;
    g_thread = std::thread(poll_loop, code);
    log_message(LOG_OK, "WatchTogether", "Joined room %s", code.c_str());
}

}  // namespace

bool active() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return !g_room.empty();
}

std::string room_code() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_room;
}

std::string current_media_title() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_media_title;
}

std::string create_room() {
    static const char* ALPH = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned seed = (unsigned)std::time(nullptr) ^ (unsigned)(uintptr_t)&seed;
    std::srand(seed);
    std::string code;
    for (int i = 0; i < 6; i++) code += ALPH[std::rand() % 32];
    start_room(code);
    return code;
}

bool join_room(const std::string& code) {
    std::string c = normalize_code(code);
    if (c.size() < 3) return false;
    start_room(c);
    return true;
}

void leave_room() {
    stop_thread();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_room.clear();
    g_media_title.clear();
    g_has_pending = false;
    log_message(LOG_OK, "WatchTogether", "Left room");
}

void note_remote_applied() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_remote_applied_at = mono_sec();
}

void on_local_playback(const player::Source& src, double pos, bool paused, bool has_video, bool force) {
    std::string room, sender;
    std::string payload_str;
    double now = mono_sec();

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_room.empty()) return;
        if (now - g_remote_applied_at < 2.2) return;

        std::string sid = src.id.empty() ? src.url : src.id;
        if (sid.empty()) return;

        bool changed = (sid != g_last_sent_id) || (paused != g_last_sent_paused);
        if (!changed && !force && (now - g_last_sent_at < 4.5)) return;

        g_last_sent_id = sid;
        g_last_sent_paused = paused;
        g_last_sent_at = now;
        if (!src.title.empty()) g_media_title = src.title;

        room = g_room;
        sender = g_sender;

        json_t* obj = json_object();
        json_object_set_new(obj, "sender", json_string(sender.c_str()));
        json_object_set_new(obj, "service", json_string(src.service.c_str()));
        json_object_set_new(obj, "id", json_string(sid.c_str()));
        json_object_set_new(obj, "title", json_string(src.title.c_str()));
        json_object_set_new(obj, "subtitle", json_string(src.subtitle.c_str()));
        json_object_set_new(obj, "image", json_string(src.artwork.c_str()));
        json_object_set_new(obj, "extra", json_string(src.extra.c_str()));
        json_object_set_new(obj, "url", json_string(src.url.c_str()));
        json_object_set_new(obj, "pos", json_real(pos));
        json_object_set_new(obj, "paused", json_boolean(paused));
        json_object_set_new(obj, "has_video", json_boolean(has_video));
        char* dumped = json_dumps(obj, JSON_COMPACT);
        payload_str = dumped ? dumped : "{}";
        if (dumped) free(dumped);
        json_decref(obj);
    }

    std::thread([room = std::move(room), payload_str = std::move(payload_str)]() {
        http::post_json("https://ntfy.sh/cfwt-" + room, payload_str, {{"User-Agent", "CoffeeFlix/1.0"}}, 6);
    }).detach();
}

bool poll_event(SyncEvent& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_has_pending) return false;
    out = std::move(g_pending);
    g_has_pending = false;
    return true;
}

}  // namespace watchtogether

