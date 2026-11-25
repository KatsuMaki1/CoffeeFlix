#pragma once

// Chromecast-style receiver for CoffeeFlix:
// 1. YouTube Lounge TV Pairing (official YouTube mobile Cast button via TV code + DIAL).
// 2. Local DIAL SSDP + HTTP server (port 8008) + Web Cast Remote page (http://<wiiu_ip>:8008/).

#include <string>

namespace cast_receiver {

struct Command {
    enum Type { NONE, PLAY_YOUTUBE, PLAY_URL, PLAY, PAUSE, SEEK, NEXT, PREV, STOP };
    Type type = NONE;
    std::string target;  // videoId or media URL
    std::string title;
    double position = 0.0;
};

void start();
void stop();
bool running();

std::string pairing_code();  // formatted XXX-XXX-XXX-XXX
std::string web_url();       // http://<ip>:8008/
std::string connected_device();
void refresh_pairing_code();

// Called once per frame on the main thread.
bool poll_command(Command& out);

}  // namespace cast_receiver

