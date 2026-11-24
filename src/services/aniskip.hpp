#pragma once

// AniSkip (https://api.aniskip.com) opening/ending auto-skip lookup for anime episodes.

#include <string>
#include <vector>

#include "player/player.hpp"

namespace aniskip {

// Looks up MAL ID via Jikan and fetches OP/ED skip segments from AniSkip.
std::vector<player::SkipSegment> fetch_segments(const std::string& anime_title, int ep_num);

}  // namespace aniskip

