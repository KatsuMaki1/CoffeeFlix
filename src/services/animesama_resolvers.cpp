#include "services/animesama.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "core/http.hpp"
#include "core/log.hpp"
#include "services/animesama_text.hpp"

namespace animesama {

namespace {

constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

http::Response http_get(const std::string& url, const std::string& referer = "", long timeout = 8) {
    http::Request req;
    req.url = url;
    req.timeout = timeout;
    req.headers.push_back({"User-Agent", UA});
    req.headers.push_back({"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
    req.headers.push_back({"Accept-Language", "fr-FR,fr;q=0.9,en-US;q=0.8,en;q=0.7"});
    if (!referer.empty()) req.headers.push_back({"Referer", referer});
    return http::perform(req);
}

bool ok_status(long st) { return st == 200 || st == 206; }

bool find_m3u8(const std::string& text, bool require_file_key, std::string& out) {
    size_t p = 0;
    while ((p = text.find(".m3u8", p)) != std::string::npos) {
        size_t q = p;
        while (q > 0 && text[q - 1] != '"' && text[q - 1] != '\'' && text[q - 1] != '\n' && p - q < 2048) q--;
        if (q == 0 || (text[q - 1] != '"' && text[q - 1] != '\'')) {
            p += 5;
            continue;
        }
        char quote = text[q - 1];
        size_t e = text.find(quote, p);
        if (e == std::string::npos) break;
        std::string url = text.substr(q, e - q);
        if (!txt::starts_with(url, "http://") && !txt::starts_with(url, "https://")) {
            p = e;
            continue;
        }
        if (require_file_key) {
            size_t k = q - 1;
            while (k > 0 && std::isspace((unsigned char)text[k - 1])) k--;
            if (k == 0 || text[k - 1] != ':') {
                p = e;
                continue;
            }
            k--;
            while (k > 0 && std::isspace((unsigned char)text[k - 1])) k--;
            if (k < 4 || text.compare(k - 4, 4, "file") != 0) {
                p = e;
                continue;
            }
        }
        out = txt::trim(url);
        return true;
    }
    return false;
}

const char* BASE62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

long base_n_value(const std::string& tok, int base) {
    long v = 0;
    for (char c : tok) {
        const char* pos = strchr(BASE62, c);
        if (!pos || !c) return -1;
        long d = pos - BASE62;
        if (d >= base) return -1;
        v = v * base + d;
        if (v > 1000000) return -1;
    }
    return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    while (true) {
        size_t e = s.find(sep, b);
        out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

bool resolve_vidmoly(const std::string& url, Stream& out) {
    std::string url_net = url;
    txt::replace_all(url_net, "vidmoly.to", "vidmoly.net");
    http::Response r = http_get(url_net, url_net);
    if (!ok_status(r.status)) return false;
    std::string redirect;
    if (txt::between(r.body, "window.location.replace('", "')", redirect)) {
        r = http_get(redirect, url_net);
        if (!ok_status(r.status)) return false;
    }
    std::string m3u8;
    if (find_m3u8(r.body, true, m3u8)) {
        out = {m3u8, "m3u8", txt::url_origin(url_net) + "/"};
        return true;
    }
    return false;
}

bool resolve_smoothpre(const std::string& url, Stream& out) {
    std::string base = txt::url_origin(url);
    http::Response r = http_get(url, base + "/");
    if (!ok_status(r.status)) return false;
    std::string decoded = unpack_packer(r.body);
    if (decoded.empty()) decoded = r.body;
    for (const char* key : {"hls4", "hls3", "hls2"}) {
        std::string needle = std::string("\"") + key + "\"";
        size_t p = decoded.find(needle);
        if (p == std::string::npos) continue;
        p = decoded.find(':', p + needle.size());
        if (p == std::string::npos) continue;
        size_t q1 = decoded.find('"', p);
        if (q1 == std::string::npos) continue;
        size_t q2 = decoded.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string target = decoded.substr(q1 + 1, q2 - q1 - 1);
        txt::replace_all(target, "\\", "");
        if (target.empty()) continue;
        if (target[0] == '/') target = base + target;
        out = {target, "m3u8", base + "/"};
        return true;
    }
    return false;
}

bool resolve_sendvid(const std::string& url, Stream& out) {
    http::Response r = http_get(url, "https://sendvid.com/");
    if (!ok_status(r.status)) return false;
    std::string video, tag;
    size_t from = 0, tag_end = 0;
    while (txt::find_tag(r.body, "<source", from, tag, &tag_end)) {
        std::string src = txt::attr(tag, "src");
        if (src.find(".mp4") != std::string::npos) {
            video = src;
            break;
        }
        from = tag_end;
    }
    if (video.empty() && txt::find_tag(r.body, "property=\"og:video\"", 0, tag)) video = txt::attr(tag, "content");
    if (video.empty() && txt::find_tag(r.body, "property=\"og:video:url\"", 0, tag))
        video = txt::attr(tag, "content");
    if (video.empty()) return false;
    if (txt::starts_with(video, "//")) video = "https:" + video;
    out = {video, "mp4", "https://sendvid.com/"};
    return true;
}

bool resolve_generic_m3u8(const std::string& url, Stream& out, bool embed_fallback) {
    http::Response r = http_get(url, url);
    if (ok_status(r.status)) {
        std::string m3u8;
        if (find_m3u8(r.body, true, m3u8) || find_m3u8(r.body, false, m3u8)) {
            out = {m3u8, "m3u8", txt::url_origin(url) + "/"};
            return true;
        }
    }
    if (embed_fallback) {
        out = {url, "embed", ""};
        return true;
    }
    return false;
}

bool resolve_sibnet(const std::string& url, Stream& out) {
    http::Response r = http_get(url, url);
    if (!ok_status(r.status)) return false;
    size_t p = r.body.find("player.src");
    if (p == std::string::npos) return false;
    std::string path;
    if (!txt::between(r.body, "src: \"", "\"", path, p) && !txt::between(r.body, "src:\"", "\"", path, p)) return false;
    if (path.find(".mp4") == std::string::npos) return false;
    out = {txt::url_resolve("https://video.sibnet.ru/", path), "mp4", url};
    return true;
}

}  // namespace

std::string unpack_packer(const std::string& html) {
    size_t p = html.find("eval(function(p,a,c,k,e,d)");
    if (p == std::string::npos) return "";
    size_t start = html.find("}('", p);
    if (start == std::string::npos) return "";
    start += 3;

    size_t q = start, payload_end = std::string::npos, a_pos = 0;
    while ((q = html.find("',", q)) != std::string::npos) {
        size_t d = q + 2, d0 = d;
        while (d < html.size() && std::isdigit((unsigned char)html[d])) d++;
        if (d > d0 && d < html.size() && html[d] == ',') {
            size_t d1 = ++d;
            while (d < html.size() && std::isdigit((unsigned char)html[d])) d++;
            if (d > d1 && d + 1 < html.size() && html[d] == ',' && html[d + 1] == '\'') {
                payload_end = q;
                a_pos = d0;
                break;
            }
        }
        q += 2;
    }
    if (payload_end == std::string::npos) return "";

    std::string payload = html.substr(start, payload_end - start);
    int a = atoi(html.c_str() + a_pos);
    size_t comma = html.find(',', a_pos);
    int c = atoi(html.c_str() + comma + 1);
    size_t k_start = html.find(",'", comma + 1);
    if (k_start == std::string::npos) return "";
    k_start += 2;
    size_t k_end = html.find("'.split('|')", k_start);
    if (k_end == std::string::npos) return "";
    std::vector<std::string> keys = split(html.substr(k_start, k_end - k_start), '|');

    if (a < 2 || a > 62) return "";

    std::string p_str;
    p_str.reserve(payload.size());
    for (size_t i = 0; i < payload.size(); i++) {
        if (payload[i] == '\\' && i + 1 < payload.size()) p_str += payload[++i];
        else p_str += payload[i];
    }

    std::string out;
    out.reserve(p_str.size() * 2);
    size_t i = 0;
    while (i < p_str.size()) {
        unsigned char ch = (unsigned char)p_str[i];
        if (std::isalnum(ch) || ch == '_') {
            size_t j = i;
            while (j < p_str.size() && (std::isalnum((unsigned char)p_str[j]) || p_str[j] == '_')) j++;
            std::string tok = p_str.substr(i, j - i);
            long v = base_n_value(tok, a);
            if (v >= 0 && v < c && (size_t)v < keys.size() && !keys[v].empty()) out += keys[v];
            else out += tok;
            i = j;
        } else {
            out += (char)ch;
            i++;
        }
    }
    return out;
}

bool resolve_video_url(const std::string& raw_url, Stream& out) {
    std::string url = txt::trim(raw_url);
    if (url.empty()) return false;
    std::string low = txt::lower(url);
    size_t qpos = low.find_first_of("?#");
    std::string path = qpos == std::string::npos ? low : low.substr(0, qpos);
    if (txt::ends_with(path, ".m3u8")) {
        out = {url, "m3u8", ""};
        return true;
    }
    if (txt::ends_with(path, ".mp4")) {
        out = {url, "mp4", ""};
        return true;
    }

    std::string host = txt::url_host(url);
    if (host == "vidmoly.to" || host == "vidmoly.net" || host == "vidmoly.me") return resolve_vidmoly(url, out);
    if (host == "smoothpre.com" || host == "vidhide.com" || host == "vidhidepro.com" || host == "streamwish.com" ||
        host == "streamwish.to")
        return resolve_smoothpre(url, out);
    if (host == "sendvid.com") return resolve_sendvid(url, out);
    if (host == "ansembed.net" || host == "ansembed.com") return resolve_generic_m3u8(url, out, false);
    if (host == "embed4me.com" || host == "lpayer.embed4me.com" || host == "player.embed4me.com")
        return resolve_generic_m3u8(url, out, true);
    if (host == "video.sibnet.ru") return resolve_sibnet(url, out);

    log_message(LOG_WARNING, "AnimeSama", "No resolver for host %s", host.c_str());
    out = {url, "raw", ""};
    return true;
}

int check_stream_health(const Stream& st) {
    if (st.url.empty()) return 999;
    std::string referer = st.referer.empty() ? txt::url_origin(st.url) + "/" : st.referer;

    if (st.type == "m3u8" || st.url.find(".m3u8") != std::string::npos) {
        http::Response r = http_get(st.url, referer, 6);
        if (!r.error.empty()) return 1;
        if (r.status == 404 || r.status == 410 || r.status == 403 || r.status == 502 || r.status == 503) return 999;
        auto ct = r.headers.find("content-type");
        std::string head = r.body.substr(0, 300);
        std::string head_low = txt::lower(head);
        if ((ct != r.headers.end() && txt::contains_ci(ct->second, "html")) ||
            head_low.find("<html") != std::string::npos) {
            for (const char* e : {"file deleted", "not found", "404", "access denied", "blocked"})
                if (head_low.find(e) != std::string::npos) return 999;
        }
        if (head.find("#EXTM3U") != std::string::npos || head.find("#EXT-X-") != std::string::npos) return 0;
        return ok_status(r.status) ? 1 : 999;
    }

    if (st.type == "mp4" || st.url.find(".mp4") != std::string::npos) {
        http::Request req;
        req.url = st.url;
        req.timeout = 6;
        req.headers.push_back({"User-Agent", UA});
        req.headers.push_back({"Referer", referer});
        req.headers.push_back({"Range", "bytes=0-1024"});
        size_t received = 0;
        req.on_data = [&](const http::Response&, const char*, size_t n) {
            received += n;
            return received < 1024;
        };
        http::Response r = http::perform(req);
        if (r.status == 404 || r.status == 410 || r.status == 403 || r.status == 502 || r.status == 503) return 999;
        auto ct = r.headers.find("content-type");
        if (ct != r.headers.end() && txt::contains_ci(ct->second, "html")) return 999;
        if (ok_status(r.status)) return 0;
        return r.error.empty() ? 999 : 1;
    }

    return 1;
}

}  // namespace animesama
