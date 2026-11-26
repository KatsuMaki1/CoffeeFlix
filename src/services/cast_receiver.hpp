#pragma once

// Chromecast-style receiver & Mobile Remote Control API (port 8008):
// 1. YouTube Lounge TV Pairing (official YouTube mobile Cast button via TV code + DIAL).
// 2. Local DIAL SSDP + HTTP server (port 8008) + Installable Android PWA Remote UI & REST API.

#include <string>

namespace cast_receiver {

struct Command {
    enum Type {
        NONE,
        PLAY_YOUTUBE,
        PLAY_URL,
        PLAY,
        PAUSE,
        TOGGLE_PAUSE,
        SEEK,
        SEEK_REL,
        NEXT,
        PREV,
        STOP,
        VOLUME,
        NAV,      // target = "up"|"down"|"left"|"right"|"a"|"b"|"menu"
        SECTION,  // target = section id ("home", "animesama", "ytmusic", ...)
    };
    Type type = NONE;
    std::string target;
    std::string title;
    double position = 0.0;
};

struct LiveState {
    bool started = false;
    bool paused = false;
    bool live = false;
    bool has_next = false;
    bool has_prev = false;
    double pos = 0.0;
    double duration = 0.0;
    int volume = 100;
    std::string title;
    std::string subtitle;
};

void start();
void stop();
bool running();

std::string pairing_code();  // formatted XXX-XXX-XXX-XXX
std::string web_url();       // http://<ip>:8008/
std::string connected_device();
void refresh_pairing_code();

void update_state(const LiveState& st);

// Called once per frame on the main thread.
bool poll_command(Command& out);

}  // namespace cast_receiver
