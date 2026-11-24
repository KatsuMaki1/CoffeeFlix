// StreamsDL: French movies & series via Starland9/streams_dl API (Flemmix, PapaduStream, French-Stream).
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
#include "services/streams_dl.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace sd = streams_dl;

class StreamsDetailScreen : public app::Screen {
public:
    StreamsDetailScreen(sd::Media media, std::string provider)
        : media_(std::move(media)), provider_(std::move(provider)) {
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
        text::draw_fit(font::body_bold, x0, y, text_w, provider_, t.accent);
        y = std::max(y + 48, poster_r.y + ph + 24);

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 60, "Extraction des liens UQload...");
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
            std::string label = v.title.empty() ? util::fmt("Lien %d", (int)i + 1) : v.title;
            if (!v.resolution.empty()) label += " (" + v.resolution + ")";
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
        std::string prov = provider_;
        scope_.reset();
        scope_.run<sd::VideoResults>([url, prov] { return sd::get_videos(url, prov); },
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
    std::string provider_;
    sd::VideoResults videos_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

class StreamsDlScreen : public app::Screen {
public:
    StreamsDlScreen() { load_providers(); }

    app::Section section() const override { return app::SEC_STREAMSDL; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("streamsdl");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 300, "Films & Series", t.text);
        std::string srv = sd::server_url();
        float sw = std::min(300.0f, std::max(160.0f, measure_button(srv.c_str(), ic::SETTINGS)));
        if (button(id(g, "server"), Rect(W - 60 - sw, y, sw, 44), srv.c_str(), ic::SETTINGS, BTN_NORMAL, g)) {
            prompt_text("Serveur streams_dl", srv, "http://192.168.1.x:8000", [this](std::string u) {
                sd::set_server_url(u);
                load_providers();
            }, false, true);
        }
        y += 60;

        // Provider chips
        Id pg = id(g, "providers");
        float px = x0;
        for (size_t i = 0; i < providers_.size(); i++) {
            float cw = std::max(120.0f, text::measure(font::body_bold, providers_[i]) + 36.0f);
            if (chip(id(pg, (int64_t)i), Rect(px, y, cw, 40), providers_[i].c_str(), providers_[i] == provider_, pg)) {
                if (providers_[i] != provider_) {
                    provider_ = providers_[i];
                    if (searched_ && !query_.empty()) run_search();
                }
            }
            px += cw + 12;
        }
        if (focus_in_group(pg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        // Search bar
        Rect sb(x0, y, W - x0 - 60, 56);
        if (search_bar(id(g, "search"), sb, query_, "Rechercher un film ou une serie...", g, F_DEFAULT)) {
            prompt_text("Rechercher (StreamsDL)", query_, "Titre du film ou de la serie", [this](std::string q) {
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
                                   "Serveur streams_dl inaccessible", results_.error.c_str())) {
                run_search();
            }
            y += 280;
        } else if (!searched_) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::LIVE_TV, "StreamsDL (Flemmix / French-Stream / PapaduStream)",
                        "Configure l'adresse de ton serveur streams_dl en haut a droite puis lance une recherche.");
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
                app::push(std::make_unique<StreamsDetailScreen>(results_.items[i], provider_));
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
    }

private:
    void load_providers() {
        prov_scope_.reset();
        prov_scope_.run<std::vector<std::string>>(
            [] {
                std::string err;
                return sd::get_providers(err);
            },
            [this](std::vector<std::string> list) {
                if (!list.empty()) {
                    providers_ = std::move(list);
                    if (std::find(providers_.begin(), providers_.end(), provider_) == providers_.end())
                        provider_ = providers_[0];
                }
            });
    }

    void run_search() {
        if (query_.empty()) return;
        loading_ = true;
        searched_ = true;
        std::string q = query_;
        std::string prov = provider_;
        scope_.reset();
        scope_.run<sd::SearchResults>([q, prov] { return sd::search(q, prov); }, [this](sd::SearchResults res) {
            results_ = std::move(res);
            loading_ = false;
        });
    }

    std::string query_;
    std::vector<std::string> providers_ = {"french-stream", "flemmix", "papadustream"};
    std::string provider_ = "french-stream";
    sd::SearchResults results_;
    bool loading_ = false;
    bool searched_ = false;
    tasks::Scope scope_;
    tasks::Scope prov_scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_streamsdl() { return std::make_unique<StreamsDlScreen>(); }

}  // namespace screens

