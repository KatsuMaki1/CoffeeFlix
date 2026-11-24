#pragma once

// MyAnimeList API v2 anime watch-progress synchronization client.

#include <string>

namespace mal {

bool enabled();
void set_enabled(bool on);

bool signed_in();
std::string username();
std::string client_id();

void set_client_id(const std::string& id);
bool authenticate(const std::string& access_token, const std::string& refresh_token, std::string& err);
void sign_out();

// Called from player::report_progress() for anime video playback (e.g. Anime-Sama).
void on_progress(const std::string& service, const std::string& id, const std::string& title,
                 const std::string& subtitle, double pos, double duration);

}  // namespace mal

