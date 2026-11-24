// WatchTogether: create/join a synchronized watch party room and apply incoming
// sync events.
#include <cmath>
#include <string>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "player/player.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "screens/youtube_common.hpp"
#include "services/animesama.hpp"
#include "services/watchtogether.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
namespace wt = watchtogether;

class WatchTogetherScreen : public app::Screen {
public:
  app::Section section() const override { return app::SEC_WATCHTOGETHER; }

  void frame() override {
    const Theme &t = theme();
    float x0 = content_x();
    Id g = id("watchtogether");

    page_.begin(id(g, "page"));
    float y = page_.y(52);

    text::draw_fit(font::headline, x0, y, W - x0 - 60, "WatchTogether", t.text);
    y += 62;

    if (!wt::active()) {
      Rect card(x0, y, W - x0 - 60, 180);
      gfx::fill_rrect(card, 16, t.surface);
      text::draw_fit(font::title, card.x + 28, card.y + 28, card.w - 56,
                     "Regarde a plusieurs en temps reel", t.text);
      text::draw_wrapped(
          font::body, Rect(card.x + 28, card.y + 72, card.w - 56, 84),
          "Cree un salon ou rejoins le code d'un ami. Ensuite lance une video "
          "(YouTube, Anime-Sama, StreamsDL...) : lecture, pause et avance "
          "seront synchronisees automatiquement !",
          t.text2, 3);
      y += 206;

      float bw = 260;
      if (button(id(g, "create"), Rect(x0, y, bw, 56), "Creer un salon",
                 ic::PLAY, BTN_PRIMARY, g, F_DEFAULT)) {
        std::string code = wt::create_room();
        toast(util::fmt("Salon cree: %s", code.c_str()), ic::CHECK_CIRCLE);
      }
      if (button(id(g, "join"), Rect(x0 + bw + 20, y, bw, 56),
                 "Rejoindre un salon", ic::PEOPLE, BTN_NORMAL, g)) {
        prompt_text(
            "Rejoindre WatchTogether", "", "Code a 6 caracteres (ex: A7K2M9)",
            [](std::string code) {
              if (wt::join_room(code)) {
                toast(util::fmt("Salon %s rejoint !", wt::room_code().c_str()),
                      ic::CHECK_CIRCLE);
              } else {
                toast("Code salon invalide", ic::ERROR_OUTLINE);
              }
            });
      }
      y += 80;
    } else {
      std::string code = wt::room_code();
      Rect card(x0, y, W - x0 - 60, 210);
      gfx::fill_rrect(card, 16, t.surface);
      text::draw(font::small_bold, card.x + 28, card.y + 24,
                 "SALON ACTIF — PARTAGE CE CODE", t.accent);
      text::draw(font::display, card.x + 28, card.y + 58, code, t.text);

      std::string media = wt::current_media_title();
      std::string status = media.empty()
                               ? "En attente d'une video... Lance un episode "
                                 "ou une video pour synchroniser."
                               : ("Synchronise sur : " + media);
      text::draw_wrapped(font::body,
                         Rect(card.x + 28, card.y + 132, card.w - 56, 60),
                         status, t.text2, 2);
      y += 236;

      if (button(id(g, "chat"), Rect(x0, y, 240, 52), "Envoyer un message",
                 ic::FORUM, BTN_PRIMARY, g, F_DEFAULT)) {
        prompt_text("Chat WatchTogether", "", "Ton message...",
                    [](std::string msg) { wt::send_chat(msg); });
      }
      if (button(id(g, "leave"), Rect(x0 + 256, y, 220, 52), "Quitter le salon",
                 ic::STOP, BTN_NORMAL, g)) {
        wt::leave_room();
        toast("Salon quitte", ic::INFO);
      }
      y += 68;

      // Quick reactions
      const char *reacts[] = {"LOL", "Incroyable !", "Pause stp", "GO !"};
      Id rg = id(g, "reacts");
      float rx = x0;
      for (int i = 0; i < 4; i++) {
        float cw =
            std::max(100.0f, text::measure(font::body_bold, reacts[i]) + 28.0f);
        if (chip(id(rg, (int64_t)i), Rect(rx, y, cw, 38), reacts[i], false,
                 rg)) {
          wt::send_chat(reacts[i]);
        }
        rx += cw + 10;
      }
      y += 54;

      auto msgs = wt::chat_history();
      if (!msgs.empty()) {
        text::draw(font::small_bold, x0, y, "CHAT DU SALON", t.accent);
        y += 30;
        size_t start = msgs.size() > 6 ? msgs.size() - 6 : 0;
        for (size_t i = start; i < msgs.size(); i++) {
          std::string line = msgs[i].sender + ": " + msgs[i].text;
          text::draw_fit(font::body, x0, y, W - x0 - 60, line, t.text);
          y += 30;
        }
      }
    }

    page_.end(y + page_.scroll());
    hint_bar({{"A", tr("Select")}, {"B", tr("Back")}});
  }

private:
  Page page_;
};

} // namespace

std::unique_ptr<app::Screen> make_watchtogether() {
  return std::make_unique<WatchTogetherScreen>();
}

void poll_watchtogether() {
  wt::SyncEvent ev;
  if (!wt::poll_event(ev))
    return;

  if (ev.is_chat) {
    ui::toast(util::fmt("[Chat] %s", ev.title.c_str()), ic::FORUM);
    return;
  }

  const player::Source &cur = player::source();
  std::string cur_id = cur.id.empty() ? cur.url : cur.id;
  bool need_open = (ev.id != cur_id && !ev.id.empty());

  wt::note_remote_applied();

    if (need_open) {
        ui::toast(util::fmt("WatchTogether: %s", ev.title.empty() ? ev.id.c_str() : ev.title.c_str()), ic::PEOPLE);
        if (ev.service == "youtube") {
            youtube::Video v;
            v.id = ev.id;
            v.title = ev.title;
            v.channel = ev.subtitle;
            yt::play(v);
        } else if (!ev.url.empty()) {
            // Direct resolved stream URL (bypasses Cloudflare on anime-sama.to for the friend!)
            player::Source s;
            s.url = ev.url;
            s.title = ev.title;
            s.subtitle = ev.subtitle;
            s.artwork = ev.image;
            s.service = ev.service;
            s.id = ev.id;
            s.extra = ev.extra;
            s.start = ev.pos;
            s.chunked_http = ev.chunked;
            s.user_agent = ev.user_agent.empty()
                               ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36"
                               : ev.user_agent;
            if (!ev.referer.empty()) s.headers.push_back({"Referer", ev.referer});
            if (ev.has_video) play_video(s);
            else play_audio(s);
        } else if (ev.service == "animesama") {
            play_video(animesama::source_from_resume(ev.id, ev.title, ev.subtitle, ev.image, ev.extra, ev.pos));
        }
        return;
    }

  if (!player::started())
    return;

  bool cur_paused = (player::state() == player::PAUSED);
  if (ev.paused != cur_paused) {
    player::set_paused(ev.paused);
  }
  double cur_pos = player::position();
  if (!player::live() && std::abs(ev.pos - cur_pos) > 2.2) {
    player::seek(ev.pos);
  }
}

} // namespace screens
