#pragma once

// Freebox TV (Multiposte M3U/RTSP) + DRM-free French Live TV (France.tv, ARTE, etc.) & France.tv Replays.

#include <string>
#include <vector>

#include "player/player.hpp"

namespace freetv {

struct Channel {
    std::string id;
    std::string name;
    std::string group;
    std::string url;       // rtsp:// or https:// or internal resolver ID
    std::string image_url;
    bool is_freebox = false;
};

struct ChannelList {
    std::vector<Channel> items;
    std::string error;
};

std::string freebox_url();
void set_freebox_url(const std::string& url);

ChannelList get_freebox_channels();
ChannelList get_fr_live_channels();
ChannelList get_francetv_replays(const std::string& channel_slug);
ChannelList get_iptv_channels(const std::string& playlist_key);

std::string iptv_custom_url();
void set_iptv_custom_url(const std::string& url);

player::Source make_channel_source(const Channel& ch);

}  // namespace freetv
