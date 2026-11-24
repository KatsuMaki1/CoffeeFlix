#include "services/streams_dl.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "core/http.hpp"
#include "logger/logger.hpp"
#include "services/animesama.hpp"
#include "services/animesama_text.hpp"

namespace streams_dl {

namespace {

constexpr const char* BASE_URL = "https://www.french-streaming.tv";
constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

http::Response fetch_html(const std::string& url, const std::string& referer = "", long timeout = 15) {
    http::Request req;
    req.url = url;
    req.timeout = timeout;
    req.headers.push_back({"User-Agent", UA});
    req.headers.push_back({"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
    req.headers.push_back({"Accept-Language", "fr-FR,fr;q=0.9,en-US;q=0.8,en;q=0.7"});
    if (!referer.empty()) req.headers.push_back({"Referer", referer});
    return http::perform(req);
}

std::vector<Media> parse_cards(const std::string& html) {
    std::vector<Media> out;
    std::set<std::string> seen;

    // Focus on main content if present, otherwise scan whole document
    size_t start = html.find("id=\"dle-content\"");
    if (start == std::string::npos) start = 0;

    size_t pos = start;
    while (pos < html.size()) {
        std::string tag;
        size_t tag_end = 0;
        if (!animesama::txt::find_tag(html, "short-poster", pos, tag, &tag_end)) break;
        pos = tag_end;

        std::string href = animesama::txt::attr(tag, "href");
        if (href.empty()) continue;
        std::string full_url = animesama::txt::url_resolve(BASE_URL, href);
        if (!seen.insert(full_url).second) continue;

        size_t close_a = html.find("</a>", tag_end);
        if (close_a == std::string::npos) close_a = std::min(html.size(), tag_end + 1500);
        std::string block = html.substr(tag_end, close_a - tag_end);

        std::string img_url;
        std::string img_tag;
        if (animesama::txt::find_tag(block, "<img", 0, img_tag)) {
            img_url = animesama::txt::attr(img_tag, "src");
            if (img_url.empty()) img_url = animesama::txt::attr(img_tag, "data-src");
            if (!img_url.empty()) img_url = animesama::txt::url_resolve(BASE_URL, img_url);
        }

        std::string title;
        size_t tpos = block.find("short-title");
        if (tpos != std::string::npos) {
            size_t gt = block.find('>', tpos);
            if (gt != std::string::npos) {
                title = animesama::txt::inner_text(block, gt + 1, "div");
            }
        }
        if (title.empty()) title = animesama::txt::attr(tag, "alt");
        title = animesama::txt::collapse_ws(animesama::txt::html_decode(title));
        if (title.empty()) continue;

        Media m;
        m.title = std::move(title);
        m.url = std::move(full_url);
        m.image_url = std::move(img_url);
        out.push_back(std::move(m));
    }
    return out;
}

bool is_supported_host(const std::string& url) {
    std::string low = animesama::txt::lower(url);
    return low.find("vidzy.") != std::string::npos || low.find("uqload.") != std::string::npos ||
           low.find("fsvid.") != std::string::npos || low.find("vidmoly.") != std::string::npos ||
           low.find("sibnet.") != std::string::npos || low.find("sendvid.") != std::string::npos ||
           low.find("luluvdo.") != std::string::npos;
}

std::string host_label(const std::string& url) {
    std::string host = animesama::txt::url_host(url);
    if (host.empty()) return "Lecteur";
    return host;
}

bool find_stream_in_html(const std::string& html, std::string& stream_url, bool& is_mp4) {
    // Scan for .m3u8 first, then .mp4 (skipping known decoy URLs like troll/master.m3u8)
    const char* exts[] = {".m3u8", ".mp4"};
    for (int pass = 0; pass < 2; pass++) {
        const char* ext = exts[pass];
        size_t p = 0;
        while ((p = html.find(ext, p)) != std::string::npos) {
            size_t q = p;
            while (q > 0 && html[q - 1] != '"' && html[q - 1] != '\'' && html[q - 1] != '\n' && p - q < 1024) q--;
            if (q == 0 || (html[q - 1] != '"' && html[q - 1] != '\'')) {
                p += 4;
                continue;
            }
            char quote = html[q - 1];
            size_t e = html.find(quote, p);
            if (e == std::string::npos) {
                p += 4;
                continue;
            }
            std::string u = html.substr(q, e - q);
            std::string low = animesama::txt::lower(u);
            if ((animesama::txt::starts_with(low, "http://") || animesama::txt::starts_with(low, "https://")) &&
                low.find("troll/") == std::string::npos && low.find("sample") == std::string::npos) {
                stream_url = u;
                is_mp4 = (pass == 1);
                return true;
            }
            p = e;
        }
    }
    return false;
}

bool resolve_embed(const std::string& embed_url, const std::string& referer, std::string& stream_url, bool& is_mp4,
                   std::string& err) {
    std::string low = animesama::txt::lower(embed_url);
    if (low.find(".m3u8") != std::string::npos) {
        stream_url = embed_url;
        is_mp4 = false;
        return true;
    }
    if (low.find(".mp4") != std::string::npos) {
        stream_url = embed_url;
        is_mp4 = true;
        return true;
    }

    http::Response r = fetch_html(embed_url, referer, 15);
    if (!r.ok() || r.body.empty()) {
        err = r.error.empty() ? ("Embed HTTP " + std::to_string(r.status)) : r.error;
        return false;
    }

    if (find_stream_in_html(r.body, stream_url, is_mp4)) {
        return true;
    }

    // Try Dean Edwards packer decoding (used by uqload.vc, smoothpre, etc.)
    std::string unpacked = animesama::unpack_packer(r.body);
    if (!unpacked.empty() && find_stream_in_html(unpacked, stream_url, is_mp4)) {
        return true;
    }

    // Fallback to Anime-Sama host resolvers (vidmoly, sibnet, sendvid, etc.)
    animesama::Stream st;
    if (animesama::resolve_video_url(embed_url, st) && !st.url.empty() &&
        (st.type == "m3u8" || st.type == "mp4")) {
        stream_url = st.url;
        is_mp4 = (st.type == "mp4");
        return true;
    }

    // Check if there is a nested iframe inside the embed wrapper
    size_t pos = 0;
    std::string tag;
    while (animesama::txt::find_tag(r.body, "<iframe", pos, tag, &pos)) {
        std::string src = animesama::txt::attr(tag, "src");
        if (!src.empty() && animesama::txt::starts_with(src, "http")) {
            http::Response r2 = fetch_html(src, embed_url, 15);
            if (r2.ok() && find_stream_in_html(r2.body, stream_url, is_mp4)) return true;
        }
    }

    err = "Flux video introuvable sur cet hebergeur";
    return false;
}

}  // namespace

SearchResults browse(const std::string& category) {
    SearchResults res;
    std::string path = "/";
    if (category == "Films") path = "/film/";
    else if (category == "Series") path = "/serie/";

    http::Response r = fetch_html(std::string(BASE_URL) + path, BASE_URL, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }
    res.items = parse_cards(r.body);
    if (res.items.empty()) res.error = "Aucun contenu trouve sur le site";
    log_message(LOG_OK, "StreamsDL", "browse(%s) -> %d item(s)", category.c_str(), (int)res.items.size());
    return res;
}

SearchResults search(const std::string& query, const std::string& category) {
    SearchResults res;
    std::string q = animesama::txt::trim(query);
    if (q.empty()) return browse(category);

    std::string url = std::string(BASE_URL) + "/search/" + animesama::txt::url_encode(q) + "/";
    http::Response r = fetch_html(url, BASE_URL, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    res.items = parse_cards(r.body);
    log_message(LOG_OK, "StreamsDL", "search('%s') -> %d result(s)", q.c_str(), (int)res.items.size());
    return res;
}

VideoResults get_videos(const std::string& media_url, const std::string& /*category*/) {
    VideoResults res;
    http::Response r = fetch_html(media_url, BASE_URL, 15);
    if (!r.ok()) {
        res.error = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return res;
    }

    std::set<std::string> seen;

    // Scan attributes (data-url, data-url-default, data-src, src, href) for supported video hosts
    const char* needles[] = {"data-url=\"", "data-url-default=\"", "data-src=\"", "src=\"", "href=\""};
    for (const char* needle : needles) {
        size_t pos = 0;
        size_t nlen = std::char_traits<char>::length(needle);
        while ((pos = r.body.find(needle, pos)) != std::string::npos) {
            pos += nlen;
            size_t end = r.body.find('"', pos);
            if (end == std::string::npos) break;
            std::string u = animesama::txt::trim(r.body.substr(pos, end - pos));
            pos = end + 1;

            if (!animesama::txt::starts_with(u, "http")) continue;
            if (!is_supported_host(u)) continue;
            if (!seen.insert(u).second) continue;

            Video v;
            v.title = "Lecteur (" + host_label(u) + ")";
            v.url = u;
            v.html_url = media_url;
            res.items.push_back(std::move(v));
        }
    }

    // Prioritize vidzy and uqload at the top of the list
    std::stable_sort(res.items.begin(), res.items.end(), [](const Video& a, const Video& b) {
        auto rank = [](const std::string& u) {
            std::string low = animesama::txt::lower(u);
            if (low.find("vidzy.") != std::string::npos) return 0;
            if (low.find("uqload.") != std::string::npos) return 1;
            return 2;
        };
        return rank(a.url) < rank(b.url);
    });
    for (size_t i = 0; i < res.items.size(); i++) {
        res.items[i].title = "Lecteur " + std::to_string(i + 1) + " (" + host_label(res.items[i].url) + ")";
    }

    if (res.items.empty()) {
        res.error = "Aucun lecteur compatible trouve sur cette page";
    }
    log_message(LOG_OK, "StreamsDL", "get_videos(%s) -> %d link(s)", media_url.c_str(), (int)res.items.size());
    return res;
}

player::Source make_source(const std::string& media_title, const std::string& media_cover, const Video& v) {
    player::Source src;
    src.title = v.title.empty() ? media_title : (media_title + " - " + v.title);
    src.subtitle = v.resolution.empty() ? host_label(v.url) : v.resolution;
    src.artwork = v.image_url.empty() ? media_cover : v.image_url;
    src.service = "streamsdl";
    src.id = v.url;
    src.user_agent = UA;
    src.chunked_http = false;
    src.resolve = [embed = v.url, ref = v.html_url](player::Source& s, std::string& err) -> bool {
        std::string stream_url;
        bool is_mp4 = false;
        if (!resolve_embed(embed, ref, stream_url, is_mp4, err)) return false;
        s.url = stream_url;
        s.chunked_http = is_mp4;
        s.headers.clear();
        s.headers.push_back({"Referer", animesama::txt::url_origin(embed) + "/"});
        return true;
    };
    return src;
}

}  // namespace streams_dl
