#pragma once

// SoundCloud v2 API client (auto client_id discovery, charts, search, progressive/HLS audio playback).

#include <string>
#include <vector>

#include "player/player.hpp"

namespace soundcloud {

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string artwork;
    std::string transcoding_url;
    bool is_progressive = false;
    int duration_sec = 0;
};

struct TrackList {
    std::vector<Track> items;
    std::string error;
};

TrackList charts(const std::string& kind = "trending");
TrackList search(const std::string& query);
TrackList user_likes();

bool signed_in();
std::string username();
bool sign_in_username(const std::string& user_or_url, std::string& err);
void sign_out();

player::Source make_source(const Track& t);

}  // namespace soundcloud

