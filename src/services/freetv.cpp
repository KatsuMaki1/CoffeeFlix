#include "services/freetv.hpp"

#include <jansson.h>

#include <sstream>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"
#include "services/dlna.hpp"

namespace freetv {

namespace {

constexpr const char* STORE_FREEBOX_URL = "freebox_playlist_url";
constexpr const char* STORE_IPTV_URL = "iptv_custom_m3u_url";
constexpr const char* DEFAULT_FREEBOX_URLS[] = {
    "http://mafreebox.freebox.fr/freeboxtv/playlist.m3u",
    "http://192.168.1.254/freeboxtv/playlist.m3u",
    "http://212.27.38.253/freeboxtv/playlist.m3u",
};
constexpr const char* UA_IOS = "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15";

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

bool resolve_francetv_id(const std::string& broadcast_id, std::string& out_url, std::string& err) {
    std::string info_url = "https://k7.ftven.fr/videos/" + broadcast_id +
                           "?country_code=FR&os=ios&diffusion_mode=tunnel_first&offline=false";
    http::Response r = http::get(info_url, {{"User-Agent", UA_IOS}}, 12);
    if (!r.ok()) {
        err = r.error.empty() ? ("France.tv HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON France.tv invalide";
        return false;
    }

    json_t* video = json_object_get(root, "video");
    std::string raw_url = jstr(video, "url");
    std::string tok_ep = jstr(video, "token");
    bool drm = json_is_true(json_object_get(video, "drm"));
    json_decref(root);

    if (raw_url.empty()) {
        err = "Flux France.tv indisponible";
        return false;
    }
    if (drm) {
        err = "Ce programme France.tv est protege par DRM";
        return false;
    }

    if (tok_ep.empty()) tok_ep = "https://hdfauth.ftven.fr/esi/TA";
    std::string sign_url = tok_ep + (tok_ep.find('?') != std::string::npos ? "&" : "?") +
                           "format=json&url=" + animesama::txt::url_encode(raw_url);
    http::Response sr = http::get(sign_url, {{"User-Agent", UA_IOS}}, 10);
    if (!sr.ok()) {
        err = sr.error.empty() ? ("Token France.tv HTTP " + std::to_string(sr.status)) : sr.error;
        return false;
    }

    json_t* sroot = json_loads(sr.body.c_str(), 0, &jerr);
    if (!sroot) {
        err = "Reponse token France.tv invalide";
        return false;
    }
    out_url = jstr(sroot, "url");
    json_decref(sroot);

    if (out_url.empty()) {
        err = "URL signee France.tv vide";
        return false;
    }
    return true;
}

bool resolve_arte_live(std::string& out_url, std::string& err) {
    http::Response r = http::get("https://api.arte.tv/api/player/v2/config/fr/LIVE", {{"User-Agent", UA_IOS}}, 10);
    if (!r.ok()) {
        err = r.error.empty() ? ("ARTE HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }
    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        err = "Reponse JSON ARTE invalide";
        return false;
    }
    json_t* data = json_object_get(root, "data");
    json_t* attrs = data ? json_object_get(data, "attributes") : nullptr;
    json_t* streams = attrs ? json_object_get(attrs, "streams") : nullptr;
    if (json_is_array(streams) && json_array_size(streams) > 0) {
        json_t* first = json_array_get(streams, 0);
        out_url = jstr(first, "url");
    }
    json_decref(root);
    if (out_url.empty()) {
        err = "Flux ARTE introuvable";
        return false;
    }
    return true;
}

}  // namespace

std::string freebox_url() {
    std::string u = animesama::txt::trim(store::get_str(STORE_FREEBOX_URL, ""));
    return u.empty() ? std::string(DEFAULT_FREEBOX_URLS[0]) : u;
}

void set_freebox_url(const std::string& url) { store::set_str(STORE_FREEBOX_URL, animesama::txt::trim(url)); }

ChannelList get_freebox_channels() {
    ChannelList res;

    // 1. If user explicitly set a custom HTTP M3U playlist URL, load HTTP streams from it.
    std::string custom = animesama::txt::trim(store::get_str(STORE_FREEBOX_URL, ""));
    if (!custom.empty()) {
        http::Response r = http::get(custom, {}, 8);
        if (r.ok() && r.body.find("#EXTM3U") != std::string::npos) {
            std::istringstream iss(r.body);
            std::string line, cur_name;
            while (std::getline(iss, line)) {
                line = animesama::txt::trim(line);
                if (line.empty()) continue;
                if (animesama::txt::starts_with(line, "#EXTINF:")) {
                    size_t comma = line.rfind(',');
                    cur_name = (comma != std::string::npos) ? animesama::txt::trim(line.substr(comma + 1)) : "Chaine";
                } else if (line[0] != '#' && animesama::txt::starts_with(line, "http")) {
                    Channel ch;
                    ch.id = line;
                    ch.name = cur_name.empty() ? line : cur_name;
                    ch.group = "Freebox TV";
                    ch.url = line;
                    ch.is_freebox = true;
                    res.items.push_back(std::move(ch));
                    cur_name.clear();
                }
            }
            if (!res.items.empty()) return res;
        }
    }

    // 2. Auto-discover Freebox DLNA/UPnP server (same HTTP MPEG-TS method used by PS3, no RTSP needed!)
    std::vector<dlna::Server> servers = dlna::known_servers();
    auto find_freebox = [](const std::vector<dlna::Server>& list) -> const dlna::Server* {
        for (const auto& s : list) {
            if (animesama::txt::contains_ci(s.name, "freebox") || animesama::txt::contains_ci(s.model, "freebox")) {
                return &s;
            }
        }
        return nullptr;
    };

    const dlna::Server* fbx = find_freebox(servers);
    dlna::Servers discovered;
    if (!fbx) {
        discovered = dlna::discover(2500);
        fbx = find_freebox(discovered.items);
    }

    if (!fbx) {
        res.error = "Freebox introuvable en DLNA/UPnP sur le reseau local. Verifie que UPnP AV est active sur ta Freebox.";
        return res;
    }

    dlna::Listing root = dlna::browse(*fbx, "0");
    if (!root.ok) {
        res.error = root.error.empty() ? "Impossible de lire le serveur DLNA Freebox" : root.error;
        return res;
    }

    auto collect_videos = [&](const dlna::Listing& lst) {
        for (const auto& it : lst.items) {
            if (it.kind == dlna::VIDEO && animesama::txt::starts_with(it.url, "http")) {
                Channel ch;
                ch.id = it.url;
                ch.name = it.title;
                ch.group = "Freebox TV (DLNA)";
                ch.url = it.url;
                ch.image_url = it.art;
                ch.is_freebox = true;
                res.items.push_back(std::move(ch));
            }
        }
    };

    // Look for TV / Television / Chaines folder in root
    std::string tv_id;
    for (const auto& it : root.items) {
        if (it.kind != dlna::CONTAINER) continue;
        std::string low = animesama::txt::lower(it.title);
        if (low.find("tv") != std::string::npos || low.find("vision") != std::string::npos ||
            low.find("chaine") != std::string::npos || low.find("chaîne") != std::string::npos) {
            tv_id = it.id;
            break;
        }
    }

    if (!tv_id.empty()) {
        dlna::Listing tv_lst = dlna::browse(*fbx, tv_id);
        collect_videos(tv_lst);
        if (res.items.empty()) {
            // Browse sub-containers one level deeper (e.g. "Toutes les chaines" / "TNT")
            for (const auto& sub : tv_lst.items) {
                if (sub.kind == dlna::CONTAINER) {
                    dlna::Listing sub_lst = dlna::browse(*fbx, sub.id);
                    collect_videos(sub_lst);
                    if (!res.items.empty()) break;
                }
            }
        }
    }

    if (res.items.empty()) {
        res.error = "Aucune chaine TV HTTP trouvee sur le serveur DLNA Freebox (" + fbx->name + ").";
    }
    log_message(LOG_OK, "FreeTV", "Loaded %d Freebox DLNA channel(s) from %s", (int)res.items.size(), fbx->name.c_str());
    return res;
}

ChannelList get_fr_live_channels() {
    ChannelList res;
    auto add = [&](const char* id, const char* name, const char* group, const char* url) {
        Channel ch;
        ch.id = id;
        ch.name = name;
        ch.group = group;
        ch.url = url;
        res.items.push_back(std::move(ch));
    };

    add("ftv:006194ea-117d-4bcf-94a9-153d999c59ae", "France 2 (Direct)", "France.tv", "");
    add("ftv:29bdf749-7082-4426-a4f3-595cc436aa0d", "France 3 (Direct)", "France.tv", "");
    add("ftv:beb2e35a-5815-4ac4-95df-cdc8a69c49b7", "France 4 (Direct)", "France.tv", "");
    add("ftv:45007886-f3ff-4b3e-9706-1ef1014c5a60", "France 5 (Direct)", "France.tv", "");
    add("ftv:35be22fb-1569-43ff-857c-99bf81defa2e", "Franceinfo (Direct)", "France.tv", "");
    add("arte:LIVE", "ARTE (Direct)", "ARTE", "");
    return res;
}

ChannelList get_francetv_replays(const std::string& channel_slug) {
    ChannelList res;
    std::string slug = channel_slug.empty() ? "france-2" : channel_slug;
    std::string url = "https://api-front.yatta.francetv.fr/standard/publish/channels/" + slug + "/contents";
    http::Response r = http::get(url, {{"User-Agent", UA_IOS}}, 12);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("France.tv HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON France.tv invalide";
        return res;
    }

    json_t* arr = json_object_get(root, "result");
    if (json_is_array(arr)) {
        size_t i;
        json_t* el;
        json_array_foreach(arr, i, el) {
            std::string title = jstr(el, "title");
            if (title.empty()) title = jstr(el, "label");
            std::string sub = jstr(el, "headline_title");
            std::string img;
            json_t* imgs = json_object_get(el, "images");
            if (json_is_array(imgs) && json_array_size(imgs) > 0) {
                json_t* im0 = json_array_get(imgs, 0);
                json_t* urls = im0 ? json_object_get(im0, "urls") : nullptr;
                img = jstr(urls, "w:400");
                if (img.empty()) img = jstr(urls, "w:1024");
            }

            std::string si_id;
            json_t* medias = json_object_get(el, "content_has_medias");
            if (json_is_array(medias)) {
                size_t j;
                json_t* m;
                json_array_foreach(medias, j, m) {
                    json_t* med = json_object_get(m, "media");
                    std::string s = jstr(med, "si_id");
                    if (!s.empty()) {
                        si_id = s;
                        break;
                    }
                }
            }
            if (si_id.empty()) si_id = jstr(el, "si_id");
            if (title.empty() || si_id.empty()) continue;

            Channel ch;
            ch.id = "ftv:" + si_id;
            ch.name = sub.empty() ? title : (title + " - " + sub);
            ch.group = "Replay " + slug;
            ch.image_url = img;
            res.items.push_back(std::move(ch));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucun replay disponible";
    return res;
}

std::string iptv_custom_url() { return animesama::txt::trim(store::get_str(STORE_IPTV_URL, "")); }

void set_iptv_custom_url(const std::string& url) { store::set_str(STORE_IPTV_URL, animesama::txt::trim(url)); }

ChannelList get_iptv_channels(const std::string& playlist_key) {
    ChannelList res;
    std::string url;
    if (playlist_key == "custom") {
        url = iptv_custom_url();
        if (url.empty()) {
            res.error = "Configure d'abord une URL M3U personnalisee";
            return res;
        }
    } else if (playlist_key == "fra") {
        url = "https://iptv-org.github.io/iptv/languages/fra.m3u";
    } else if (playlist_key == "animation") {
        url = "https://iptv-org.github.io/iptv/categories/animation.m3u";
    } else if (playlist_key == "movies") {
        url = "https://iptv-org.github.io/iptv/categories/movies.m3u";
    } else if (playlist_key == "sports") {
        url = "https://iptv-org.github.io/iptv/categories/sports.m3u";
    } else if (playlist_key == "music") {
        url = "https://iptv-org.github.io/iptv/categories/music.m3u";
    } else {
        url = "https://iptv-org.github.io/iptv/countries/fr.m3u";
    }

    http::Response r = http::get(url, {{"User-Agent", UA_IOS}}, 15);
    if (!r.ok() || r.body.empty()) {
        res.error = r.error.empty() ? ("IPTV HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    std::istringstream iss(r.body);
    std::string line;
    std::string cur_name;
    while (std::getline(iss, line)) {
        line = animesama::txt::trim(line);
        if (line.empty()) continue;
        if (animesama::txt::starts_with(line, "#EXTINF:")) {
            size_t comma = line.rfind(',');
            cur_name = (comma != std::string::npos) ? animesama::txt::trim(line.substr(comma + 1)) : "Chaine IPTV";
        } else if (line[0] != '#' && animesama::txt::starts_with(line, "http")) {
            Channel ch;
            ch.id = line;
            ch.name = cur_name.empty() ? line : cur_name;
            ch.group = "IPTV";
            ch.url = line;
            ch.is_freebox = true;  // direct stream URL
            res.items.push_back(std::move(ch));
            cur_name.clear();
            if (res.items.size() >= 400) break;
        }
    }

    if (res.items.empty()) res.error = "Aucune chaine trouvee dans cette playlist IPTV";
    log_message(LOG_OK, "FreeTV", "Loaded %d IPTV channel(s) from %s", (int)res.items.size(), playlist_key.c_str());
    return res;
}

player::Source make_channel_source(const Channel& ch) {
    player::Source src;
    src.title = ch.name;
    src.subtitle = ch.group;
    src.artwork = ch.image_url;
    src.service = "freetv";
    src.id = ch.id;
    src.user_agent = UA_IOS;
    src.remember_position = false;
    src.live = ch.is_freebox || (ch.group == "France.tv" || ch.group == "ARTE");

    if (ch.is_freebox) {
        src.url = ch.url;
        src.http_only = false;
        src.chunked_http = false;
        return src;
    }

    std::string cid = ch.id;
    src.resolve = [cid](player::Source& s, std::string& err) -> bool {
        if (animesama::txt::starts_with(cid, "ftv:")) {
            std::string bid = cid.substr(4);
            std::string url;
            if (!resolve_francetv_id(bid, url, err)) return false;
            s.url = url;
            s.chunked_http = false;
            return true;
        }
        if (cid == "arte:LIVE") {
            std::string url;
            if (!resolve_arte_live(url, err)) return false;
            s.url = url;
            s.chunked_http = false;
            return true;
        }
        err = "Chaine inconnue";
        return false;
    };
    return src;
}

}  // namespace freetv
