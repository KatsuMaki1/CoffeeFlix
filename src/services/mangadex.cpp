#include "services/mangadex.hpp"

#include <jansson.h>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama_text.hpp"

namespace mangadex {

namespace {

constexpr const char* API = "https://api.mangadex.org";
constexpr const char* UA = "CoffeeFlix/1.0 (WiiU)";

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string pick_localized(json_t* obj) {
    if (!obj || !json_is_object(obj)) return "";
    for (const char* k : {"fr", "en", "ja-ro", "ja"}) {
        std::string v = jstr(obj, k);
        if (!v.empty()) return v;
    }
    const char* key;
    json_t* val;
    json_object_foreach(obj, key, val) {
        if (json_is_string(val)) return json_string_value(val);
    }
    return "";
}

MangaList fetch_manga_list(const std::string& url) {
    MangaList res;
    http::Response r = http::get(url, {{"User-Agent", UA}}, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("MangaDex HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON MangaDex invalide";
        return res;
    }

    json_t* data = json_object_get(root, "data");
    if (json_is_array(data)) {
        size_t i;
        json_t* el;
        json_array_foreach(data, i, el) {
            std::string id = jstr(el, "id");
            json_t* attrs = json_object_get(el, "attributes");
            std::string title = pick_localized(attrs ? json_object_get(attrs, "title") : nullptr);
            std::string desc = pick_localized(attrs ? json_object_get(attrs, "description") : nullptr);
            if (id.empty() || title.empty()) continue;

            std::string cover_file;
            json_t* rels = json_object_get(el, "relationships");
            if (json_is_array(rels)) {
                size_t j;
                json_t* rel;
                json_array_foreach(rels, j, rel) {
                    if (jstr(rel, "type") == "cover_art") {
                        json_t* ra = json_object_get(rel, "attributes");
                        cover_file = jstr(ra, "fileName");
                        if (!cover_file.empty()) break;
                    }
                }
            }

            Manga m;
            m.id = id;
            m.title = title;
            m.description = desc;
            if (!cover_file.empty()) {
                m.cover_url = "https://uploads.mangadex.org/covers/" + id + "/" + cover_file + ".512.jpg";
            }
            res.items.push_back(std::move(m));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucun manga trouve";
    return res;
}

}  // namespace

MangaList browse_popular() {
    return fetch_manga_list(
        std::string(API) +
        "/manga?limit=24&includes[]=cover_art&order[followedCount]=desc&contentRating[]=safe&contentRating[]=suggestive");
}

MangaList search(const std::string& query) {
    std::string q = animesama::txt::trim(query);
    if (q.empty()) return browse_popular();
    return fetch_manga_list(
        std::string(API) + "/manga?title=" + animesama::txt::url_encode(q) +
        "&limit=24&includes[]=cover_art&order[relevance]=desc&contentRating[]=safe&contentRating[]=suggestive");
}

ChapterList get_chapters(const std::string& manga_id) {
    ChapterList res;
    std::string url = std::string(API) + "/manga/" + manga_id +
                      "/feed?translatedLanguage[]=fr&translatedLanguage[]=en&order[chapter]=asc&limit=96&contentRating[]=safe&contentRating[]=suggestive";
    http::Response r = http::get(url, {{"User-Agent", UA}}, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("MangaDex HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON MangaDex invalide";
        return res;
    }

    json_t* data = json_object_get(root, "data");
    if (json_is_array(data)) {
        size_t i;
        json_t* el;
        json_array_foreach(data, i, el) {
            std::string cid = jstr(el, "id");
            json_t* attrs = json_object_get(el, "attributes");
            if (!attrs || cid.empty()) continue;
            int pages = (int)json_integer_value(json_object_get(attrs, "pages"));
            if (pages <= 0) continue;

            Chapter ch;
            ch.id = cid;
            ch.number = jstr(attrs, "chapter");
            ch.title = jstr(attrs, "title");
            ch.lang = jstr(attrs, "translatedLanguage");
            res.items.push_back(std::move(ch));
        }
    }
    json_decref(root);
    if (res.items.empty()) res.error = "Aucun chapitre disponible en FR/EN";
    return res;
}

PageList get_pages(const std::string& chapter_id) {
    PageList res;
    std::string url = std::string(API) + "/at-home/server/" + chapter_id;
    http::Response r = http::get(url, {{"User-Agent", UA}}, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("MangaDex HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    json_error_t jerr;
    json_t* root = json_loads(r.body.c_str(), 0, &jerr);
    if (!root) {
        res.error = "Reponse JSON MangaDex invalide";
        return res;
    }

    std::string base = jstr(root, "baseUrl");
    json_t* ch = json_object_get(root, "chapter");
    std::string hash = jstr(ch, "hash");
    json_t* data = ch ? json_object_get(ch, "dataSaver") : nullptr;
    const char* mode = "data-saver";
    if (!json_is_array(data) || json_array_size(data) == 0) {
        data = ch ? json_object_get(ch, "data") : nullptr;
        mode = "data";
    }

    if (!base.empty() && !hash.empty() && json_is_array(data)) {
        size_t i;
        json_t* fn;
        json_array_foreach(data, i, fn) {
            if (json_is_string(fn)) {
                res.urls.push_back(base + "/" + mode + "/" + hash + "/" + json_string_value(fn));
            }
        }
    }
    json_decref(root);
    if (res.urls.empty()) res.error = "Aucune page trouvee pour ce chapitre";
    return res;
}

}  // namespace mangadex

