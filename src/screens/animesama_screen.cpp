// Anime-Sama: search, anime details, seasons (VOSTFR / VF) and episode playback.
#include <algorithm>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/animesama.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace as = animesama;

class AnimeDetailScreen : public app::Screen {
public:
    explicit AnimeDetailScreen(as::SearchResult anime, as::Lang lang)
        : anime_(std::move(anime)), lang_(lang) {
        load_info();
    }

    app::Section section() const override { return app::SEC_ANIMESAMA; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("as_detail");
        set_backdrop(anime_.cover);

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        // Poster on the right
        float pw = 210, ph = 300;
        Rect poster_r(W - 60 - pw, y, pw, ph);
        gfx::fill_rrect(poster_r, 12, t.surface);
        if (const images::Image* img = images::get(anime_.cover, 300, 420)) {
            if (img->tex) {
                gfx::image_cover(img->tex, img->w, img->h, poster_r, 12, Color(255, 255, 255, 255));
            }
        }

        float text_w = W - x0 - pw - 96;
        const std::string& title = info_.title.empty() ? anime_.title : info_.title;
        text::draw_wrapped(font::display, Rect(x0, y, text_w, 120), title, t.text, 2);
        y += text::measure_wrapped(font::display, text_w, title, 2) + 8;

        if (!info_.alt_titles.empty()) {
            text::draw_fit(font::body, x0, y, text_w, info_.alt_titles[0], t.text2);
            y += 32;
        }

        if (!info_.synopsis.empty()) {
            y += text::draw_wrapped(font::body, Rect(x0, y, text_w, 150), info_.synopsis, t.text2, 5, text::LEFT, 1.15f) + 16;
        }

        y = std::max(y, poster_r.y + ph + 20);

        if (loading_info_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60, "Chargement de la fiche...");
            page_.end(y + 140 + page_.scroll());
            hint_bar({{"B", tr("Back")}});
            return;
        }

        if (!info_.error.empty()) {
            if (empty_state_action(id(g, "retry_info"), Rect(x0, y, W - x0 - 60, 220), ic::WIFI_OFF,
                                   "Impossible de charger l'anime", info_.error.c_str())) {
                load_info();
            }
            page_.end(y + 240 + page_.scroll());
            hint_bar({{"A", tr("Retry")}, {"B", tr("Back")}});
            return;
        }

