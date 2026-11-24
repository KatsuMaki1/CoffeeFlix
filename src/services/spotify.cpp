#include "services/spotify.hpp"

#include <jansson.h>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"
#include "services/ytmusic.hpp"

namespace spotify {

namespace {

constexpr const char* KEY_TOKEN = "spotify_access_token";
constexpr const char* KEY_USER = "spotify_user";
constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string extract_playlist_id(std::string s) {
    s = animesama::txt::trim(s);
    size_t p = s.find("playlist/");
    if (p != std::string::npos) s = s.substr(p + 9);
    size_t q = s.find_first_of("?#/");
    if (q != std::string::npos) s = s.substr(0, q);
    return animesama::txt::trim(s);
}

http::Response api_get(const std::string& url, const std::string& tok) {
    return http::get(url, {{"Authorization", "Bearer " + tok}, {"User-Agent", UA}}, 12);
}

}  // namespace

bool signed_in() { return !store::get_str(KEY_TOKEN, "").empty(); }

std::string username() { return store::get_str(KEY_USER, ""); }

bool sign_in_token(const std::string& token, std::string& err) {
    std::string tok = animesama::txt::trim(token);
    if (tok.empty()) {
        err = "Access Token Spotify vide";
        return false;
    }
    http::Response r = api_get("https://api.spotify.com/v1/me", tok);
    if (!r.ok()) {
        err = r.error.empty() ? ("Spotify HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON Spotify invalide";
        return false;
    }
    std::string name = jstr(root, "display_name");
    if (name.empty()) name = jstr(root, "id");
    if (name.empty()) name = "Spotify";
    json_decref(root);

    store::set_str(KEY_TOKEN, tok);
    store::set_str(KEY_USER, name);
    log_message(LOG_OK, "Spotify", "Signed in as %s", name.c_str());
    return true;
}

void sign_out() {
    store::set_str(KEY_TOKEN, "");
    store::set_str(KEY_USER, "");
}

TrackList fetch_playlist(const std::string& id_or_url) {
    TrackList res;
    std::string pid = extract_playlist_id(id_or_url);
    if (pid.empty()) {
        res.error = "ID playlist Spotify invalide";
        return res;
    }

    std::string url = "https://open.spotify.com/embed/playlist/" + pid;
    http::Response r = http::get(url, {{"User-Agent", UA}}, 15);
    if (!r.ok() || r.body.empty()) {
        res.error = r.error.empty() ? ("Spotify HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    std::string json_str;
    if (!animesama::txt::between(r.body, "<script id=\"__NEXT_DATA__\" type=\"application/json\">", "</script>", json_str)) {
        size_t p = r.body.find("id=\"__NEXT_DATA__\"");
        if (p != std::string::npos) {
            size_t gt = r.body.find('>', p);
            size_t end = (gt != std::string::npos) ? r.body.find("</script>", gt + 1) : std::string::npos;
            if (gt != std::string::npos && end != std::string::npos) json_str = r.body.substr(gt + 1, end - gt - 1);
        }
    }
    if (json_str.empty()) {
        res.error = "Donnees playlist Spotify introuvables";
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(json_str.c_str(), 0, &jerr);
    if (!root) {
        res.error = "JSON Spotify invalide";
        return res;
    }

    json_t* props = json_object_get(root, "props");
    json_t* pp = props ? json_object_get(props, "pageProps") : nullptr;
    json_t* st = pp ? json_object_get(pp, "state") : nullptr;
    json_t* data = st ? json_object_get(st, "data") : nullptr;
    json_t* ent = data ? json_object_get(data, "entity") : nullptr;
    if (ent) {
        res.title = jstr(ent, "name");
        json_t* ca = json_object_get(ent, "coverArt");
        json_t* srcs = ca ? json_object_get(ca, "sources") : nullptr;
        if (json_is_array(srcs) && json_array_size(srcs) > 0) {
            res.cover = jstr(json_array_get(srcs, 0), "url");
        }
        json_t* tl = json_object_get(ent, "trackList");
        if (json_is_array(tl)) {
            size_t i;
            json_t* el;
            json_array_foreach(tl, i, el) {
                Track t;
                t.id = jstr(el, "uri");
                t.title = jstr(el, "title");
                t.artist = jstr(el, "subtitle");
                t.artwork = res.cover;
                if (!t.title.empty()) res.items.push_back(std::move(t));
            }
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Playlist Spotify vide";
    log_message(LOG_OK, "Spotify", "Loaded playlist '%s' -> %d track(s)", res.title.c_str(), (int)res.items.size());
    return res;
}

TrackList user_likes() {
    TrackList res;
    std::string tok = store::get_str(KEY_TOKEN, "");
    if (tok.empty()) {
        res.error = "Connecte-toi d'abord avec ton Access Token Spotify (en haut a droite)";
        return res;
    }

    http::Response r = api_get("https://api.spotify.com/v1/me/tracks?limit=50", tok);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("Spotify HTTP " + std::to_string(r.status) + " (token expire ?)") : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON Spotify invalide";
        return res;
    }

    json_t* items = json_object_get(root, "items");
    if (json_is_array(items)) {
        size_t i;
        json_t* el;
        json_array_foreach(items, i, el) {
            json_t* tr = json_object_get(el, "track");
            if (!tr) continue;
            Track t;
            t.id = jstr(tr, "id");
            t.title = jstr(tr, "name");
            json_t* arts = json_object_get(tr, "artists");
            if (json_is_array(arts) && json_array_size(arts) > 0) {
                t.artist = jstr(json_array_get(arts, 0), "name");
            }
            json_t* alb = json_object_get(tr, "album");
            json_t* imgs = alb ? json_object_get(alb, "images") : nullptr;
            if (json_is_array(imgs) && json_array_size(imgs) > 0) {
                t.artwork = jstr(json_array_get(imgs, 0), "url");
            }
            if (!t.title.empty()) res.items.push_back(std::move(t));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucun titre like trouve";
    return res;
}

TrackList search(const std::string& query) {
    TrackList res;
    std::string q = animesama::txt::trim(query);
    if (q.empty()) return fetch_playlist("37i9dQZF1DXcBWIGoYBM5M");

    std::string tok = store::get_str(KEY_TOKEN, "");
    if (!tok.empty()) {
        http::Response r = api_get("https://api.spotify.com/v1/search?type=track&limit=25&q=" +
                                       animesama::txt::url_encode(q),
                                   tok);
        if (r.ok()) {
            json_error_t jerr;
            json_t* root = json_loads(r.body.c_str(), 0, &jerr);
            json_t* trs = root ? json_object_get(root, "tracks") : nullptr;
            json_t* items = trs ? json_object_get(trs, "items") : nullptr;
            if (json_is_array(items)) {
                size_t i;
                json_t* tr;
                json_array_foreach(items, i, tr) {
                    Track t;
                    t.id = jstr(tr, "id");
                    t.title = jstr(tr, "name");
                    json_t* arts = json_object_get(tr, "artists");
                    if (json_is_array(arts) && json_array_size(arts) > 0) {
                        t.artist = jstr(json_array_get(arts, 0), "name");
                    }
                    json_t* alb = json_object_get(tr, "album");
                    json_t* imgs = alb ? json_object_get(alb, "images") : nullptr;
                    if (json_is_array(imgs) && json_array_size(imgs) > 0) {
                        t.artwork = jstr(json_array_get(imgs, 0), "url");
                    }
                    if (!t.title.empty()) res.items.push_back(std::move(t));
                }
            }
            if (root) json_decref(root);
            if (!res.items.empty()) return res;
        }
    }

    // Fallback when logged out: search via YT Music catalogue so search always works
    ytmusic::SearchResults ym = ytmusic::search(q, ytmusic::Filter::SONGS);
    res.error = ym.error;
    for (const auto& yt : ym.tracks) {
        Track t;
        t.id = "yt:" + yt.id;
        t.title = yt.title;
        t.artist = yt.artist;
        t.artwork = yt.thumbnail;
        res.items.push_back(std::move(t));
    }
    return res;
}

player::Source make_source(const Track& t) {
    if (animesama::txt::starts_with(t.id, "yt:")) {
        ytmusic::Track yt;
        yt.id = t.id.substr(3);
        yt.title = t.title;
        yt.artist = t.artist;
        yt.thumbnail = t.artwork;
        return ytmusic::make_audio_source(yt);
    }

    player::Source src;
    src.title = t.title;
    src.subtitle = t.artist;
    src.artwork = t.artwork;
    src.service = "spotify";
    src.id = t.id.empty() ? (t.title + "|" + t.artist) : t.id;
    src.remember_position = false;

    std::string q = t.artist.empty() ? t.title : (t.title + " " + t.artist);
    src.resolve = [q, title = t.title, artist = t.artist, art = t.artwork](player::Source& s, std::string& err) -> bool {
        ytmusic::SearchResults ym = ytmusic::search(q, ytmusic::Filter::SONGS);
        if (ym.tracks.empty()) {
            err = ym.error.empty() ? "Audio introuvable pour ce titre Spotify" : ym.error;
            return false;
        }
        player::Source ysrc = ytmusic::make_audio_source(ym.tracks[0]);
        if (!ysrc.resolve || !ysrc.resolve(s, err)) return false;
        if (!title.empty()) s.title = title;
        if (!artist.empty()) s.subtitle = artist;
        if (!art.empty()) s.artwork = art;
        return true;
    };
    return src;
}

}  // namespace spotify

