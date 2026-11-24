#pragma once

// Client for Starland9/streams_dl FastAPI server (Flemmix, PapaduStream, French-Stream).

#include <string>
#include <vector>

#include "player/player.hpp"

namespace streams_dl {

struct Media {
    std::string title;
    std::string url;
    std::string image_url;
};

struct SearchResults {
    std::vector<Media> items;
    std::string error;
};

struct Video {
    std::string title;
    std::string url;
    std::string html_url;
    std::string image_url;
    std::string duration;
    std::string resolution;
};

struct VideoResults {
    std::vector<Video> items;
    std::string error;
};

std::string server_url();
void set_server_url(const std::string& url);

std::vector<std::string> get_providers(std::string& err);
SearchResults search(const std::string& query, const std::string& provider);
VideoResults get_videos(const std::string& media_url, const std::string& provider);

player::Source make_source(const std::string& media_title, const std::string& media_cover, const Video& v);

}  // namespace streams_dl

