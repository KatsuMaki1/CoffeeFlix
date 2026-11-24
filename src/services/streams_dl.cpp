#include "services/streams_dl.hpp"

#include <jansson.h>

#include <cstdlib>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace streams_dl {

namespace {

constexpr const char* STORE_URL_KEY = "streams_dl_url";
constexpr const char* DEFAULT_URL = "http://127.0.0.1:8000";

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string normalize_base(std::string u) {
    u = animesama::txt::trim(u);
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (!u.empty() && !animesama::txt::starts_with(u, "http://") && !animesama::txt::starts_with(u, "https://"))
        u = "http://" + u;
    return u;
}

}  // namespace

std::string server_url() {
    std::string u = store::get_str(STORE_URL_KEY, DEFAULT_URL);
    return u.empty() ? std::string(DEFAULT_URL) : normalize_base(u);
}

void set_server_url(const std::string& url) { store::set_str(STORE_URL_KEY, normalize_base(url)); }

std::vector<std::string> get_providers(std::string& err) {
    std::vector<std::string> out = {"french-stream", "flemmix", "papadustream"};
    http::Response r = http::get(server_url() + "/providers", {}, 5);
    if (!r.ok()) {
        err = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return out;
    }
    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) return out;
    json_t* arr = json_object_get(root, "providers");
    if (json_is_array(arr) && json_array_size(arr) > 0) {
        out.clear();
        size_t i;
        json_t* el;
        json_array_foreach(arr, i, el) {
            if (json_is_string(el)) out.push_back(json_string_value(el));
        }
    }
    json_decref(root);
    return out;
}

SearchResults search(const std::string& query, const std::string& provider) {
    SearchResults res;
    std::string q = animesama::txt::trim(query);
    if (q.empty()) {
        res.error = "Recherche vide";
        return res;
    }

    std::string url = server_url() + "/search?query=" + animesama::txt::url_encode(q) +
                      "&provider_name=" + animesama::txt::url_encode(provider);
    http::Response r = http::get(url, {}, 30);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON invalide";
        return res;
    }

    std::string api_err = jstr(root, "error");
    if (!api_err.empty()) {
        res.error = api_err;
        json_decref(root);
        return res;
    }

    json_t* arr = json_object_get(root, "results");
    if (json_is_array(arr)) {
        size_t i;
        json_t* el;
        json_array_foreach(arr, i, el) {
            Media m;
            m.title = jstr(el, "title");
            m.url = jstr(el, "url");
            m.image_url = jstr(el, "image_url");
            if (!m.title.empty() && !m.url.empty()) res.items.push_back(std::move(m));
        }
    }
    json_decref(root);
    log_message(LOG_OK, "StreamsDL", "search('%s', %s) -> %d result(s)", q.c_str(), provider.c_str(),
                (int)res.items.size());
    return res;
}

VideoResults get_videos(const std::string& media_url, const std::string& provider) {
    VideoResults res;
    json_t* body = json_object();
    json_object_set_new(body, "media_url", json_string(media_url.c_str()));
    json_object_set_new(body, "provider_name", json_string(provider.c_str()));
    char* dumped = json_dumps(body, JSON_COMPACT);
    std::string payload = dumped ? dumped : "{}";
    if (dumped) free(dumped);
    json_decref(body);

    http::Response r = http::post_json(server_url() + "/get-videos", payload, {}, 45);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON invalide";
        return res;
    }

    std::string api_err = jstr(root, "error");
    if (!api_err.empty()) {
        res.error = api_err;
        json_decref(root);
        return res;
    }

    json_t* arr = json_object_get(root, "results");
    if (json_is_array(arr)) {
        size_t i;
        json_t* el;
        json_array_foreach(arr, i, el) {
            Video v;
            v.title = jstr(el, "title");
            v.url = jstr(el, "url");
            v.html_url = jstr(el, "html_url");
            v.image_url = jstr(el, "image_url");
            v.duration = jstr(el, "duration");
            v.resolution = jstr(el, "resolution");
            if (!v.url.empty()) res.items.push_back(std::move(v));
        }
    }
    json_decref(root);
    log_message(LOG_OK, "StreamsDL", "get_videos(%s) -> %d video(s)", media_url.c_str(), (int)res.items.size());
    return res;
}

player::Source make_source(const std::string& media_title, const std::string& media_cover, const Video& v) {
    player::Source src;
    src.title = v.title.empty() ? media_title : (media_title + " - " + v.title);
    src.subtitle = v.resolution;
    src.artwork = v.image_url.empty() ? media_cover : v.image_url;
    src.url = v.url;
    src.service = "streamsdl";
    src.id = v.url;
    src.chunked_http = true;
    src.user_agent = http::user_agent();
    if (!v.html_url.empty()) src.headers.push_back({"Referer", v.html_url});
    return src;
}

}  // namespace streams_dl

