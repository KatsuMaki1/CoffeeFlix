#pragma once

// Spotify catalogue (public embed playlists + optional Bearer token login for Likes) with full YT Music audio playback.

#include <string>
#include <vector>

#include "player/player.hpp"

namespace spotify {

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string artwork;
};

struct TrackList {
    std::string title;
    std::string cover;
    std::vector<Track> items;
    std::string error;
};

bool signed_in();
std::string username();
bool sign_in_token(const std::string& token, std::string& err);
void sign_out();

TrackList fetch_playlist(const std::string& id_or_url);
TrackList user_likes();
TrackList search(const std::string& query);

player::Source make_source(const Track& t);

}  // namespace spotify

