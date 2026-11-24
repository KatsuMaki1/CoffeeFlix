#pragma once

// Native C++ port of AnimeSamaApi (https://github.com/TMCooper/AnimeSamaApi, GPL-3.0)

#include <string>
#include <vector>

#include "player/player.hpp"

namespace animesama {

enum class Lang { VOSTFR, VF };
const char* lang_name(Lang l);

struct SearchResult {
    std::string title;
    std::string url;
    std::string cover;
};

struct SearchResults {
    std::vector<SearchResult> items;
    std::string error;
};

struct Season {
    std::string name;
    std::string url;
};

struct AnimeInfo {
    std::string title;
    std::string url;
    std::string cover;
    std::string synopsis;
    std::vector<std::string> alt_titles;
    std::vector<Season> seasons;
    std::string error;
};

struct Episode {
    int number = 0;
    std::vector<std::string> sources;
};

struct EpisodeList {
    std::string page_url;
    std::string jeton;
    std::vector<Episode> episodes;
    std::string error;
};

struct Stream {
    std::string url;
    std::string type;  // "m3u8", "mp4", "embed", "raw"
    std::string referer;
};

bool find_base_url(std::string& out, std::string& err);
SearchResults search(const std::string& query);
AnimeInfo get_info(const SearchResult& anime);
EpisodeList get_episodes(const Season& season, Lang lang);
bool resolve_episode(const EpisodeList& list, const Episode& ep, Stream& out, std::string& err);

player::Source make_source(const AnimeInfo& info, const Season& season, const EpisodeList& list, const Episode& ep);

// Internal resolver helpers
bool resolve_video_url(const std::string& url, Stream& out);
int check_stream_health(const Stream& stream);
std::string unpack_packer(const std::string& html);

}  // namespace animesama
