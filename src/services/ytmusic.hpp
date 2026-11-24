#pragma once

// YouTube Music client (WEB_REMIX InnerTube API, inspired by rukamori/ArchiveTune).

#include <string>
#include <vector>

#include "player/player.hpp"

namespace ytmusic {

enum class Filter { SONGS, ALBUMS, PLAYLISTS };

struct Track {
    std::string id;        // videoId
    std::string title;
    std::string artist;
    std::string duration;
    std::string thumbnail;
};

struct Collection {
    std::string browse_id;
    std::string title;
    std::string subtitle;
    std::string thumbnail;
};

struct SearchResults {
    std::vector<Track> tracks;
    std::vector<Collection> collections;
    std::string error;
};

struct CollectionDetail {
    std::string title;
    std::string subtitle;
    std::string thumbnail;
    std::vector<Track> tracks;
    std::string error;
};

SearchResults search(const std::string& query, Filter filter);
CollectionDetail browse(const std::string& browse_id);
player::Source make_audio_source(const Track& t);

}  // namespace ytmusic

