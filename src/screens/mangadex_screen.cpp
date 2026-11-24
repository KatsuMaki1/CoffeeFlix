// MangaDex: popular/search manga, FR/EN chapter list, and fullscreen page reader via photo_viewer.
#include <algorithm>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/text.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/mangadex.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace md = mangadex;

class MangaDetailScreen : public app::Screen {
public:
    explicit MangaDetailScreen(md::Manga manga) : manga_(std::move(manga)) { load_chapters(); }

    app::Section section() const override { return app::SEC_MANGADEX; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("md_detail");
        set_backdrop(manga_.cover_url);

        page_.begin(id(g, "page"));
        float y = page_.y(52);

        float pw = 200, ph = 285;
        Rect poster_r(W - 60 - pw, y, pw, ph);
        gfx::fill_rrect(poster_r, 12, t.surface);
        if (const images::Image* img = images::get(manga_.cover_url, 300, 420)) {
            if (img->tex) gfx::image_cover(img->tex, img->w, img->h, poster_r, 12, Color(255, 255, 255, 255));
        }

        float text_w = W - x0 - pw - 96;
        text::draw_wrapped(font::display, Rect(x0, y, text_w, 120), manga_.title, t.text, 2);
        y += text::measure_wrapped(font::display, text_w, manga_.title, 2) + 12;
        if (!manga_.description.empty()) {
            y += text::draw_wrapped(font::body, Rect(x0, y, text_w, 150), manga_.description, t.text2, 5, text::LEFT,
                                    1.15f) +
                 16;
        }
        y = std::max(y, poster_r.y + ph + 24);

        if (loading_ || opening_chap_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60,
                              opening_chap_ ? "Chargement des pages..." : "Chargement des chapitres...");
            page_.end(y + 140 + page_.scroll());
            hint_bar({{"B", tr("Back")}});
            return;
        }

        if (!chaps_.error.empty() || chaps_.items.empty()) {
            const char* err = chaps_.error.empty() ? "Aucun chapitre disponible" : chaps_.error.c_str();
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 220), ic::WIFI_OFF,
                                   "Impossible de charger", err)) {
                load_chapters();
            }
            page_.end(y + 240 + page_.scroll());
            hint_bar({{"A", tr("Retry")}, {"B", tr("Back")}});
            return;
        }

        Id cg = id(g, "chaps");
        const int cols = 3;
        float gap = 14;
        float bw = (W - x0 - 60 - gap * (cols - 1)) / cols;
        float bh = 56;
        for (size_t i = 0; i < chaps_.items.size(); i++) {
            int col = (int)(i % cols);
            int row = (int)(i / cols);
            Rect br(x0 + col * (bw + gap), y + row * (bh + 12), bw, bh);
            Id bid = id(cg, (int64_t)i);
            const auto& ch = chaps_.items[i];
            std::string label = ch.number.empty() ? "Chapitre" : ("Ch. " + ch.number);
            if (!ch.lang.empty()) label += " [" + ch.lang + "]";
            if (button(bid, br, label.c_str(), ic::BOOK, BTN_NORMAL, cg, i == 0 ? F_DEFAULT : 0)) {
                open_chapter(ch.id);
            }
            if (ui::focused() == bid) {
                page_.focus_range(br.y + page_.scroll() - 20, br.b() + page_.scroll() + 20);
            }
        }
        int rows = ((int)chaps_.items.size() + cols - 1) / cols;
        y += rows * (bh + 12) + 20;

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    void load_chapters() {
        loading_ = true;
        std::string mid = manga_.id;
        scope_.reset();
        scope_.run<md::ChapterList>([mid] { return md::get_chapters(mid); }, [this](md::ChapterList res) {
            chaps_ = std::move(res);
            loading_ = false;
        });
    }

    void open_chapter(const std::string& cid) {
        opening_chap_ = true;
        page_scope_.reset();
        page_scope_.run<md::PageList>([cid] { return md::get_pages(cid); }, [this](md::PageList res) {
            opening_chap_ = false;
            if (!res.error.empty() || res.urls.empty()) {
                toast(res.error.empty() ? "Impossible de charger le chapitre" : res.error, ic::ERROR_OUTLINE);
                return;
            }
            app::push(make_photo_viewer(std::move(res.urls), 0));
        });
    }

    md::Manga manga_;
    md::ChapterList chaps_;
    bool loading_ = false;
    bool opening_chap_ = false;
    tasks::Scope scope_;
    tasks::Scope page_scope_;
    Page page_;
};

class MangaDexScreen : public app::Screen {
public:
    MangaDexScreen() { load_popular(); }

    app::Section section() const override { return app::SEC_MANGADEX; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("mangadex");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "MangaDex", t.text);
        y += 58;

        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un manga (FR / EN)...", g, F_DEFAULT)) {
            prompt_text("Rechercher sur MangaDex", query_, "Titre du manga", [this](std::string q) {
                query_ = std::move(q);
                run_search();
            });
        }
        y += 80;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Chargement MangaDex...");
            y += 200;
        } else if (!list_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 260), ic::WIFI_OFF,
                                   "MangaDex inaccessible", list_.error.c_str())) {
                if (query_.empty()) load_popular();
                else run_search();
            }
            y += 280;
        } else if (list_.items.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::BOOK, "Aucun manga",
                        util::fmt("Rien trouve pour \"%s\"", query_.c_str()).c_str());
            y += 280;
        } else {
            GridSpec gs;
            gs.count = (int)list_.items.size();
            gs.cols = 4;
            gs.shape = CARD_POSTER;
            gs.item_w = 240;
            gs.gap_x = 24;
            gs.item = [this](int i) {
                const auto& m = list_.items[i];
                CardInfo c;
                c.image = m.cover_url;
                c.image_w = 300;
                c.title = m.title;
                c.icon = ic::BOOK;
                return c;
            };
            gs.on_focus = [this](int i) { set_backdrop(list_.items[i].cover_url); };
            gs.on_click = [this](int i) { app::push(std::make_unique<MangaDetailScreen>(list_.items[i])); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void load_popular() {
        loading_ = true;
        scope_.reset();
        scope_.run<md::MangaList>([] { return md::browse_popular(); }, [this](md::MangaList res) {
            list_ = std::move(res);
            loading_ = false;
        });
    }

    void run_search() {
        if (query_.empty()) {
            load_popular();
            return;
        }
        loading_ = true;
        std::string q = query_;
        scope_.reset();
        scope_.run<md::MangaList>([q] { return md::search(q); }, [this](md::MangaList res) {
            list_ = std::move(res);
            loading_ = false;
        });
    }

    std::string query_;
    md::MangaList list_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_mangadex() { return std::make_unique<MangaDexScreen>(); }

}  // namespace screens

