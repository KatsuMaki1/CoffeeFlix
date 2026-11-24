#pragma once

// Last.fm scrobbling & Now Playing client (https://ws.audioscrobbler.com/2.0/).

#include <string>

namespace lastfm {

bool enabled();
void set_enabled(bool on);

bool signed_in();
std::string username();
std::string api_key();
std::string api_secret();

void set_api_credentials(const std::string& key, const std::string& secret);
bool authenticate(const std::string& user, const std::string& password, std::string& err);
void sign_out();

// Called from player::update() on the main thread for audio playback.
void on_playback(const std::string& id, const std::string& title, const std::string& artist, double pos,
                 double duration, bool paused);

}  // namespace lastfm

