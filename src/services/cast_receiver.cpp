#include "services/cast_receiver.hpp"

#include <arpa/inet.h>
#include <jansson.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/http.hpp"
#include "core/store.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "services/animesama_text.hpp"

namespace cast_receiver {

namespace {

constexpr const char* KEY_SCREEN_ID = "cast_yt_screen_id";
constexpr const char* KEY_LOUNGE_TOK = "cast_yt_lounge_token";
constexpr const char* KEY_UUID = "cast_device_uuid";
constexpr const char* UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

std::mutex g_mtx;
std::string g_pairing_code;
std::string g_connected_device;
std::deque<Command> g_queue;
LiveState g_live_state;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
int g_http_fd = -1;
int g_ssdp_fd = -1;

std::thread g_http_thread;
std::thread g_ssdp_thread;
std::thread g_lounge_thread;

void push_cmd(Command cmd) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.push_back(std::move(cmd));
}

std::string get_device_uuid() {
    std::string u = store::get_str(KEY_UUID, "");
    if (!u.empty()) return u;
    u = "coffeeflix-wiiu-2026-cast-0001";
    store::set_str(KEY_UUID, u);
    return u;
}

std::string jstr(json_t* obj, const char* key) {
    if (!obj || !json_is_object(obj)) return "";
    json_t* v = json_object_get(obj, key);
    return json_is_string(v) ? json_string_value(v) : "";
}

std::string format_code(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); i++) {
        if (i > 0 && i % 3 == 0) out += '-';
        out += raw[i];
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out += (char)std::strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string form_param(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return url_decode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return "";
}

std::string extract_yt_id(const std::string& input) {
    std::string s = animesama::txt::trim(input);
    if (s.size() == 11 && s.find('/') == std::string::npos && s.find('.') == std::string::npos) return s;
    size_t vp = s.find("v=");
    if (vp != std::string::npos) {
        std::string id = s.substr(vp + 2, 11);
        if (id.size() == 11) return id;
    }
    for (const char* pfx : {"youtu.be/", "/shorts/", "/embed/", "/live/"}) {
        size_t p = s.find(pfx);
        if (p != std::string::npos) {
            p += std::strlen(pfx);
            std::string id = s.substr(p, 11);
            if (id.size() == 11) return id;
        }
    }
    return "";
}

bool ensure_lounge_credentials(std::string& sid, std::string& ltok, bool force_new = false) {
    sid = store::get_str(KEY_SCREEN_ID, "");
    ltok = store::get_str(KEY_LOUNGE_TOK, "");

    if (force_new || sid.empty()) {
        http::Response r = http::get("https://www.youtube.com/api/lounge/pairing/generate_screen_id",
                                     {{"User-Agent", UA}}, 10);
        if (!r.ok() || r.body.empty()) return false;
        sid = animesama::txt::trim(r.body);
        store::set_str(KEY_SCREEN_ID, sid);
        ltok.clear();
    }

    if (force_new || ltok.empty()) {
        http::Request req;
        req.method = "POST";
        req.url = "https://www.youtube.com/api/lounge/pairing/get_lounge_token_batch";
        req.timeout = 10;
        req.headers.push_back({"User-Agent", UA});
        req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        req.body = "screen_ids=" + animesama::txt::url_encode(sid);
        http::Response r = http::perform(req);
        if (!r.ok()) return false;

        json_error_t jerr;
        json_t* root = json_loads(r.body.c_str(), 0, &jerr);
        json_t* screens = root ? json_object_get(root, "screens") : nullptr;
        if (json_is_array(screens) && json_array_size(screens) > 0) {
            ltok = jstr(json_array_get(screens, 0), "loungeToken");
        }
        if (root) json_decref(root);
        if (ltok.empty()) return false;
        store::set_str(KEY_LOUNGE_TOK, ltok);
    }

    // Fetch pairing code
    http::Request preq;
    preq.method = "POST";
    preq.url = "https://www.youtube.com/api/lounge/pairing/get_pairing_code?ctx=pair";
    preq.timeout = 10;
    preq.headers.push_back({"User-Agent", UA});
    preq.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    preq.body = "access_type=permanent&app=yt-tv&lounge_token=" + animesama::txt::url_encode(ltok) +
                "&screen_id=" + animesama::txt::url_encode(sid) +
                "&screen_name=" + animesama::txt::url_encode("CoffeeFlix (Wii U)");
    http::Response pr = http::perform(preq);
    if (pr.ok() && !pr.body.empty()) {
        std::string code = format_code(animesama::txt::trim(pr.body));
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pairing_code = code;
        log_message(LOG_OK, "Cast", "YouTube TV pairing code: %s", code.c_str());
    }
    return true;
}

void process_lounge_chunk(const std::string& json_chunk) {
    json_error_t jerr;
    json_t* root = json_loads(json_chunk.c_str(), 0, &jerr);
    if (!root || !json_is_array(root)) {
        if (root) json_decref(root);
        return;
    }

    size_t i;
    json_t* item;
    json_array_foreach(root, i, item) {
        if (!json_is_array(item) || json_array_size(item) < 2) continue;
        json_t* ev = json_array_get(item, 1);
        if (!json_is_array(ev) || json_array_size(ev) < 1) continue;
        json_t* jname = json_array_get(ev, 0);
        if (!json_is_string(jname)) continue;
        std::string cmd = json_string_value(jname);
        json_t* data = (json_array_size(ev) >= 2 && json_is_object(json_array_get(ev, 1))) ? json_array_get(ev, 1) : nullptr;

        if (cmd == "setPlaylist" && data) {
            std::string vid = jstr(data, "videoId");
            if (!vid.empty()) {
                Command c;
                c.type = Command::PLAY_YOUTUBE;
                c.target = vid;
                std::string ct = jstr(data, "currentTime");
                if (!ct.empty()) c.position = std::atof(ct.c_str());
                push_cmd(std::move(c));
            }
        } else if (cmd == "play") {
            push_cmd({Command::PLAY, "", "", 0.0});
        } else if (cmd == "pause") {
            push_cmd({Command::PAUSE, "", "", 0.0});
        } else if (cmd == "seekTo" && data) {
            std::string nt = jstr(data, "newTime");
            if (!nt.empty()) push_cmd({Command::SEEK, "", "", std::atof(nt.c_str())});
        } else if (cmd == "next") {
            push_cmd({Command::NEXT, "", "", 0.0});
        } else if (cmd == "previous") {
            push_cmd({Command::PREV, "", "", 0.0});
        } else if (cmd == "stopVideo") {
            push_cmd({Command::STOP, "", "", 0.0});
        } else if (cmd == "loungeStatus" && data) {
            json_t* devs = json_object_get(data, "devices");
            if (json_is_string(devs)) {
                json_t* darr = json_loads(json_string_value(devs), 0, &jerr);
                if (json_is_array(darr)) {
                    std::string found_dev;
                    size_t j;
                    json_t* d;
                    json_array_foreach(darr, j, d) {
                        if (jstr(d, "type") == "REMOTE_CONTROL") {
                            found_dev = jstr(d, "name");
                            if (!found_dev.empty()) break;
                        }
                    }
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_connected_device = found_dev;
                }
                if (darr) json_decref(darr);
            }
        }
    }
    json_decref(root);
}

void lounge_loop() {
    while (g_running) {
        std::string sid, ltok;
        if (!ensure_lounge_credentials(sid, ltok, false)) {
            for (int i = 0; i < 30 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string base_q = "VER=8&CVER=1&loungeIdToken=" + animesama::txt::url_encode(ltok) +
                             "&device=LOUNGE_SCREEN&id=" + animesama::txt::url_encode(sid) +
                             "&name=" + animesama::txt::url_encode("CoffeeFlix (Wii U)") + "&app=yt-tv";

        http::Request b_req;
        b_req.method = "POST";
        b_req.url = "https://www.youtube.com/api/lounge/bc/bind?RID=0&" + base_q;
        b_req.timeout = 12;
        b_req.cancel = &g_stop;
        b_req.headers.push_back({"User-Agent", UA});
        b_req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        b_req.body = "count=0";
        http::Response br = http::perform(b_req);
        if (!g_running) break;

        if (br.status == 401 || br.status == 403 || br.status == 400) {
            ensure_lounge_credentials(sid, ltok, true);
            for (int i = 0; i < 20 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string session_id, gsession_id;
        animesama::txt::between(br.body, "[\"c\",\"", "\"", session_id);
        animesama::txt::between(br.body, "[\"S\",\"", "\"", gsession_id);
        if (session_id.empty()) {
            for (int i = 0; i < 20 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string bind_url = "https://www.youtube.com/api/lounge/bc/bind?RID=rpc&CI=0&TYPE=xmlhttp&SID=" +
                               animesama::txt::url_encode(session_id) + "&" + base_q;
        if (!gsession_id.empty()) bind_url += "&gsessionid=" + animesama::txt::url_encode(gsession_id);

        std::string acc;
        http::Request l_req;
        l_req.url = bind_url;
        l_req.timeout = 55;
        l_req.cancel = &g_stop;
        l_req.headers.push_back({"User-Agent", UA});
        l_req.on_data = [&acc](const http::Response&, const char* data, size_t size) -> bool {
            if (!g_running) return false;
            acc.append(data, size);
            // YouTube Lounge sends length-prefixed JSON chunks: "<len>\n<json>\n"
            while (true) {
                size_t nl = acc.find('\n');
                if (nl == std::string::npos) break;
                std::string len_line = animesama::txt::trim(acc.substr(0, nl));
                if (len_line.empty() || !std::isdigit((unsigned char)len_line[0])) {
                    acc.erase(0, nl + 1);
                    continue;
                }
                size_t chunk_len = (size_t)std::atoi(len_line.c_str());
                if (acc.size() < nl + 1 + chunk_len) break;
                std::string chunk = acc.substr(nl + 1, chunk_len);
                acc.erase(0, nl + 1 + chunk_len);
                process_lounge_chunk(chunk);
            }
            return g_running.load();
        };
        http::perform(l_req);
    }
}

void ssdp_loop() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    g_ssdp_fd = fd;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1900);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        g_ssdp_fd = -1;
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    char buf[2048];
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{1, 0};
        if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string msg(buf, (size_t)n);
        if (msg.find("M-SEARCH") == std::string::npos) continue;
        if (msg.find("dial") == std::string::npos && msg.find("ssdp:all") == std::string::npos &&
            msg.find("rootdevice") == std::string::npos) {
            continue;
        }

        std::string ip = platform::ip_address();
        if (ip.empty() || ip == "0.0.0.0") continue;
        std::string uuid = get_device_uuid();
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "EXT:\r\n"
            "LOCATION: http://" + ip + ":8008/ssdp/device-desc.xml\r\n"
            "SERVER: WiiU/1.0 UPnP/1.0 CoffeeFlix/1.0\r\n"
            "ST: urn:dial-multiscreen-org:service:dial:1\r\n"
            "USN: uuid:" + uuid + "::urn:dial-multiscreen-org:service:dial:1\r\n\r\n";
        sendto(fd, resp.c_str(), resp.size(), 0, (sockaddr*)&from, flen);
    }

    close(fd);
    g_ssdp_fd = -1;
}

void send_http(int cfd, const std::string& status, const std::string& ctype, const std::string& body,
               const std::string& extra_hdrs = "") {
    std::string h = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n" + extra_hdrs + "\r\n";
    send(cfd, h.c_str(), h.size(), 0);
    if (!body.empty()) send(cfd, body.c_str(), body.size(), 0);
}

void http_loop() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    g_http_fd = fd;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8008);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        g_http_fd = -1;
        return;
    }

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{1, 0};
        if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int cfd = accept(fd, (sockaddr*)&cli, &clen);
        if (cfd < 0) continue;

        char buf[4096];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            close(cfd);
            continue;
        }
        buf[n] = '\0';
        std::string req(buf, (size_t)n);

        std::string method, path;
        size_t sp1 = req.find(' ');
        size_t sp2 = (sp1 != std::string::npos) ? req.find(' ', sp1 + 1) : std::string::npos;
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = req.substr(0, sp1);
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        size_t hdr_end = req.find("\r\n\r\n");
        std::string body = (hdr_end != std::string::npos) ? req.substr(hdr_end + 4) : "";
        std::string ip = platform::ip_address();

        if (path == "/ssdp/device-desc.xml") {
            std::string uuid = get_device_uuid();
            std::string xml =
                "<?xml version=\"1.0\"?>\n"
                "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
                "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
                "  <device>\n"
                "    <deviceType>urn:schemas-upnp-org:device:dial:1</deviceType>\n"
                "    <friendlyName>CoffeeFlix (Wii U)</friendlyName>\n"
                "    <manufacturer>CoffeeFlix</manufacturer>\n"
                "    <modelName>Wii U</modelName>\n"
                "    <UDN>uuid:" + uuid + "</UDN>\n"
                "  </device>\n"
                "</root>\n";
            send_http(cfd, "200 OK", "application/xml", xml,
                      "Application-URL: http://" + ip + ":8008/apps/\r\n");
        } else if (path == "/apps/YouTube" && method == "GET") {
            std::string sid = store::get_str(KEY_SCREEN_ID, "");
            std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<service xmlns=\"urn:dial-multiscreen-org:schemas:dial\" dialVer=\"2.1\">\n"
                "  <name>YouTube</name>\n"
                "  <options allowStop=\"true\"/>\n"
                "  <state>running</state>\n"
                "  <additionalData><screenId>" + sid + "</screenId></additionalData>\n"
                "</service>\n";
            send_http(cfd, "200 OK", "application/xml", xml);
        } else if (path == "/apps/YouTube" && method == "POST") {
            std::string vid = form_param(body, "v");
            std::string t = form_param(body, "t");
            if (!vid.empty()) {
                Command c;
                c.type = Command::PLAY_YOUTUBE;
                c.target = vid;
                if (!t.empty()) c.position = std::atof(t.c_str());
                push_cmd(std::move(c));
            }
            send_http(cfd, "201 Created", "text/plain", "");
        } else if (path == "/api/state" && method == "GET") {
            LiveState st;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                st = g_live_state;
            }
            json_t* obj = json_object();
            json_object_set_new(obj, "started", json_boolean(st.started));
            json_object_set_new(obj, "paused", json_boolean(st.paused));
            json_object_set_new(obj, "live", json_boolean(st.live));
            json_object_set_new(obj, "has_next", json_boolean(st.has_next));
            json_object_set_new(obj, "has_prev", json_boolean(st.has_prev));
            json_object_set_new(obj, "pos", json_real(st.pos));
            json_object_set_new(obj, "duration", json_real(st.duration));
            json_object_set_new(obj, "volume", json_integer(st.volume));
            json_object_set_new(obj, "title", json_string(st.title.c_str()));
            json_object_set_new(obj, "subtitle", json_string(st.subtitle.c_str()));
            char* dumped = json_dumps(obj, JSON_COMPACT);
            std::string js = dumped ? dumped : "{}";
            if (dumped) free(dumped);
            json_decref(obj);
            send_http(cfd, "200 OK", "application/json", js);
        } else if (path == "/manifest.json" && method == "GET") {
            std::string man =
                "{\"name\":\"CoffeeFlix Remote\",\"short_name\":\"CoffeeFlix\",\"start_url\":\"/\","
                "\"display\":\"standalone\",\"background_color\":\"#0b0b12\",\"theme_color\":\"#f59e0b\"}";
            send_http(cfd, "200 OK", "application/manifest+json", man);
        } else if (path == "/api/cmd" && method == "POST") {
            std::string cmd = form_param(body, "cmd");
            std::string val = form_param(body, "val");
            if (cmd == "toggle") push_cmd({Command::TOGGLE_PAUSE, "", "", 0.0});
            else if (cmd == "play") push_cmd({Command::PLAY, "", "", 0.0});
            else if (cmd == "pause") push_cmd({Command::PAUSE, "", "", 0.0});
            else if (cmd == "next") push_cmd({Command::NEXT, "", "", 0.0});
            else if (cmd == "prev") push_cmd({Command::PREV, "", "", 0.0});
            else if (cmd == "stop") push_cmd({Command::STOP, "", "", 0.0});
            else if (cmd == "seek_rel") push_cmd({Command::SEEK_REL, "", "", std::atof(val.c_str())});
            else if (cmd == "seek_abs") push_cmd({Command::SEEK, "", "", std::atof(val.c_str())});
            else if (cmd == "volume") push_cmd({Command::VOLUME, "", "", std::atof(val.c_str())});
            else if (cmd == "nav") push_cmd({Command::NAV, val, "", 0.0});
            else if (cmd == "section") push_cmd({Command::SECTION, val, "", 0.0});
            else if (cmd == "cast" && !val.empty()) {
                std::string yid = extract_yt_id(val);
                if (!yid.empty()) push_cmd({Command::PLAY_YOUTUBE, yid, "", 0.0});
                else push_cmd({Command::PLAY_URL, val, "Cast Web", 0.0});
            }
            send_http(cfd, "200 OK", "application/json", "{\"ok\":true}");
        } else if (path == "/cast" && method == "POST") {
            std::string u = form_param(body, "url");
            std::string action = form_param(body, "action");
            if (!u.empty()) {
                std::string yid = extract_yt_id(u);
                if (!yid.empty()) push_cmd({Command::PLAY_YOUTUBE, yid, "", 0.0});
                else push_cmd({Command::PLAY_URL, u, "Cast Web", 0.0});
            } else if (action == "play") push_cmd({Command::PLAY, "", "", 0.0});
            else if (action == "pause") push_cmd({Command::PAUSE, "", "", 0.0});
            else if (action == "stop") push_cmd({Command::STOP, "", "", 0.0});
            else if (action == "next") push_cmd({Command::NEXT, "", "", 0.0});
            else if (action == "prev") push_cmd({Command::PREV, "", "", 0.0});
            send_http(cfd, "303 See Other", "text/plain", "", "Location: /\r\n");
        } else {
            std::string html =
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
                "<meta name='theme-color' content='#0b0b12'>"
                "<link rel='manifest' href='/manifest.json'>"
                "<title>CoffeeFlix Remote</title>"
                "<style>"
                "*{box-sizing:border-box}body{background:#0b0b12;color:#f4f4f8;font-family:system-ui,-apple-system,sans-serif;margin:0;padding:16px;max-width:480px;margin:auto}"
                ".card{background:#171724;border-radius:16px;padding:16px;margin-bottom:14px}"
                "h2{margin:0 0 6px;font-size:18px;color:#f59e0b}small{color:#9ca3af}"
                ".row{display:flex;gap:8px;margin-top:10px}button{background:#252538;color:#fff;border:none;border-radius:12px;padding:12px;font-size:15px;font-weight:600;flex:1;cursor:pointer}"
                "button:active{transform:scale(0.96);background:#f59e0b;color:#111}.pri{background:#f59e0b;color:#111}"
                "input[type=text]{width:100%;padding:12px;border-radius:12px;border:none;background:#252538;color:#fff;font-size:15px;margin-bottom:8px}"
                "input[type=range]{width:100%;accent-color:#f59e0b;margin:8px 0}"
                ".dpad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:240px;margin:10px auto}"
                ".tabs{display:flex;flex-wrap:wrap;gap:6px}.tabs button{flex:0 0 calc(33.3% - 4px);font-size:13px;padding:10px 6px}"
                "</style></head><body>"
                "<div class='card'><h2 id='tt'>CoffeeFlix Wii U</h2><small id='st'>En attente...</small>"
                "<input id='sk' type='range' min='0' max='100' value='0' onchange='cmd(\"seek_abs\",this.value)'>"
                "<div class='row'><button onclick='cmd(\"prev\")'>Prev</button><button onclick='cmd(\"seek_rel\",-15)'>-15s</button>"
                "<button class='pri' id='pp' onclick='cmd(\"toggle\")'>Play/Pause</button><button onclick='cmd(\"seek_rel\",30)'>+30s</button>"
                "<button onclick='cmd(\"next\")'>Next</button></div>"
                "<div class='row'><button onclick='cmd(\"seek_rel\",85)'>+85s (OP)</button><button onclick='cmd(\"stop\")'>Stop</button></div>"
                "<div style='margin-top:10px'><small>Volume</small><input id='vl' type='range' min='0' max='100' value='100' oninput='cmd(\"volume\",this.value)'></div></div>"
                "<div class='card'><h2>Telecommande</h2>"
                "<div class='dpad'><div></div><button onclick='cmd(\"nav\",\"up\")'>&#9650;</button><div></div>"
                "<button onclick='cmd(\"nav\",\"left\")'>&#9664;</button><button class='pri' onclick='cmd(\"nav\",\"a\")'>A / OK</button><button onclick='cmd(\"nav\",\"right\")'>&#9654;</button>"
                "<button onclick='cmd(\"nav\",\"menu\")'>Menu</button><button onclick='cmd(\"nav\",\"down\")'>&#9660;</button><button onclick='cmd(\"nav\",\"b\")'>B / Retour</button></div></div>"
                "<div class='card'><h2>Caster une video / URL</h2>"
                "<input id='cu' type='text' placeholder='Lien YouTube ou URL .mp4 / .m3u8...'>"
                "<button class='pri' style='width:100%' onclick='castUrl()'>Caster sur la Wii U</button></div>"
                "<div class='card'><h2>Onglets rapides</h2><div class='tabs'>"
                "<button onclick='cmd(\"section\",\"home\")'>Accueil</button>"
                "<button onclick='cmd(\"section\",\"youtube\")'>YouTube</button>"
                "<button onclick='cmd(\"section\",\"ytmusic\")'>YT Music</button>"
                "<button onclick='cmd(\"section\",\"spotify\")'>Spotify</button>"
                "<button onclick='cmd(\"section\",\"soundcloud\")'>SoundCloud</button>"
                "<button onclick='cmd(\"section\",\"animesama\")'>Anime-Sama</button>"
                "<button onclick='cmd(\"section\",\"mangadex\")'>MangaDex</button>"
                "<button onclick='cmd(\"section\",\"freetv\")'>TV &amp; Freebox</button>"
                "<button onclick='cmd(\"section\",\"watchtogether\")'>WatchTogether</button>"
                "</div></div>"
                "<script>"
                "function cmd(c,v){fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)+'&val='+encodeURIComponent(v||'')});}"
                "function castUrl(){var el=document.getElementById('cu');if(el.value){cmd('cast',el.value);el.value='';}}"
                "setInterval(function(){fetch('/api/state').then(r=>r.json()).then(s=>{"
                "document.getElementById('tt').textContent=s.started?(s.title||'Lecture en cours'):'CoffeeFlix Wii U';"
                "document.getElementById('st').textContent=s.started?((s.subtitle||'')+' '+(s.paused?'(Pause)':'(Lecture)')):'Pret';"
                "document.getElementById('pp').textContent=s.paused?'Play':'Pause';"
                "var sk=document.getElementById('sk');if(s.duration>0){sk.max=Math.floor(s.duration);sk.value=Math.floor(s.pos);}"
                "}).catch(()=>{});},1200);"
                "</script></body></html>";
            send_http(cfd, "200 OK", "text/html; charset=utf-8", html);
        }
        close(cfd);
    }

    close(fd);
    g_http_fd = -1;
}

}  // namespace

