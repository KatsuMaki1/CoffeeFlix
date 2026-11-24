// SoundCloud: trending charts, track search and audio playback (progressive MP3 / HLS + Last.fm scrobbling).
#include <algorithm>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/soundcloud.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace sc = soundcloud;

class SoundCloudScreen : public app::Screen {
public:
    SoundCloudScreen() { load_charts(); }

    app::Section section() const override { return app::SEC_SOUNDCLOUD; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("soundcloud");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 300, "SoundCloud", t.text);
        std::string acc_label = sc::signed_in() ? sc::username() : "Connexion";
        float bw = std::min(260.0f, std::max(150.0f, measure_button(acc_label.c_str(), ic::PEOPLE)));
        if (button(id(g, "account"), Rect(W - 60 - bw, y, bw, 44), acc_label.c_str(), ic::PEOPLE, BTN_NORMAL, g)) {
            if (sc::signed_in()) {
                sc::sign_out();
                toast("Deconnecte de SoundCloud", ic::INFO);
                if (kind_ == "likes") {
                    kind_ = "trending";
                    load_charts();
                }
            } else {
                prompt_text("Profil SoundCloud", "", "Pseudo ou URL (ex: soundcloud.com/pseudo)", [this](std::string u) {
                    std::string err;
                    if (sc::sign_in_username(u, err)) {
                        toast(util::fmt("Connecte: %s", sc::username().c_str()), ic::CHECK_CIRCLE);
                        kind_ = "likes";
                        query_.clear();
                        load_charts();
                    } else {
                        toast(err.empty() ? "Profil introuvable" : err, ic::ERROR_OUTLINE);
                    }
                });
            }
        }
        y += 58;

        const char* kinds[] = {"trending", "top", "likes"};
        const char* labels[] = {"Tendances", "Top Charts", "Mes Likes"};
        Id kg = id(g, "kinds");
        float px = x0;
        for (int i = 0; i < 3; i++) {
            float cw = std::max(130.0f, text::measure(font::body_bold, labels[i]) + 36.0f);
            if (chip(id(kg, (int64_t)i), Rect(px, y, cw, 40), labels[i], kind_ == kinds[i] && query_.empty(), kg)) {
                kind_ = kinds[i];
                query_.clear();
                load_charts();
            }
            px += cw + 12;
        }
        if (focus_in_group(kg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un son, artiste, remix...", g, F_DEFAULT)) {
            prompt_text("Rechercher sur SoundCloud", query_, "Titre ou artiste", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Chargement SoundCloud...");
            y += 200;
        } else if (!list_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "SoundCloud inaccessible", list_.error.c_str())) {
                if (query_.empty()) load_charts();
                else run_search();
            }
            y += 280;
        } else if (list_.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::MUSIC, "Aucun resultat",
                        util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
            y += 280;
        } else {
            GridSpec gs;
            gs.count = (int)list_.items.size();
            gs.cols = 4;
            gs.shape = CARD_SQUARE;
            gs.item_w = 240;
            gs.gap_x = 24;
            gs.item = [this](int i) {
                const auto& trk = list_.items[i];
                CardInfo c;
                c.image = trk.artwork;
                c.image_w = 300;
                c.title = trk.title;
                c.subtitle = trk.artist;
                c.icon = ic::MUSIC;
                return c;
            };
            gs.on_focus = [this](int i) { set_backdrop(list_.items[i].artwork); };
            gs.on_click = [this](int i) { play_from(i); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load_charts() {
        loading_ = true;
        std::string k = kind_;
        scope_.reset();
        scope_.run<sc::TrackList>(
            [k] {
                if (k == "likes") return sc::user_likes();
                return sc::charts(k);
            },
            [this](sc::TrackList res) {
                list_ = std::move(res);
                loading_ = false;
            });
    }

    void run_search() {
        if (query_.empty()) {
            load_charts();
            return;
        }
        loading_ = true;
        std::string q = query_;
        scope_.reset();
        scope_.run<sc::TrackList>([q] { return sc::search(q); }, [this](sc::TrackList res) {
            list_ = std::move(res);
            loading_ = false;
        });
    }

    void play_from(int index) {
        std::vector<player::Source> queue;
        for (const auto& t : list_.items) queue.push_back(sc::make_source(t));
        if (!queue.empty()) play_audio_queue(std::move(queue), index);
    }

    std::string kind_ = "trending";
    std::string query_;
    sc::TrackList list_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_soundcloud() { return std::make_unique<SoundCloudScreen>(); }

}  // namespace screens

