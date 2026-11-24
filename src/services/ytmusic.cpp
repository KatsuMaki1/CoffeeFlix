#include "services/ytmusic.hpp"

#include <jansson.h>

#include <cstdlib>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/youtube.hpp"

namespace ytmusic {

namespace {

constexpr const char* MUSIC_API = "https://music.youtube.com/youtubei/v1/";
constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

json_t* jat(json_t* root, std::initializer_list<const char*> path) {
    json_t* cur = root;
    for (const char* k : path) {
        if (!cur || !json_is_object(cur)) return nullptr;
        cur = json_object_get(cur, k);
    }
    return cur;
}

std::string runs_text(json_t* obj) {
    if (!obj || !json_is_object(obj)) return "";
    std::string s = jstr(obj, "simpleText");
    if (!s.empty()) return s;
    json_t* runs = json_object_get(obj, "runs");
    if (!json_is_array(runs)) return "";
    std::string out;
    size_t i;
    json_t* el;
    json_array_foreach(runs, i, el) { out += jstr(el, "text"); }
    return out;
}

std::string best_thumb(json_t* obj) {
    json_t* thumbs = jat(obj, {"thumbnail", "musicThumbnailRenderer", "thumbnail", "thumbnails"});
    if (!json_is_array(thumbs) || json_array_size(thumbs) == 0)
        thumbs = jat(obj, {"thumbnails"});
    if (!json_is_array(thumbs) || json_array_size(thumbs) == 0) return "";
    json_t* last = json_array_get(thumbs, json_array_size(thumbs) - 1);
    return jstr(last, "url");
}

json_t* call_music(const char* endpoint, json_t* body, std::string& err) {
    json_t* client = json_object();
    json_object_set_new(client, "clientName", json_string("WEB_REMIX"));
    json_object_set_new(client, "clientVersion", json_string("1.20250922.01.00"));
    json_object_set_new(client, "hl", json_string("fr"));
    json_object_set_new(client, "gl", json_string("FR"));

    json_t* ctx = json_object();
    json_object_set_new(ctx, "client", client);
    json_object_set_new(body, "context", ctx);

    char* dumped = json_dumps(body, JSON_COMPACT);
    std::string payload = dumped ? dumped : "{}";
    if (dumped) free(dumped);
    json_decref(body);

    http::Request req;
    req.method = "POST";
    req.url = std::string(MUSIC_API) + endpoint + "?prettyPrint=false";
    req.body = payload;
    req.timeout = 15;
    req.headers.push_back({"Content-Type", "application/json"});
    req.headers.push_back({"User-Agent", UA});
    req.headers.push_back({"Origin", "https://music.youtube.com"});
    req.headers.push_back({"Referer", "https://music.youtube.com/"});

    http::Response r = http::perform(req);
    if (!r.ok()) {
        err = r.error.empty() ? ("YT Music HTTP " + std::to_string(r.status)) : r.error;
        return nullptr;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON YT Music invalide";
        return nullptr;
    }
    return root;
}

std::string col_text(json_t* item, size_t idx) {
    json_t* cols = json_object_get(item, "flexColumns");
    if (!json_is_array(cols) || idx >= json_array_size(cols)) return "";
    json_t* col = json_array_get(cols, idx);
    json_t* text_obj = jat(col, {"musicResponsiveListItemFlexColumnRenderer", "text"});
    return runs_text(text_obj);
}

std::string extract_video_id(json_t* item) {
    std::string id = jstr(json_object_get(item, "playlistItemData"), "videoId");
    if (!id.empty()) return id;
    json_t* we = jat(item, {"overlay", "musicItemThumbnailOverlayRenderer", "content", "musicPlayButtonRenderer",
                            "playNavigationEndpoint", "watchEndpoint"});
    id = jstr(we, "videoId");
    if (!id.empty()) return id;
    json_t* cols = json_object_get(item, "flexColumns");
    if (json_is_array(cols) && json_array_size(cols) > 0) {
        json_t* runs = jat(json_array_get(cols, 0), {"musicResponsiveListItemFlexColumnRenderer", "text", "runs"});
        if (json_is_array(runs) && json_array_size(runs) > 0) {
            id = jstr(jat(json_array_get(runs, 0), {"navigationEndpoint", "watchEndpoint"}), "videoId");
        }
    }
    return id;
}

void collect_items(json_t* node, Filter filter, SearchResults& out) {
    if (!node) return;
    if (json_is_array(node)) {
        size_t i;
        json_t* el;
        json_array_foreach(node, i, el) { collect_items(el, filter, out); }
        return;
    }
    if (!json_is_object(node)) return;

    json_t* item = json_object_get(node, "musicResponsiveListItemRenderer");
    if (item && json_is_object(item)) {
        if (filter == Filter::SONGS) {
            if (out.tracks.size() >= 30) return;
            std::string vid = extract_video_id(item);
            std::string title = col_text(item, 0);
            if (!vid.empty() && !title.empty()) {
                Track t;
                t.id = vid;
                t.title = title;
                t.artist = col_text(item, 1);
                t.thumbnail = best_thumb(item);
                if (t.thumbnail.empty()) t.thumbnail = "https://i.ytimg.com/vi/" + vid + "/hqdefault.jpg";
                out.tracks.push_back(std::move(t));
            }
        } else {
            if (out.collections.size() >= 24) return;
            std::string bid = jstr(jat(item, {"navigationEndpoint", "browseEndpoint"}), "browseId");
            std::string title = col_text(item, 0);
            if (!bid.empty() && !title.empty()) {
                Collection c;
                c.browse_id = bid;
                c.title = title;
                c.subtitle = col_text(item, 1);
                c.thumbnail = best_thumb(item);
                out.collections.push_back(std::move(c));
            }
        }
        return;
    }

    const char* key;
    json_t* val;
    json_object_foreach(node, key, val) {
        if (json_is_object(val) || json_is_array(val)) collect_items(val, filter, out);
    }
}

void collect_tracks(json_t* node, std::vector<Track>& out, const std::string& fallback_thumb) {
    if (!node || out.size() >= 100) return;
    if (json_is_array(node)) {
        size_t i;
        json_t* el;
        json_array_foreach(node, i, el) { collect_tracks(el, out, fallback_thumb); }
        return;
    }
    if (!json_is_object(node)) return;

    json_t* item = json_object_get(node, "musicResponsiveListItemRenderer");
    if (item && json_is_object(item)) {
        std::string vid = extract_video_id(item);
        std::string title = col_text(item, 0);
        if (!vid.empty() && !title.empty()) {
            Track t;
            t.id = vid;
            t.title = title;
            t.artist = col_text(item, 1);
            t.thumbnail = best_thumb(item);
            if (t.thumbnail.empty()) t.thumbnail = fallback_thumb;
            if (t.thumbnail.empty()) t.thumbnail = "https://i.ytimg.com/vi/" + vid + "/hqdefault.jpg";
            out.push_back(std::move(t));
        }
        return;
    }

    const char* key;
    json_t* val;
    json_object_foreach(node, key, val) {
        if (json_is_object(val) || json_is_array(val)) collect_tracks(val, out, fallback_thumb);
    }
}

}  // namespace

SearchResults search(const std::string& query, Filter filter) {
    SearchResults res;
    if (query.empty()) {
        res.error = "Recherche vide";
        return res;
    }

    const char* params = "EgWKAQIIAWoKEAkQBRAKEAMQBA%3D%3D";  // SONGS
    if (filter == Filter::ALBUMS) params = "EgWKAQIYAWoKEAkQBRAKEAMQBA%3D%3D";
    else if (filter == Filter::PLAYLISTS) params = "EgeKAQQoAEABagwQDhAKEAkQAxAEEAU%3D";

    json_t* body = json_object();
    json_object_set_new(body, "query", json_string(query.c_str()));
    json_object_set_new(body, "params", json_string(params));

    json_t* root = call_music("search", body, res.error);
    if (!root) return res;

    collect_items(root, filter, res);
    json_decref(root);
    log_message(LOG_OK, "YTMusic", "search('%s') -> %d tracks, %d collections", query.c_str(), (int)res.tracks.size(),
                (int)res.collections.size());
    return res;
}

CollectionDetail browse(const std::string& browse_id) {
    CollectionDetail detail;
    json_t* body = json_object();
    json_object_set_new(body, "browseId", json_string(browse_id.c_str()));

    json_t* root = call_music("browse", body, detail.error);
    if (!root) return detail;

    json_t* hdr = jat(root, {"header", "musicDetailHeaderRenderer"});
    if (!hdr) hdr = jat(root, {"header", "musicResponsiveHeaderRenderer"});
    if (hdr) {
        detail.title = runs_text(json_object_get(hdr, "title"));
        detail.subtitle = runs_text(json_object_get(hdr, "subtitle"));
        detail.thumbnail = best_thumb(hdr);
    }

    collect_tracks(root, detail.tracks, detail.thumbnail);
    json_decref(root);
    log_message(LOG_OK, "YTMusic", "browse('%s') -> %d track(s)", browse_id.c_str(), (int)detail.tracks.size());
    return detail;
}

player::Source make_audio_source(const Track& t) {
    youtube::Video v;
    v.id = t.id;
    v.title = t.title;
    v.channel = t.artist;
    player::Source src = youtube::make_source(v);
    src.artwork = t.thumbnail;
    src.subtitle = t.artist;
    return src;
}

}  // namespace ytmusic