        // Language chips (VOSTFR / VF)
        Id lg = id(g, "lang");
        float cx = x0;
        if (chip(id(lg, "vostfr"), Rect(cx, y, 130, 42), "VOSTFR", lang_ == as::Lang::VOSTFR, lg)) {
            if (lang_ != as::Lang::VOSTFR) {
                lang_ = as::Lang::VOSTFR;
                load_episodes();
            }
        }
        cx += 144;
        if (chip(id(lg, "vf"), Rect(cx, y, 96, 42), "VF", lang_ == as::Lang::VF, lg)) {
            if (lang_ != as::Lang::VF) {
                lang_ = as::Lang::VF;
                load_episodes();
            }
        }
        if (focus_in_group(lg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 58;

        // Season chips
        Id sg = id(g, "seasons");
        float sx = x0;
        float sy0 = y;
        for (size_t i = 0; i < info_.seasons.size(); i++) {
            float cw = std::max(110.0f, text::measure(font::body_bold, info_.seasons[i].name) + 36.0f);
            if (sx + cw > W - 60) {
                sx = x0;
                y += 52;
            }
            if (chip(id(sg, (int64_t)i), Rect(sx, y, cw, 42), info_.seasons[i].name.c_str(), (int)i == season_idx_, sg,
                     i == 0 ? F_DEFAULT : 0)) {
                if ((int)i != season_idx_) {
                    season_idx_ = (int)i;
                    load_episodes();
                }
            }
            sx += cw + 12;
        }
        if (focus_in_group(sg)) page_.focus_range(sy0 + page_.scroll() - 20, y + 52 + page_.scroll());
        y += 64;

        // Episodes
        if (loading_eps_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60, "Chargement des episodes...");
            y += 140;
        } else if (!eps_.error.empty() || eps_.episodes.empty()) {
            const char* err = eps_.error.empty() ? "Aucun episode disponible" : eps_.error.c_str();
            if (empty_state_action(id(g, "retry_eps"), Rect(x0, y, W - x0 - 60, 200), ic::MOVIE, "Aucun episode", err)) {
                load_episodes();
            }
            y += 220;
        } else {
            Id eg = id(g, "episodes");
            const int cols = 4;
            float gap = 16;
            float bw = (W - x0 - 60 - gap * (cols - 1)) / cols;
            float bh = 54;
            for (size_t i = 0; i < eps_.episodes.size(); i++) {
                int col = (int)(i % cols);
                int row = (int)(i / cols);
                Rect br(x0 + col * (bw + gap), y + row * (bh + 14), bw, bh);
                Id bid = id(eg, (int64_t)i);
                std::string label = util::fmt("Episode %d", eps_.episodes[i].number);
                int flags = (i == 0) ? F_DEFAULT : 0;
                if (button(bid, br, label.c_str(), ic::PLAY, BTN_NORMAL, eg, flags)) {
                    play_from((int)i);
                }
                if (ui::focused() == bid) {
                    page_.focus_range(br.y + page_.scroll() - 20, br.b() + page_.scroll() + 20);
                }
            }
            int rows = ((int)eps_.episodes.size() + cols - 1) / cols;
            y += rows * (bh + 14) + 20;
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load_info() {
        loading_info_ = true;
        as::SearchResult anime = anime_;
        scope_.reset();
        scope_.run<as::AnimeInfo>([anime] { return as::get_info(anime); }, [this](as::AnimeInfo info) {
            info_ = std::move(info);
            loading_info_ = false;
            season_idx_ = 0;
            if (info_.error.empty() && !info_.seasons.empty()) load_episodes();
        });
    }

    void load_episodes() {
        if (season_idx_ < 0 || season_idx_ >= (int)info_.seasons.size()) return;
        loading_eps_ = true;
        as::Season season = info_.seasons[season_idx_];
        as::Lang lang = lang_;
        ep_scope_.reset();
        ep_scope_.run<as::EpisodeList>([season, lang] { return as::get_episodes(season, lang); },
                                       [this](as::EpisodeList list) {
                                           eps_ = std::move(list);
                                           loading_eps_ = false;
                                       });
    }

    void play_from(int index) {
        if (season_idx_ < 0 || season_idx_ >= (int)info_.seasons.size()) return;
        std::vector<player::Source> queue;
        const auto& season = info_.seasons[season_idx_];
        for (const auto& ep : eps_.episodes) {
            queue.push_back(as::make_source(info_, season, eps_, ep));
        }
        if (!queue.empty()) play_video_queue(std::move(queue), index);
    }

    as::SearchResult anime_;
    as::Lang lang_;
    as::AnimeInfo info_;
    as::EpisodeList eps_;
    int season_idx_ = 0;
    bool loading_info_ = false;
    bool loading_eps_ = false;
    tasks::Scope scope_;
    tasks::Scope ep_scope_;
    Page page_;
};

class AnimeSamaScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_ANIMESAMA; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("animesama");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 260, "Anime-Sama", t.text);
        Id lg = id(g, "lang");
        if (chip(id(lg, "toggle"), Rect(W - 210, y + 2, 150, 42), lang_ == as::Lang::VOSTFR ? "VOSTFR" : "VF", true,
                 lg)) {
            lang_ = (lang_ == as::Lang::VOSTFR) ? as::Lang::VF : as::Lang::VOSTFR;
        }
        y += 64;

        // Search bar
        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un anime (ex: Frieren, One Piece)...", g, F_DEFAULT)) {
            prompt_text("Rechercher sur Anime-Sama", query_, "Nom de l'anime", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Recherche en cours...");
            y += 200;
        } else if (!results_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "Anime-Sama inaccessible", results_.error.c_str())) {
                run_search();
            }
            y += 280;
        } else if (!searched_) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::MOVIE, "Anime-Sama",
                        "Appuie sur la barre de recherche ci-dessus pour trouver un anime en VOSTFR ou VF.");
            y += 280;
        } else if (results_.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::SEARCH, "Aucun resultat",
                        util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
            y += 280;
        } else {
            GridSpec gs;
            gs.count = (int)results_.items.size();
            gs.cols = 4;
            gs.shape = CARD_POSTER;
            gs.item_w = 240;
            gs.gap_x = 24;
            gs.item = [this](int i) {
                const auto& r = results_.items[i];
                CardInfo c;
                c.image = r.cover;
                c.image_w = 300;
                c.title = r.title;
                c.icon = ic::MOVIE;
                return c;
            };
            gs.on_focus = [this](int i) { set_backdrop(results_.items[i].cover); };
            gs.on_click = [this](int i) {
                app::push(std::make_unique<AnimeDetailScreen>(results_.items[i], lang_));
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void run_search() {
        if (query_.empty()) return;
        loading_ = true;
        searched_ = true;
        std::string q = query_;
        scope_.reset();
        scope_.run<as::SearchResults>([q] { return as::search(q); }, [this](as::SearchResults res) {
            results_ = std::move(res);
            loading_ = false;
        });
    }

    std::string query_;
    as::Lang lang_ = as::Lang::VOSTFR;
    as::SearchResults results_;
    bool loading_ = false;
    bool searched_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_animesama() { return std::make_unique<AnimeSamaScreen>(); }

}  // namespace screens

