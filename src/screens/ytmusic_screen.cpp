// YouTube Music: songs, albums & playlists via WEB_REMIX InnerTube API, audio playback via CoffeeFlix.
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
#include "services/ytmusic.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace ym = ytmusic;

class YtMusicCollectionScreen : public app::Screen {
public:
    explicit YtMusicCollectionScreen(ym::Collection col) : col_(std::move(col)) { load(); }

    app::Section section() const override { return app::SEC_YTMUSIC; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ym_col");
        set_backdrop(col_.thumbnail);

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        float pw = 210, ph = 210;
        Rect art_r(W - 60 - pw, y, pw, ph);
        gfx::fill_rrect(art_r, 12, t.surface);
        if (const images::Image* img = images::get(col_.thumbnail, 300, 300)) {
            if (img->tex) gfx::image_cover(img->tex, img->w, img->h, art_r, 12, Color(255, 255, 255, 255));
        }

        float text_w = W - x0 - pw - 96;
        const std::string& title = detail_.title.empty() ? col_.title : detail_.title;
        text::draw_wrapped(font::display, Rect(x0, y, text_w, 110), title, t.text, 2);
        y += text::measure_wrapped(font::display, text_w, title, 2) + 8;
        const std::string& sub = detail_.subtitle.empty() ? col_.subtitle : detail_.subtitle;
        if (!sub.empty()) {
            text::draw_fit(font::body, x0, y, text_w, sub, t.text2);
            y += 36;
        }

        if (!loading_ && !detail_.tracks.empty()) {
            float bw = measure_button(tr("Play all"), ic::PLAY);
            if (button(id(g, "playall"), Rect(x0, y + 12, bw, 48), tr("Play all"), ic::PLAY, BTN_PRIMARY, g, F_DEFAULT)) {
                play_from(0);
            }
            if (ui::focused() == id(g, "playall")) page_.focus_range(0, y + 80 + page_.scroll());
        }
        y = std::max(y + 72, art_r.y + ph + 24);

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60, "Chargement des titres...");
            page_.end(y + 140 + page_.scroll());
            hint_bar({{"B", tr("Back")}});
            return;
        }

        if (!detail_.error.empty() || detail_.tracks.empty()) {
            const char* err = detail_.error.empty() ? "Aucun titre trouve" : detail_.error.c_str();
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 220), ic::WIFI_OFF,
                                   "Impossible de charger", err)) {
                load();
            }
            page_.end(y + 240 + page_.scroll());
            hint_bar({{"A", tr("Retry")}, {"B", tr("Back")}});
            return;
        }

        Id tg = id(g, "tracks");
        float rw = W - x0 - 60;
        float rh = 64;
        for (size_t i = 0; i < detail_.tracks.size(); i++) {
            Rect r(x0, y + i * (rh + 6), rw, rh);
            Id tid = id(tg, (int64_t)i);
            Item it = focusable(tid, r, tg);
            if (it.focused) page_.focus_range(r.y + page_.scroll() - 20, r.b() + page_.scroll() + 20);
            Rect br = r.offset(bump_x(tid), bump_y(tid));
            gfx::fill_rrect(br, 10, gfx::lerp(Color(255, 255, 255, i % 2 ? 6 : 12), t.surface_focus, it.f));
            Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
            Color fg2 = gfx::lerp(t.text2, Color(21, 18, 26, 170), it.f);
            text::draw_fit(font::body_bold, br.x + 18, br.y + 10, br.w - 36, detail_.tracks[i].title, fg);
            text::draw_fit(font::small, br.x + 18, br.y + 36, br.w - 36, detail_.tracks[i].artist, fg2);
            if (it.clicked) play_from((int)i);
        }
        y += detail_.tracks.size() * (rh + 6) + 20;

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load() {
        loading_ = true;
        std::string bid = col_.browse_id;
        scope_.reset();
        scope_.run<ym::CollectionDetail>([bid] { return ym::browse(bid); }, [this](ym::CollectionDetail d) {
            detail_ = std::move(d);
            loading_ = false;
        });
    }

    void play_from(int idx) {
        std::vector<player::Source> q;
        for (const auto& t : detail_.tracks) q.push_back(ym::make_audio_source(t));
        if (!q.empty()) play_audio_queue(std::move(q), idx);
    }

    ym::Collection col_;
    ym::CollectionDetail detail_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

class YtMusicScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_YTMUSIC; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("ytmusic");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "YouTube Music", t.text);
        y += 58;

        // Filter chips
        Id fg = id(g, "filters");
        struct Tab { ym::Filter f; const char* label; };
        const Tab tabs[] = {{ym::Filter::SONGS, "Chansons"}, {ym::Filter::ALBUMS, "Albums"}, {ym::Filter::PLAYLISTS, "Playlists"}};
        float cx = x0;
        for (size_t i = 0; i < 3; i++) {
            float cw = 140;
            if (chip(id(fg, (int64_t)i), Rect(cx, y, cw, 40), tabs[i].label, filter_ == tabs[i].f, fg)) {
                if (filter_ != tabs[i].f) {
                    filter_ = tabs[i].f;
                    if (searched_ && !query_.empty()) run_search();
                }
            }
            cx += cw + 12;
        }
        if (focus_in_group(fg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        // Search bar
        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un titre, un artiste, un album...", g, F_DEFAULT)) {
            prompt_text("Rechercher sur YouTube Music", query_, "Titre, artiste ou album", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Recherche sur YouTube Music...");
            y += 200;
        } else if (!results_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "YouTube Music inaccessible", results_.error.c_str())) {
                run_search();
            }
            y += 280;
        } else if (!searched_) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::MUSIC, "YouTube Music",
                        "Recherche des chansons, albums ou playlists (lecture audio en arriere-plan).");
            y += 280;
        } else if (filter_ == ym::Filter::SONGS) {
            if (results_.tracks.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 260), ic::SEARCH, "Aucun resultat",
                            util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
                y += 280;
            } else {
                GridSpec gs;
                gs.count = (int)results_.tracks.size();
                gs.cols = 4;
                gs.shape = CARD_SQUARE;
                gs.item_w = 240;
                gs.gap_x = 24;
                gs.item = [this](int i) {
                    const auto& trk = results_.tracks[i];
                    CardInfo c;
                    c.image = trk.thumbnail;
                    c.image_w = 300;
                    c.title = trk.title;
                    c.subtitle = trk.artist;
                    c.icon = ic::MUSIC;
                    return c;
                };
                gs.on_focus = [this](int i) { set_backdrop(results_.tracks[i].thumbnail); };
                gs.on_click = [this](int i) {
                    std::vector<player::Source> q;
                    for (const auto& trk : results_.tracks) q.push_back(ym::make_audio_source(trk));
                    play_audio_queue(std::move(q), i);
                };
                y += grid(id(g, "grid_songs"), x0, y, gs, &page_);
            }
        } else {
            if (results_.collections.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 260), ic::SEARCH, "Aucun resultat",
                            util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
                y += 280;
            } else {
                GridSpec gs;
                gs.count = (int)results_.collections.size();
                gs.cols = 4;
                gs.shape = CARD_SQUARE;
                gs.item_w = 240;
                gs.gap_x = 24;
                gs.item = [this](int i) {
                    const auto& col = results_.collections[i];
                    CardInfo c;
                    c.image = col.thumbnail;
                    c.image_w = 300;
                    c.title = col.title;
                    c.subtitle = col.subtitle;
                    c.icon = ic::ALBUM;
                    return c;
                };
                gs.on_focus = [this](int i) { set_backdrop(results_.collections[i].thumbnail); };
                gs.on_click = [this](int i) {
                    app::push(std::make_unique<YtMusicCollectionScreen>(results_.collections[i]));
                };
                y += grid(id(g, "grid_cols"), x0, y, gs, &page_);
            }
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
        ym::Filter f = filter_;
        scope_.reset();
        scope_.run<ym::SearchResults>([q, f] { return ym::search(q, f); }, [this](ym::SearchResults res) {
            results_ = std::move(res);
            loading_ = false;
        });
    }

    std::string query_;
    ym::Filter filter_ = ym::Filter::SONGS;
    ym::SearchResults results_;
    bool loading_ = false;
    bool searched_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_ytmusic() { return std::make_unique<YtMusicScreen>(); }

}  // namespace screens

