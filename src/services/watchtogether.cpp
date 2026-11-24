#include "services/watchtogether.hpp"

#include <jansson.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#include "core/http.hpp"
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
double g_last_sent_pos = 0.0;
double g_last_sent_at = 0.0;
double g_remote_applied_at = 0.0;
std::string g_last_seen_msg_id;

bool g_has_pending = false;
SyncEvent g_pending;
std::vector<ChatMsg> g_chat;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
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
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

void handle_line(const std::string& line, std::string& since) {
    if (line.empty()) return;
    json_error_t jerr;
    json_t* env = json_loads(line.c_str(), 0, &jerr);
    if (!env) return;
    std::string ev = jstr(env, "event");
    std::string mid = jstr(env, "id");
    std::string msg = jstr(env, "message");
    json_decref(env);

    if (ev != "message" || mid.empty() || msg.empty()) return;
    since = mid;

    json_t* payload = json_loads(msg.c_str(), 0, &jerr);
    if (!payload) return;

    SyncEvent se;
    std::string mtype = jstr(payload, "type");
    se.sender = jstr(payload, "sender");
    if (mtype == "chat") {
        se.is_chat = true;
        se.title = jstr(payload, "text");
        json_decref(payload);
        if (se.title.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (se.sender != g_sender && mid != g_last_seen_msg_id) {
            g_last_seen_msg_id = mid;
            g_chat.push_back({se.sender, se.title});
            if (g_chat.size() > 20) g_chat.erase(g_chat.begin());
            g_pending = std::move(se);
            g_has_pending = true;
        }
        return;
    }

    se.service = jstr(payload, "service");
    se.id = jstr(payload, "id");
    se.title = jstr(payload, "title");
    se.subtitle = jstr(payload, "subtitle");
    se.image = jstr(payload, "image");
    se.extra = jstr(payload, "extra");
    se.url = jstr(payload, "url");
    se.referer = jstr(payload, "referer");
    se.pos = json_number_value(json_object_get(payload, "pos"));
    se.paused = json_is_true(json_object_get(payload, "paused"));
    se.has_video = !json_is_false(json_object_get(payload, "has_video"));
    se.chunked = json_is_true(json_object_get(payload, "chunked"));
    json_decref(payload);

    std::lock_guard<std::mutex> lk(g_mtx);
    if (se.sender != g_sender && mid != g_last_seen_msg_id) {
        g_last_seen_msg_id = mid;
        g_pending = std::move(se);
        g_has_pending = true;
        if (!g_pending.title.empty()) g_media_title = g_pending.title;
    }
}

void poll_loop(std::string code) {
    std::string since = "10s";
    while (g_running) {
        std::string buf;
        http::Request req;
        req.url = "https://ntfy.sh/cfwt-" + code + "/json?since=" + since;
        req.timeout = 60;
        req.cancel = &g_stop;
        req.headers.push_back({"User-Agent", "CoffeeFlix/1.0"});
        req.on_data = [&buf, &since](const http::Response&, const char* data, size_t size) -> bool {
            if (!g_running) return false;
            buf.append(data, size);
            size_t nl = 0;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                handle_line(line, since);
            }
            return g_running.load();
        };
        http::perform(req);
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
    g_stop = false;
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
    g_chat.clear();
    g_has_pending = false;
    log_message(LOG_OK, "WatchTogether", "Left room");
}

void send_chat(const std::string& text) {
    std::string clean = animesama::txt::trim(text);
    if (clean.empty()) return;
    std::string room, payload_str;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_room.empty()) return;
        room = g_room;
        g_chat.push_back({"Moi", clean});
        if (g_chat.size() > 20) g_chat.erase(g_chat.begin());

        json_t* obj = json_object();
        json_object_set_new(obj, "type", json_string("chat"));
        json_object_set_new(obj, "sender", json_string(g_sender.c_str()));
        json_object_set_new(obj, "text", json_string(clean.c_str()));
        char* dumped = json_dumps(obj, JSON_COMPACT);
        payload_str = dumped ? dumped : "{}";
        if (dumped) free(dumped);
        json_decref(obj);
    }
    std::thread([room = std::move(room), payload_str = std::move(payload_str)]() {
        http::post_json("https://ntfy.sh/cfwt-" + room, payload_str, {{"User-Agent", "CoffeeFlix/1.0"}}, 6);
    }).detach();
}

std::vector<ChatMsg> chat_history() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_chat;
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
        if (now - g_remote_applied_at < 1.2) return;

        std::string sid = src.id.empty() ? src.url : src.id;
        if (sid.empty()) return;

        double dt_sent = now - g_last_sent_at;
        double expected_pos = g_last_sent_paused ? g_last_sent_pos : (g_last_sent_pos + dt_sent);
        bool jumped = std::abs(pos - expected_pos) > 2.2;
        bool changed = (sid != g_last_sent_id) || (paused != g_last_sent_paused) || jumped;
        if (!changed && !force && dt_sent < 3.0) return;

        g_last_sent_id = sid;
        g_last_sent_paused = paused;
        g_last_sent_pos = pos;
        g_last_sent_at = now;
        if (!src.title.empty()) g_media_title = src.title;

        std::string referer;
        for (const auto& h : src.headers) {
            if (animesama::txt::lower(h.first) == "referer") {
                referer = h.second;
                break;
            }
        }

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
        json_object_set_new(obj, "referer", json_string(referer.c_str()));
        json_object_set_new(obj, "pos", json_real(pos));
        json_object_set_new(obj, "paused", json_boolean(paused));
        json_object_set_new(obj, "has_video", json_boolean(has_video));
        json_object_set_new(obj, "chunked", json_boolean(src.chunked_http));
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
