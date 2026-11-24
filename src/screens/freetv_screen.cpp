// TV & Freebox: Freebox Multiposte (M3U/RTSP) + DRM-free French Live TV (France.tv, ARTE) & France.tv Replays.
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
#include "services/animesama_text.hpp"
#include "services/freetv.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace ftv = freetv;

class FreeTvScreen : public app::Screen {
public:
    FreeTvScreen() { load(); }

    app::Section section() const override { return app::SEC_FREETV; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("freetv");

        page_.begin(id(g, "page"));
        float y = page_.y(48);

        text::draw_fit(font::headline, x0, y, W - x0 - 300, "TV & Freebox", t.text);
        if (tab_ == 0) {
            float bw = 210;
            if (button(id(g, "fbx_url"), Rect(W - 60 - bw, y, bw, 44), "URL Freebox", ic::SETTINGS, BTN_NORMAL, g)) {
                prompt_text("Playlist Freebox M3U", ftv::freebox_url(),
                            "http://mafreebox.freebox.fr/freeboxtv/playlist.m3u",
                            [this](std::string u) {
                                ftv::set_freebox_url(u);
                                load();
                            },
                            false, true);
            }
        } else if (tab_ == 3) {
            float bw = 210;
            if (button(id(g, "iptv_url"), Rect(W - 60 - bw, y, bw, 44), "M3U perso", ic::SETTINGS, BTN_NORMAL, g)) {
                prompt_text("URL Playlist M3U IPTV", ftv::iptv_custom_url(),
                            "https://iptv-org.github.io/iptv/countries/fr.m3u",
                            [this](std::string u) {
                                ftv::set_iptv_custom_url(u);
                                iptv_key_ = "custom";
                                load();
                            },
                            false, true);
            }
        }
        y += 58;

        // Top mode tabs
        const char* tabs[] = {"Freebox TV", "Directs FR", "Replays France.tv", "IPTV (iptv-org)"};
        Id tg = id(g, "tabs");
        float tx = x0;
        for (int i = 0; i < 4; i++) {
            float cw = std::max(130.0f, text::measure(font::body_bold, tabs[i]) + 36.0f);
            if (chip(id(tg, (int64_t)i), Rect(tx, y, cw, 40), tabs[i], tab_ == i, tg)) {
                if (tab_ != i) {
                    tab_ = i;
                    filter_.clear();
                    load();
                }
            }
            tx += cw + 12;
        }
        if (focus_in_group(tg)) page_.focus_range(0, y + 60 + page_.scroll());
        y += 56;

        // Replay channel selector
        if (tab_ == 2) {
            const char* slugs[] = {"france-2", "france-3", "france-4", "france-5", "franceinfo", "okoo"};
            const char* labels[] = {"France 2", "France 3", "France 4", "France 5", "Franceinfo", "Okoo"};
            Id sg = id(g, "slugs");
            float sx = x0;
            for (int i = 0; i < 6; i++) {
                float cw = std::max(105.0f, text::measure(font::body_bold, labels[i]) + 28.0f);
                if (chip(id(sg, (int64_t)i), Rect(sx, y, cw, 38), labels[i], replay_slug_ == slugs[i], sg)) {
                    if (replay_slug_ != slugs[i]) {
                        replay_slug_ = slugs[i];
                        load();
                    }
                }
                sx += cw + 10;
            }
            if (focus_in_group(sg)) page_.focus_range(0, y + 56 + page_.scroll());
            y += 52;
        } else if (tab_ == 3) {
            const char* keys[] = {"fr", "fra", "animation", "movies", "sports", "music", "custom"};
            const char* labels[] = {"France", "Langue FR", "Animation", "Films", "Sport", "Musique", "Perso"};
            Id ig = id(g, "iptv_cats");
            float sx = x0;
            for (int i = 0; i < 7; i++) {
                float cw = std::max(95.0f, text::measure(font::body_bold, labels[i]) + 24.0f);
                if (chip(id(ig, (int64_t)i), Rect(sx, y, cw, 38), labels[i], iptv_key_ == keys[i], ig)) {
                    if (iptv_key_ != keys[i]) {
                        iptv_key_ = keys[i];
                        load();
                    }
                }
                sx += cw + 8;
            }
            if (focus_in_group(ig)) page_.focus_range(0, y + 56 + page_.scroll());
            y += 52;
        }

        // Filter bar for Freebox / Replays / IPTV
        if (tab_ == 0 || tab_ == 2 || tab_ == 3) {
            Rect sb(x0, y, W - x0 - 60, 52);
            if (search_bar(id(g, "filter"), sb, filter_, "Filtrer les chaines / programmes...", g, F_DEFAULT)) {
                prompt_text("Filtrer", filter_, "Ex: TF1, Gulli, France 2...", [this](std::string q) {
                    filter_ = std::move(q);
                });
            }
            y += 70;
        }

        if (loading_) {
            loading_indicator(x0 + (W - x0 - 60) * 0.5f, y + 80, "Chargement des chaines...");
            y += 180;
        } else if (!list_.error.empty()) {
            if (empty_state_action(id(g, "retry"), Rect(x0, y, W - x0 - 60, 240), ic::WIFI_OFF,
                                   "Chaines indisponibles", list_.error.c_str())) {
                load();
            }
            y += 260;
        } else {
            std::vector<const ftv::Channel*> shown;
            std::string f_low = animesama::txt::lower(animesama::txt::trim(filter_));
            for (const auto& ch : list_.items) {
                if (!f_low.empty() && animesama::txt::lower(ch.name).find(f_low) == std::string::npos) continue;
                shown.push_back(&ch);
            }

            if (shown.empty()) {
                empty_state(Rect(x0, y, W - x0 - 60, 220), ic::LIVE_TV, "Aucune chaine",
                            "Essaie un autre filtre ou change d'onglet.");
                y += 240;
            } else {
                Id cg = id(g, "channels");
                const int cols = 2;
                float gap = 16;
                float bw = (W - x0 - 60 - gap) / cols;
                float bh = 58;
                for (size_t i = 0; i < shown.size(); i++) {
                    int col = (int)(i % cols);
                    int row = (int)(i / cols);
                    Rect br(x0 + col * (bw + gap), y + row * (bh + 12), bw, bh);
                    Id bid = id(cg, (int64_t)i);
                    int flags = (i == 0 && tab_ == 1) ? F_DEFAULT : 0;
                    if (button(bid, br, shown[i]->name.c_str(), ic::LIVE_TV, BTN_NORMAL, cg, flags)) {
                        play_video(ftv::make_channel_source(*shown[i]));
                    }
                    if (ui::focused() == bid) {
                        page_.focus_range(br.y + page_.scroll() - 20, br.b() + page_.scroll() + 20);
                    }
                }
                int rows = ((int)shown.size() + cols - 1) / cols;
                y += rows * (bh + 12) + 20;
            }
        }

        page_.end(y + page_.scroll());
        hint_bar({{"A", tr("Play")}, {"B", tr("Back")}});
    }

private:
    void load() {
        loading_ = true;
        int t = tab_;
        std::string slug = replay_slug_;
        std::string ikey = iptv_key_;
        scope_.reset();
        scope_.run<ftv::ChannelList>(
            [t, slug, ikey] {
                if (t == 0) return ftv::get_freebox_channels();
                if (t == 1) return ftv::get_fr_live_channels();
                if (t == 2) return ftv::get_francetv_replays(slug);
                return ftv::get_iptv_channels(ikey);
            },
            [this](ftv::ChannelList res) {
                list_ = std::move(res);
                loading_ = false;
            });
    }

    int tab_ = 0;
    std::string replay_slug_ = "france-2";
    std::string iptv_key_ = "fr";
    std::string filter_;
    ftv::ChannelList list_;
    bool loading_ = false;
    tasks::Scope scope_;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> make_freetv() { return std::make_unique<FreeTvScreen>(); }

}  // namespace screens
