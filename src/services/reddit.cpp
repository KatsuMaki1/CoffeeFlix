#include "services/reddit.hpp"

#include <jansson.h>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace reddit {

namespace {

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

}  // namespace

PostList fetch_subreddit(const std::string& subreddit) {
    PostList res;
    std::string sub = animesama::txt::trim(subreddit);
    if (animesama::txt::starts_with(animesama::txt::lower(sub), "r/")) sub = sub.substr(2);
    if (sub.empty()) sub = "memes";

    std::string url = "https://meme-api.com/gimme/" + animesama::txt::url_encode(sub) + "/24";
    http::Response r = http::get(url, {{"User-Agent", "CoffeeFlix/1.0"}}, 12);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("Reddit HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON Reddit invalide";
        return res;
    }

    std::string msg = jstr(root, "message");
    if (!msg.empty()) {
        res.error = msg;
        json_decref(root);
        return res;
    }

    json_t* arr = json_object_get(root, "memes");
    if (json_is_array(arr)) {
        size_t i;
        json_t* el;
        json_array_foreach(arr, i, el) {
            if (json_is_true(json_object_get(el, "nsfw"))) continue;
            Post p;
            p.title = jstr(el, "title");
            p.author = jstr(el, "author");
            p.subreddit = jstr(el, "subreddit");
            p.image_url = jstr(el, "url");
            p.ups = (int)json_integer_value(json_object_get(el, "ups"));

            json_t* prev = json_object_get(el, "preview");
            if (json_is_array(prev) && json_array_size(prev) > 0) {
                size_t idx = json_array_size(prev) > 2 ? 2 : json_array_size(prev) - 1;
                json_t* pv = json_array_get(prev, idx);
                if (json_is_string(pv)) p.thumb_url = json_string_value(pv);
            }
            if (p.thumb_url.empty()) p.thumb_url = p.image_url;
            if (!p.image_url.empty() && !p.title.empty()) res.items.push_back(std::move(p));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucun post trouve sur r/" + sub;
    log_message(LOG_OK, "Reddit", "Loaded %d post(s) from r/%s", (int)res.items.size(), sub.c_str());
    return res;
}

}  // namespace reddit

