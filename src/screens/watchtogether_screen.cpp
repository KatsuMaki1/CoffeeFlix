// WatchTogether: create/join a synchronized watch party room and apply incoming sync events.
#include <cmath>
#include <string>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/youtube_common.hpp"
#include "services/animesama.hpp"
#include "services/watchtogether.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace wt = watchtogether;

class WatchTogetherScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_WATCHTOGETHER; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("watchtogether");

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "WatchTogether", t.text);
        y += 62;

        if (!wt::active()) {
            Rect card(x0, y, W - x0 - 60, 180);
            gfx::fill_rrect(card, 16, t.surface);
            text::draw_fit(font::title, card.x + 28, card.y + 28, card.w - 56,
                           "Regarde a plusieurs en temps reel", t.text);
            text::draw_wrapped(font::body, Rect(card.x + 28, card.y + 72, card.w - 56, 84),
                               "Cree un salon ou rejoins le code d'un ami. Ensuite lance une video (YouTube, Anime-Sama, StreamsDL...) : lecture, pause et avance seront synchronisees automatiquement !",
                               t.text2, 3);
            y += 206;

            float bw = 260;
            if (button(id(g, "create"), Rect(x0, y, bw, 56), "Creer un salon", ic::PLAY, BTN_PRIMARY, g, F_DEFAULT)) {
                std::string code = wt::create_room();
                toast(util::fmt("Salon cree: %s", code.c_str()), ic::CHECK_CIRCLE);
            }
            if (button(id(g, "join"), Rect(x0 + bw + 20, y, bw, 56), "Rejoindre un salon", ic::PEOPLE, BTN_NORMAL, g)) {
                prompt_text("Rejoindre WatchTogether", "", "Code a 6 caracteres (ex: A7K2M9)", [](std::string code) {
                    if (wt::join_room(code)) {
                        toast(util::fmt("Salon %s rejoint !", wt::room_code().c_str()), ic::CHECK_CIRCLE);
                    } else {
                        toast("Code salon invalide", ic::ERROR_OUTLINE);
                    }
                });
            }
            y += 80;
        } else {
            std::string code = wt::room_code();
            Rect card(x0, y, W - x0 - 60, 210);
            gfx::fill_rrect(card, 16, t.surface);
            text::draw(font::small_bold, card.x + 28, card.y + 24, "SALON ACTIF — PARTAGE CE CODE", t.accent);
            text::draw(font::display, card.x + 28, card.y + 58, code, t.text);

            std::string media = wt::current_media_title();
            std::string status = media.empty()
                                     ? "En attente d'une video... Lance un episode ou une video pour synchroniser."
                                     : ("Synchronise sur : " + media);
            text::draw_wrapped(font::body, Rect(card.x + 28, card.y + 132, card.w - 56, 60), status, t.text2, 2);
            y += 236;

            if (button(id(g, "leave"), Rect(x0, y, 250, 54), "Quitter le salon", ic::STOP, BTN_NORMAL, g, F_DEFAULT)) {
                wt::leave_room();
                toast("Salon quitte", ic::INFO);
            }
            y += 74;
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_watchtogether() { return std::make_unique<WatchTogetherScreen>(); }

void poll_watchtogether() {
    wt::SyncEvent ev;
    if (!wt::poll_event(ev)) return;

    const player::Source& cur = player::source();
    std::string cur_id = cur.id.empty() ? cur.url : cur.id;
    bool need_open = (ev.id != cur_id && !ev.id.empty());

    wt::note_remote_applied();

    if (need_open) {
        ui::toast(util::fmt("WatchTogether: %s", ev.title.empty() ? ev.id.c_str() : ev.title.c_str()), ic::PEOPLE);
        if (ev.service == "youtube") {
            youtube::Video v;
            v.id = ev.id;
            v.title = ev.title;
            v.channel = ev.subtitle;
            yt::play(v);
        } else if (!ev.url.empty()) {
            // Direct resolved stream URL (bypasses Cloudflare on anime-sama.to for the friend!)
            player::Source s;
            s.url = ev.url;
            s.title = ev.title;
            s.subtitle = ev.subtitle;
            s.artwork = ev.image;
            s.service = ev.service;
            s.id = ev.id;
            s.extra = ev.extra;
            s.start = ev.pos;
            s.chunked_http = ev.chunked;
            if (!ev.referer.empty()) s.headers.push_back({"Referer", ev.referer});
            if (ev.has_video) play_video(s);
            else play_audio(s);
        } else if (ev.service == "animesama") {
            play_video(animesama::source_from_resume(ev.id, ev.title, ev.subtitle, ev.image, ev.extra, ev.pos));
        }
        return;
    }

    if (!player::started()) return;

    bool cur_paused = (player::state() == player::PAUSED);
    if (ev.paused != cur_paused) {
        player::set_paused(ev.paused);
    }
    double cur_pos = player::position();
    if (!player::live() && std::abs(ev.pos - cur_pos) > 2.2) {
        player::seek(ev.pos);
    }
}

}  // namespace screens

