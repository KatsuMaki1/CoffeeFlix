// Cast Receiver screen: YouTube TV Pairing Code (official Cast button) + Web Cast QR code & DIAL status.
#include <string>

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/input.hpp"
#include "core/qr.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/youtube_common.hpp"
#include "services/cast_receiver.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace cr = cast_receiver;

class CastScreen : public app::Screen {
public:
    CastScreen() {
        if (!cr::running()) cr::start();
    }

    app::Section section() const override { return app::SEC_CAST; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("cast");

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "Cast / Recepteur TV", t.text);
        y += 60;

        // YouTube TV Pairing card
        Rect card(x0, y, W - x0 - 310, 240);
        gfx::fill_rrect(card, 16, t.surface);
        text::draw(font::small_bold, card.x + 28, card.y + 24, "YOUTUBE CAST (CODE TV)", t.accent);

        std::string code = cr::pairing_code();
        text::draw(font::display, card.x + 28, card.y + 56, code.empty() ? "Chargement..." : code, t.text);

        std::string dev = cr::connected_device();
        std::string info = dev.empty()
                               ? "Dans YouTube sur ton telephone : Parametres -> Regarder sur la TV -> Associer avec un code TV."
                               : ("Telephone connecte : " + dev + " (utilise le bouton Cast dans YouTube !)");
        text::draw_wrapped(font::body, Rect(card.x + 28, card.y + 132, card.w - 56, 84), info, t.text2, 3);

        // QR Code for Web Cast Remote (http://<ip>:8008/)
        std::string wurl = cr::web_url();
        if (!wurl.empty()) {
            Rect qcard(W - 60 - 225, y, 225, 240);
            gfx::fill_rrect(qcard, 16, t.surface);
            qr::Code qrc = qr::encode(wurl);
            if (qrc.size > 0) {
                float qsz = 160.0f;
                float qx = qcard.cx() - qsz * 0.5f;
                float qy = qcard.y + 20;
                gfx::fill_rrect(Rect(qx - 8, qy - 8, qsz + 16, qsz + 16), 8, gfx::WHITE);
                float mod = qsz / (float)qrc.size;
                for (int r = 0; r < qrc.size; r++) {
                    for (int c = 0; c < qrc.size; c++) {
                        if (qrc.at(c, r)) {
                            gfx::fill_rect(Rect(qx + c * mod, qy + r * mod, mod + 0.4f, mod + 0.4f), gfx::BLACK);
                        }
                    }
                }
            }
            text::draw_fit(font::small_bold, qcard.x + 12, qcard.b() - 38, qcard.w - 24, wurl, t.text2, text::CENTER);
        }

        y += 266;

        if (button(id(g, "refresh"), Rect(x0, y, 240, 52), "Nouveau code TV", ic::REFRESH, BTN_PRIMARY, g, F_DEFAULT)) {
            cr::refresh_pairing_code();
            toast("Generation d'un nouveau code TV...", ic::INFO);
        }
        bool on = cr::running();
        if (button(id(g, "toggle"), Rect(x0 + 256, y, 240, 52),
                   on ? "Desactiver Cast" : "Activer Cast", ic::CAST, BTN_NORMAL, g)) {
            if (on) cr::stop();
            else cr::start();
        }
        y += 74;

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_cast() { return std::make_unique<CastScreen>(); }

void poll_cast_receiver(Input& in) {
    cr::LiveState st;
    st.started = player::started();
    st.paused = (player::state() == player::PAUSED);
    st.live = player::live();
    st.has_next = player::has_next();
    st.has_prev = player::has_previous();
    st.pos = player::position();
    st.duration = player::duration();
    const player::Source& ps = player::source();
    st.title = ps.title;
    st.subtitle = ps.subtitle;
    cr::update_state(st);

    cr::Command cmd;
    if (!cr::poll_command(cmd)) return;

    switch (cmd.type) {
        case cr::Command::PLAY_YOUTUBE: {
            youtube::Video v;
            v.id = cmd.target;
            v.title = cmd.target;
            ui::toast("Cast YouTube recu !", ic::CAST);
            yt::play(v);
            break;
        }
        case cr::Command::PLAY_URL: {
            player::Source s;
            s.url = cmd.target;
            s.title = cmd.title.empty() ? "Cast Web" : cmd.title;
            s.start = cmd.position;
            ui::toast("Cast media recu !", ic::CAST);
            play_video(s);
            break;
        }
        case cr::Command::PLAY:
            if (player::started()) player::set_paused(false);
            break;
        case cr::Command::PAUSE:
            if (player::started()) player::set_paused(true);
            break;
        case cr::Command::TOGGLE_PAUSE:
            if (player::started()) player::toggle_pause();
            break;
        case cr::Command::SEEK:
            if (player::started()) player::seek(cmd.position);
            break;
        case cr::Command::SEEK_REL:
            if (player::started()) player::seek_relative(cmd.position);
            break;
        case cr::Command::NEXT:
            if (player::has_next()) player::next();
            break;
        case cr::Command::PREV:
            if (player::has_previous()) player::previous();
            break;
        case cr::Command::STOP:
            player::close();
            break;
        case cr::Command::VOLUME: {
            float v = (float)(cmd.position / 100.0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            audio::set_media_volume(v);
            break;
        }
        case cr::Command::NAV: {
            if (cmd.target == "up") { in.pressed |= bit(BTN_UP); in.repeat |= bit(BTN_UP); }
            else if (cmd.target == "down") { in.pressed |= bit(BTN_DOWN); in.repeat |= bit(BTN_DOWN); }
            else if (cmd.target == "left") { in.pressed |= bit(BTN_LEFT); in.repeat |= bit(BTN_LEFT); }
            else if (cmd.target == "right") { in.pressed |= bit(BTN_RIGHT); in.repeat |= bit(BTN_RIGHT); }
            else if (cmd.target == "a") in.pressed |= bit(BTN_A);
            else if (cmd.target == "b") in.pressed |= bit(BTN_B);
            else if (cmd.target == "menu") app::focus_rail();
            break;
        }
        case cr::Command::SECTION: {
            if (cmd.target == "home") app::open_section(app::SEC_HOME);
            else if (cmd.target == "youtube") app::open_section(app::SEC_YOUTUBE);
            else if (cmd.target == "ytmusic") app::open_section(app::SEC_YTMUSIC);
            else if (cmd.target == "spotify") app::open_section(app::SEC_SPOTIFY);
            else if (cmd.target == "soundcloud") app::open_section(app::SEC_SOUNDCLOUD);
            else if (cmd.target == "animesama") app::open_section(app::SEC_ANIMESAMA);
            else if (cmd.target == "mangadex") app::open_section(app::SEC_MANGADEX);
            else if (cmd.target == "freetv") app::open_section(app::SEC_FREETV);
            else if (cmd.target == "watchtogether") app::open_section(app::SEC_WATCHTOGETHER);
            break;
        }
        default:
            break;
    }
}

}  // namespace screens

