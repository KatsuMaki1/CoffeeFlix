// StreamsDL: French movies & series auto-fetched directly (no external server URL needed).
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
#include "services/streams_dl.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace sd = streams_dl;

class StreamsDetailScreen : public app::Screen {
public:
    StreamsDetailScreen(sd::Media media, std::string category)
        : media_(std::move(media)), category_(std::move(category)) {
        load();
    }

    app::Section section() const override { return app::SEC_STREAMSDL; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("sd_detail");
        set_backdrop(media_.image_url);

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        float pw = 200, ph = 285;
        Rect poster_r(W - 60 - pw, y, pw, ph);
        gfx::fill_rrect(poster_r, 12, t.surface);
        if (const images::Image* img = images::get(media_.image_url, 300, 420)) {
            if (img->tex) gfx::image_cover(img->tex, img->w, img->h, poster_r, 12, Color(255, 255, 255, 255));
        }

        float text_w = W - x0 - pw - 96;
        text::draw_wrapped(font::display, Rect(x0, y, text_w, 120), media_.title, t.text, 2);
        y += text::measure_wrapped(font::display, text_w, media_.title, 2) + 12;
        text::draw_fit(font::body_bold, x0, y, text_w, category_, t.accent);
        y = std::max(y + 48, poster_r.y + ph + 24);

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60, "Chargement des lecteurs...");
            page_.end(y + 140 + page_.scroll());
            hint_bar({{"B", tr("Back")}});
            return;
        }

        if (!videos_.error.empty() || videos_.items.empty()) {
            const char* err = videos_.error.empty() ? "Aucune video trouvee sur cette page" : videos_.error.c_str();
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 220), ic::WIFI_OFF,
                                   "Aucune video disponible", err)) {
                load();
            }
            page_.end(y + 240 + page_.scroll());
            hint_bar({{"A", tr("Retry")}, {"B", tr("Back")}});
            return;
        }

        Id vg = id(g, "videos");
        const int cols = 2;
        float gap = 16;
        float bw = (W - x0 - 60 - gap) / cols;
        float bh = 58;
        for (size_t i = 0; i < videos_.items.size(); i++) {
            int col = (int)(i % cols);
            int row = (int)(i / cols);
            Rect br(x0 + col * (bw + gap), y + row * (bh + 14), bw, bh);
            Id bid = id(vg, (int64_t)i);
            const auto& v = videos_.items[i];
            std::string label = v.title.empty() ? util::fmt("Lecteur %d", (int)i + 1) : v.title;
            if (button(bid, br, label.c_str(), ic::PLAY, BTN_NORMAL, vg, i == 0 ? F_DEFAULT : 0)) {
                play_from((int)i);
            }
            if (ui::focused() == bid) {
                page_.focus_range(br.y + page_.scroll() - 20, br.b() + page_.scroll() + 20);
            }
        }
        int rows = ((int)videos_.items.size() + cols - 1) / cols;
        y += rows * (bh + 14) + 20;

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load() {
        loading_ = true;
        std::string url = media_.url;
        std::string cat = category_;
        scope_.reset();
        scope_.run<sd::VideoResults>([url, cat] { return sd::get_videos(url, cat); },
                                     [this](sd::VideoResults res) {
                                         videos_ = std::move(res);
                                         loading_ = false;
                                     });
    }

    void play_from(int index) {
        std::vector<player::Source> queue;
        for (const auto& v : videos_.items) queue.push_back(sd::make_source(media_.title, media_.image_url, v));
        if (!queue.empty()) play_video_queue(std::move(queue), index);
    }

    sd::Media media_;
    std::string category_;
    sd::VideoResults videos_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

class StreamsDlScreen : public app::Screen {
public:
    StreamsDlScreen() { load_browse(); }

    app::Section section() const override { return app::SEC_STREAMSDL; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("streamsdl");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "Films & Series", t.text);
        y += 58;

        // Category chips
        Id cg = id(g, "cats");
        float px = x0;
        for (size_t i = 0; i < categories_.size(); i++) {
            float cw = std::max(120.0f, text::measure(font::body_bold, categories_[i]) + 36.0f);
            if (chip(id(cg, (int64_t)i), Rect(px, y, cw, 40), categories_[i].c_str(),
                     categories_[i] == category_, cg)) {
                if (categories_[i] != category_) {
                    category_ = categories_[i];
                    query_.clear();
                    load_browse();
                }
            }
            px += cw + 12;
        }
        if (focus_in_group(cg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        // Search bar
        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un film ou une serie...", g, F_DEFAULT)) {
            prompt_text("Rechercher (Films & Series)", query_, "Titre du film ou de la serie", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Chargement du catalogue...");
            y += 200;
        } else if (!results_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "Site inaccessible", results_.error.c_str())) {
                if (query_.empty()) load_browse();
                else run_search();
            }
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
                c.image = r.image_url;
                c.image_w = 300;
                c.title = r.title;
                c.icon = ic::MOVIE;
                return c;
            };
            gs.on_focus = [this](int i) { set_backdrop(results_.items[i].image_url); };
            gs.on_click = [this](int i) {
                app::push(std::make_unique<StreamsDetailScreen>(results_.items[i], category_));
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void load_browse() {
        loading_ = true;
        std::string cat = category_;
        scope_.reset();
        scope_.run<sd::SearchResults>([cat] { return sd::browse(cat); }, [this](sd::SearchResults res) {
            results_ = std::move(res);
            loading_ = false;
        });
    }

    void run_search() {
        if (query_.empty()) {
            load_browse();
            return;
        }
        loading_ = true;
        std::string q = query_;
        std::string cat = category_;
        scope_.reset();
        scope_.run<sd::SearchResults>([q, cat] { return sd::search(q, cat); }, [this](sd::SearchResults res) {
            results_ = std::move(res);
            loading_ = false;
        });
    }

    std::string query_;
    std::vector<std::string> categories_ = {"Nouveautes", "Films", "Series"};
    std::string category_ = "Nouveautes";
    sd::SearchResults results_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_streamsdl() { return std::make_unique<StreamsDlScreen>(); }

}  // namespace screens
