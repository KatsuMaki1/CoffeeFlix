// Reddit: browse subreddits and view posts/memes fullscreen via photo_viewer.
#include <algorithm>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/reddit.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace rd = reddit;

class RedditScreen : public app::Screen {
public:
    RedditScreen() { load(); }

    app::Section section() const override { return app::SEC_REDDIT; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("reddit");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 60, "Reddit", t.text);
        y += 58;

        const char* subs[] = {"memes", "france", "gaming", "wiiu", "animemes", "dankmemes"};
        Id sg = id(g, "subs");
        float px = x0;
        for (int i = 0; i < 6; i++) {
            std::string lbl = std::string("r/") + subs[i];
            float cw = std::max(105.0f, text::measure(font::body_bold, lbl) + 28.0f);
            if (chip(id(sg, (int64_t)i), Rect(px, y, cw, 38), lbl.c_str(), sub_ == subs[i], sg)) {
                if (sub_ != subs[i]) {
                    sub_ = subs[i];
                    load();
                }
            }
            px += cw + 10;
        }
        if (focus_in_group(sg)) page_.focus_range(0, y + 58 + page_.scroll());
        y += 54;

        Rect sb(x0, y, W - x0 - 60, 54);
        if (search_bar(id(g, "sub_search"), sb, custom_, "Aller sur un subreddit (ex: france, pics)...", g, F_DEFAULT)) {
            prompt_text("Subreddit", sub_, "Nom du subreddit (sans r/)", [this](std::string s) {
                if (!s.empty()) {
                    custom_ = s;
                    sub_ = std::move(s);
                    load();
                }
            });
        }
        y += 76;

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 90, "Chargement de Reddit...");
            y += 200;
        } else if (!list_.error.empty() || list_.items.empty()) {
            const char* err = list_.error.empty() ? "Aucun post disponible" : list_.error.c_str();
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 240), ic::WIFI_OFF,
                                   "Reddit inaccessible", err)) {
                load();
            }
            y += 260;
        } else {
            GridSpec gs;
            gs.count = (int)list_.items.size();
            gs.cols = 4;
            gs.shape = CARD_SQUARE;
            gs.item_w = 240;
            gs.gap_x = 24;
            gs.item = [this](int i) {
                const auto& p = list_.items[i];
                CardInfo c;
                c.image = p.thumb_url;
                c.image_w = 320;
                c.title = p.title;
                c.subtitle = util::fmt("u/%s · %d pts", p.author.c_str(), p.ups);
                c.icon = ic::PHOTO;
                return c;
            };
            gs.on_focus = [this](int i) { set_backdrop(list_.items[i].thumb_url); };
            gs.on_click = [this](int i) {
                std::vector<std::string> urls;
                for (const auto& p : list_.items) urls.push_back(p.image_url);
                app::push(make_photo_viewer(std::move(urls), i));
            };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Open")}, {"B", tr("Back")}});
    }

private:
    void load() {
        loading_ = true;
        std::string s = sub_;
        scope_.reset();
        scope_.run<rd::PostList>([s] { return rd::fetch_subreddit(s); }, [this](rd::PostList res) {
            list_ = std::move(res);
            loading_ = false;
        });
    }

    std::string sub_ = "memes";
    std::string custom_;
    rd::PostList list_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_reddit() { return std::make_unique<RedditScreen>(); }

}  // namespace screens

