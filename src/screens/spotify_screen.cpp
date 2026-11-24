// Spotify: public playlists (Top Hits, Top 50 FR, Lo-Fi, custom URL), optional token login (Mes Likes), & search.
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
#include "services/spotify.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace sp = spotify;

class SpotifyScreen : public app::Screen {
public:
    SpotifyScreen() { load_playlist(); }

    app::Section section() const override { return app::SEC_SPOTIFY; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("spotify");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 440, "Spotify", t.text);
        float imp_w = 175;
        if (button(id(g, "import"), Rect(W - 60 - imp_w, y, imp_w, 44), "Playlist URL", ic::PLAYLIST_PLAY, BTN_NORMAL, g)) {
            prompt_text("Importer une playlist Spotify", "", "URL ou ID de playlist", [this](std::string u) {
                if (!u.empty()) {
                    preset_ = "custom";
                    custom_playlist_ = std::move(u);
                    query_.clear();
                    load_playlist();
                }
            });
        }
        std::string acc_lbl = sp::signed_in() ? sp::username() : "Connexion";
        float acc_w = std::min(220.0f, std::max(145.0f, measure_button(acc_lbl.c_str(), ic::PEOPLE)));
        if (button(id(g, "account"), Rect(W - 60 - imp_w - 12 - acc_w, y, acc_w, 44), acc_lbl.c_str(), ic::PEOPLE,
                   BTN_NORMAL, g)) {
            if (sp::signed_in()) {
                sp::sign_out();
                toast("Deconnecte de Spotify", ic::INFO);
                if (preset_ == "likes") {
                    preset_ = "top_hits";
                    load_playlist();
                }
            } else {
                prompt_text("Spotify Access Token", "", "Bearer Access Token", [this](std::string tok) {
                    std::string err;
                    if (sp::sign_in_token(tok, err)) {
                        toast(util::fmt("Connecte: %s", sp::username().c_str()), ic::CHECK_CIRCLE);
                        preset_ = "likes";
                        query_.clear();
                        load_playlist();
                    } else {
                        toast(err.empty() ? "Connexion Spotify echouee" : err, ic::ERROR_OUTLINE);
                    }
                }, true);
            }
        }
        y += 58;

        const char* keys[] = {"top_hits", "top_fr", "lofi", "rap_fr", "likes"};
        const char* labels[] = {"Top Hits", "Top 50 FR", "Lo-Fi Beats", "Rap FR", "Mes Likes"};
        Id kg = id(g, "presets");
        float px = x0;
        for (int i = 0; i < 5; i++) {
            float cw = std::max(115.0f, text::measure(font::body_bold, labels[i]) + 30.0f);
            if (chip(id(kg, (int64_t)i), Rect(px, y, cw, 40), labels[i], preset_ == keys[i] && query_.empty(), kg)) {
                preset_ = keys[i];
                query_.clear();
                load_playlist();
            }
            px += cw + 10;
        }
        if (focus_in_group(kg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un son ou artiste...", g, F_DEFAULT)) {
            prompt_text("Rechercher sur Spotify", query_, "Titre ou artiste", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Chargement Spotify...");
            y += 200;
        } else if (!list_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "Spotify indisponible", list_.error.c_str())) {
                if (query_.empty()) load_playlist();
                else run_search();
            }
            y += 280;
        } else if (list_.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::MUSIC, "Aucun titre",
                        util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
            y += 280;
        } else {
            Id tg = id(g, "tracks");
            float rw = W - x0 - 60;
            float rh = 64;
            for (size_t i = 0; i < list_.items.size(); i++) {
                Rect r(x0, y + i * (rh + 6), rw, rh);
                Id tid = id(tg, (int64_t)i);
                const auto& trk = list_.items[i];
                std::string label = trk.artist.empty() ? trk.title : (trk.title + "  —  " + trk.artist);
                if (button(tid, r, label.c_str(), ic::MUSIC, BTN_NORMAL, tg)) {
                    play_from((int)i);
                }
                if (ui::focused() == tid) {
                    page_.focus_range(r.y + page_.scroll() - 20, r.b() + page_.scroll() + 20);
                }
            }
            y += (float)list_.items.size() * (rh + 6) + 20;
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load_playlist() {
        loading_ = true;
        std::string p = preset_;
        std::string cust = custom_playlist_;
        scope_.reset();
        scope_.run<sp::TrackList>(
            [p, cust] {
                if (p == "likes") return sp::user_likes();
                if (p == "custom") return sp::fetch_playlist(cust);
                if (p == "top_fr") return sp::fetch_playlist("37i9dQZEVXbIPWwFssbupI");
                if (p == "lofi") return sp::fetch_playlist("37i9dQZF1DWWQRwui0ExPn");
                if (p == "rap_fr") return sp::fetch_playlist("37i9dQZF1DX1X23oiQ1v6Y");
                return sp::fetch_playlist("37i9dQZF1DXcBWIGoYBM5M");
            },
            [this](sp::TrackList res) {
                list_ = std::move(res);
                loading_ = false;
            });
    }

    void run_search() {
        if (query_.empty()) {
            load_playlist();
            return;
        }
        loading_ = true;
        std::string q = query_;
        scope_.reset();
        scope_.run<sp::TrackList>([q] { return sp::search(q); }, [this](sp::TrackList res) {
            list_ = std::move(res);
            loading_ = false;
        });
    }

    void play_from(int index) {
        std::vector<player::Source> queue;
        for (const auto& t : list_.items) queue.push_back(sp::make_source(t));
        if (!queue.empty()) play_audio_queue(std::move(queue), index);
    }

    std::string preset_ = "top_hits";
    std::string custom_playlist_;
    std::string query_;
    sp::TrackList list_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_spotify() { return std::make_unique<SpotifyScreen>(); }

}  // namespace screens