void start() {
    if (g_running) return;
    g_stop = false;
    g_running = true;
    g_http_thread = std::thread(http_loop);
    g_ssdp_thread = std::thread(ssdp_loop);
    g_lounge_thread = std::thread(lounge_loop);
    log_message(LOG_OK, "Cast", "Cast receiver started (DIAL :8008 + YouTube Lounge)");
}

void stop() {
    if (!g_running) return;
    g_running = false;
    g_stop = true;
    if (g_http_fd >= 0) close(g_http_fd);
    if (g_ssdp_fd >= 0) close(g_ssdp_fd);
    if (g_http_thread.joinable()) g_http_thread.join();
    if (g_ssdp_thread.joinable()) g_ssdp_thread.join();
    if (g_lounge_thread.joinable()) g_lounge_thread.join();
    log_message(LOG_OK, "Cast", "Cast receiver stopped");
}

bool running() { return g_running.load(); }

std::string pairing_code() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_pairing_code;
}

std::string web_url() {
    std::string ip = platform::ip_address();
    if (ip.empty() || ip == "0.0.0.0") return "";
    return "http://" + ip + ":8008/";
}

std::string connected_device() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_connected_device;
}

void refresh_pairing_code() {
    std::thread([]() {
        std::string sid, ltok;
        ensure_lounge_credentials(sid, ltok, true);
    }).detach();
}

void update_state(const LiveState& st) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_live_state = st;
}

bool poll_command(Command& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_queue.empty()) return false;
    out = std::move(g_queue.front());
    g_queue.pop_front();
    return true;
}

}  // namespace cast_receiver

