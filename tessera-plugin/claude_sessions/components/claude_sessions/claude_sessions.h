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
  // Sessions working, asking or waiting for approval; since when the oldest of them is active (epoch, 0 none); when
  // the last activity was (epoch, 0 none).
  int active = 0;
  uint32_t active_since = 0, last_active = 0;
  bool valid = false;      // something was ever received
  uint32_t at = 0;         // when, by the screen's clock
};

// One background task, from the background sensor's attributes: a shell, an agent, a monitor or a workflow of a
// session, and when it started (epoch, and "HH:MM" as the house's clock reads it).
struct BackgroundRow {
  char title[52] = "";
  char kind[10] = "shell";
  char sess[28] = "";
  char hm[6] = "";
  uint32_t at = 0;
  // Steps the task reported through the mod: -1 and 0 while unknown.
  int done = -1, total = 0;
  // An agent's tool calls so far (integration 0.2.6): -1 while unknown, for a shell, or from an older integration.
  int calls = -1;
  // What the task said last (only in the get_background answer; "" when none).
  char note[124] = "";
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
class BackgroundCard;
class QuestionCard;
class AskPopup;

// The plugin: four tiles (overview, activity, limits, background), three cards ("sessions", "background", "question"), a top bar item ("asks"), a popup for a new
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
  void set_event_rev(text_sensor::TextSensor *s) { event_rev_ = s; }
  void set_open(text_sensor::TextSensor *s) { open_ = s; }
  void set_rev(text_sensor::TextSensor *s) { rev_ = s; }
  void set_question_popup(switch_::Switch *s) { question_popup_ = s; }
  void set_hint_popup(switch_::Switch *s) { hint_popup_ = s; }
  void set_notify(text_sensor::TextSensor *s) { notify_ = s; }

  // ---- The model, shared by the tile, the cards and the popup ----
  Overview &overview() { return overview_; }
  Question &question() { return question_; }
  PopupView &popup_view() { return popup_; }
  // The tile's data (the sensor's attributes) refreshes the overview too.
  void take_tile_data(JsonObjectConst data);
  // The background tasks for the background card: from the sensor's lists, then with their notes from get_background.
  const tessera::List<BackgroundRow> &background() const { return background_; }
  bool background_valid() const { return background_valid_; }
  // The sensor's rows; notes of rows that stay are kept until the answer to get_background brings them anew.
  void take_background(JsonObjectConst attributes);

  // ---- Asking the integration (tessera::send); the answers land in on_message ----
  void request_overview();
  void request_background();
  // Empties the background list (the card does it on opening, before the notes come anew).
  void clear_background();
  void request_question(const char *handle);
  void request_hint(const char *handle);
  // The announcement on a question or hint (speech, tone, none), kept by the integration: shown at once, the sensor's
  // attribute catches up a moment later.
  void request_notify(const char *mode);
  int notify_index() const;
  // The answer to the question the model holds: option ids per part. The card hears the outcome in `answered`.
  bool send_answer();

  // ---- Opening things ----
  void open_question(const char *handle);
  // The handle the question card opens with (set before open_card).
  const char *pending_question() const { return pending_question_; }

  // The cards and the popup on the glass, to hand them what came in (each clears itself in its destructor).
  SessionsCard *sessions_card_ = nullptr;
  BackgroundCard *background_card_ = nullptr;
  QuestionCard *question_card_ = nullptr;
  AskPopup *popup_shown_ = nullptr;

  int open_count() const;

 protected:
  void handle_event(const std::string &event);
  // Sends the request of `pending_event_` (a question for the popup, a hint); on_interval repeats it while no reply came.
  void send_event_request();
  void show_popup_now();
  // A popup switch changed (on the glass or in Home Assistant): off closes a popup of its kind and drops a request
  // still out, on shows what waits now.
  void popup_switch_changed(bool question, bool on);
  void remember_event(const std::string &event);
  tessera::BarItem bar() const;
  uint32_t ask(const char *service, JsonDocument &data);

  text_sensor::TextSensor *event_{}, *event_rev_{}, *open_{}, *rev_{};
  switch_::Switch *question_popup_{}, *hint_popup_{};
  text_sensor::TextSensor *notify_{};
  // The two switches as on_interval saw them last; the first pass only takes them.
  bool question_popup_was_ = false, hint_popup_was_ = false, popups_seen_ = false;
  // The mode the person chose on the glass, until the sensor's attribute says the same (or NOTIFY_PENDING_MS passed).
  uint32_t asked_notify_ = 0, pending_notify_ms_ = 0;
  char pending_notify_[8] = "";
  Overview overview_;
  // The background card's rows; asked again (spaced like the overview) when the sensor changed while it is open.
  tessera::List<BackgroundRow> background_;
  bool background_valid_ = false, background_due_ = false;
  uint32_t asked_background_ = 0, last_background_ms_ = 0;
  Question question_;
  PopupView popup_;
  char pending_question_[12] = "";
  tessera::Text seen_event_, seen_event_rev_, seen_rev_;
  // The newest event of each kind, so switching a popup on finds its own even after a later event of the other kind.
  std::string last_question_event_, last_hint_event_;
  bool event_seen_ = false, event_rev_seen_ = false;
  // When the event was last handled, by the screen's clock: the event and its counter arrive as two values, and the
  // second of a pair is no second event.
  uint32_t event_handled_ms_ = 0;
  bool first_rev_ = true;
  uint32_t asked_overview_ = 0, asked_question_ = 0, asked_hint_ = 0, asked_answer_ = 0;
  uint32_t last_overview_ms_ = 0;
  uint32_t last_question_ms_ = 0;
  // A popup asks again whether its question or hint still waits, when the registry changed: answered on another
  // screen or at the PC, it closes.
  uint32_t asked_check_ = 0, last_check_ms_ = 0;
  bool overview_due_ = false;
  // What the pending question or hint request is for: the popup, or the card.
  bool question_for_popup_ = false;
  // The event whose request the app has not answered yet (a request sent while the screen still waited for the app,
  // or a reply lost on the way, is repeated by on_interval), when it was sent last, and how often.
  std::string pending_event_;
  uint32_t pending_sent_ms_ = 0;
  uint8_t pending_tries_ = 0;
};

}  // namespace esphome::claude_sessions
