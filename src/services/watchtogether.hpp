#pragma once

// WatchTogether: serverless-relay room synchronization (via ntfy.sh) for YouTube, Anime-Sama, StreamsDL, etc.

#include <string>
#include <vector>

#include "player/player.hpp"

namespace watchtogether {

struct ChatMsg {
    std::string sender;
    std::string text;
};

struct SyncEvent {
    bool is_chat = false;
    std::string sender;
    std::string service;
    std::string id;
    std::string title;
    std::string subtitle;
    std::string image;
    std::string extra;
    std::string url;
    std::string referer;
    double pos = 0.0;
    bool paused = false;
    bool has_video = true;
    bool chunked = false;
};

bool active();
std::string room_code();
std::string current_media_title();

std::string create_room();
bool join_room(const std::string& code);
void leave_room();

void send_chat(const std::string& text);
std::vector<ChatMsg> chat_history();

// Called from player::report_progress() to broadcast local playback state.
void on_local_playback(const player::Source& src, double pos, bool paused, bool has_video, bool force);

// Marks that a remote sync was just applied (anti-echo guard).
void note_remote_applied();

// Returns true if a new remote SyncEvent arrived for the main thread to apply.
bool poll_event(SyncEvent& out);

}  // namespace watchtogether

