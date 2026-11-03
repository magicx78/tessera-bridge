#pragma once
#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::claude_sessions {

// What the plugin knows of the sessions: from the tile's data (the sessions sensor's attributes) or from the answer
// of tessera_claude.get_overview, whichever came last. Handles (`h`, `q`) are the integration's: short, never reused,
// the only thing the screen sends back.
struct SessionRow {
  char h[12] = "";
  char name[28] = "";
  char pc[28] = "";
  char state[8] = "idle";  // idle, work, ask, perm, lost, end
  int ctx = -1;            // percent, -1 unknown
  int asks = 0;
  char q[12] = "";         // the first open question's handle, "" for none
  char detail[100] = "";   // what it waits for on the PC
};

struct Overview {
  tessera::List<SessionRow> rows;
  int count = 0;           // sessions alive (also beyond the rows the lists carry)
  int open = 0;            // questions open
  int l5h = -1, lwk = -1;  // the account's windows, percent used, -1 unknown
  uint32_t l5h_at = 0, lwk_at = 0;
  bool valid = false;      // something was ever received
  uint32_t at = 0;         // when, by the screen's clock
};

struct Option {
  char id[8] = "";
  char label[52] = "";
  char desc[52] = "";
  bool chosen = false;
};

struct Part {
  char id[8] = "";
  char header[28] = "";
  tessera::List<tessera::Text> lines;
  bool multi = false;
  tessera::List<Option> options;
};

struct Question {
  char h[12] = "";
  int rev = 0;
  char session[12] = "";
  char name[28] = "";
  char status[12] = "";
  tessera::List<Part> parts;
  bool valid = false;
};

// What the popup shows: a new question (a tap opens it) or a session waiting for approval on the PC.
struct PopupView {
  bool question = true;
  char handle[12] = "";
  char name[28] = "";
  tessera::List<tessera::Text> lines;
};

class SessionsCard;
class QuestionCard;
class AskPopup;

// The plugin: a tile ("overview"), two cards ("sessions", "question"), a top bar item ("asks"), a popup for a new
// question or a permission hint, and two settings rows. Everything it asks of Home Assistant goes through
// tessera::send (the integration's services); everything it hears comes back in on_message.
class ClaudeSessions : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_interval(uint32_t now_ms) override;
  bool settings(tessera::SettingsPage &page) override;
  void on_message(JsonObjectConst message) override;
  void on_cards_closed() override;

  void set_event(text_sensor::TextSensor *s) { event_ = s; }
  void set_open(text_sensor::TextSensor *s) { open_ = s; }
  void set_rev(text_sensor::TextSensor *s) { rev_ = s; }
  void set_question_popup(switch_::Switch *s) { question_popup_ = s; }
  void set_hint_popup(switch_::Switch *s) { hint_popup_ = s; }

  // ---- The model, shared by the tile, the cards and the popup ----
  Overview &overview() { return overview_; }
  Question &question() { return question_; }
  PopupView &popup_view() { return popup_; }
  // The tile's data (the sensor's attributes) refreshes the overview too.
  void take_tile_data(JsonObjectConst data);

  // ---- Asking the integration (tessera::send); the answers land in on_message ----
  void request_overview();
  void request_question(const char *handle);
  void request_hint(const char *handle);
  // The answer to the question the model holds: option ids per part. The card hears the outcome in `answered`.
  bool send_answer();

  // ---- Opening things ----
  void open_question(const char *handle);
  // The handle the question card opens with (set before open_card).
  const char *pending_question() const { return pending_question_; }

  // The cards and the popup on the glass, to hand them what came in (each clears itself in its destructor).
  SessionsCard *sessions_card_ = nullptr;
  QuestionCard *question_card_ = nullptr;
  AskPopup *popup_shown_ = nullptr;

  int open_count() const;

 protected:
  void handle_event(const std::string &event);
  void show_popup_now();
  tessera::BarItem bar() const;
  uint32_t ask(const char *service, JsonDocument &data);

  text_sensor::TextSensor *event_{}, *open_{}, *rev_{};
  switch_::Switch *question_popup_{}, *hint_popup_{};
  Overview overview_;
  Question question_;
  PopupView popup_;
  char pending_question_[12] = "";
  tessera::Text seen_event_, seen_rev_;
  bool first_event_ = true, first_rev_ = true;
  uint32_t asked_overview_ = 0, asked_question_ = 0, asked_hint_ = 0, asked_answer_ = 0;
  uint32_t last_overview_ms_ = 0;
  bool overview_due_ = false;
  // What the pending question or hint request is for: the popup, or the card.
  bool question_for_popup_ = false;
};

}  // namespace esphome::claude_sessions
