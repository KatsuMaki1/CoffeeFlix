#pragma once

// Direct French movies & series scraper (auto-fetches from french-streaming.tv, no external server required).

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
    std::string url;       // embed URL or direct stream URL
    std::string html_url;  // media page URL (Referer)
    std::string image_url;
    std::string resolution;
};

struct VideoResults {
    std::vector<Video> items;
    std::string error;
};

SearchResults browse(const std::string& category);
SearchResults search(const std::string& query, const std::string& category);
VideoResults get_videos(const std::string& media_url, const std::string& category);

player::Source make_source(const std::string& media_title, const std::string& media_cover, const Video& v);

}  // namespace streams_dl
