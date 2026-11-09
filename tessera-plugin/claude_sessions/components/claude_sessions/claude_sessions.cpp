#include "claude_sessions.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>

namespace esphome::claude_sessions {

static const char *const TAG = "claude_sessions";

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

static constexpr uint32_t ICON_ROBOT = 0xF06A9;
static constexpr uint32_t ICON_ROBOT_ASKS = 0xF169F;  // robot-confused
static constexpr uint32_t ICON_LOCK = 0xF08EE;        // lock-alert
static constexpr uint32_t ICON_PREV = 0xF0141;        // chevron-left
static constexpr uint32_t ICON_NEXT = 0xF0142;        // chevron-right
static constexpr uint32_t ICON_CHECK = 0xF012C;       // check
static constexpr uint32_t ICON_LAPTOP = 0xF0322;
static constexpr uint32_t ICON_TASKS = 0xF0996;     // progress-clock
static constexpr uint32_t ICON_SHELL = 0xF0379;     // monitor
static constexpr uint32_t ICON_WATCH = 0xF0208;     // eye
static constexpr uint32_t ICON_WORKFLOW = 0xF0493;  // cog

static constexpr int MAX_TILE_ROWS = 8;
static constexpr int MAX_TASK_ROWS = 10;
static constexpr int MAX_PAGE_ROWS = 8;
static constexpr int MAX_LINES = 6;
static constexpr int MAX_OPTIONS = 4;
static constexpr uint32_t OVERVIEW_SPACING_MS = 1500;
// A question or hint request without a reply is repeated after this long, this many times in all.
static constexpr uint32_t EVENT_RETRY_MS = 10000;
static constexpr uint8_t EVENT_RETRIES = 6;
// The event's text and its counter arrive one after the other; a counter that moves this soon after the text is
// the same event, later it is a new one with the same text (a second hint of the same session).
static constexpr uint32_t EVENT_PAIR_MS = 3000;
// The announcement modes of tessera_claude.set_notify, in the order of the settings row's words.
static const char *const NOTIFY_MODES[] = {"speech", "tone", "none"};
static constexpr int NOTIFY_COUNT = 3;
// How long the row shows the mode a tap chose before the sensor's attribute has to agree.
static constexpr uint32_t NOTIFY_PENDING_MS = 10000;

// ---- Small helpers ----

template <size_t N> static void copy(char (&to)[N], const char *from) {
  if (!from) from = "";
  std::strncpy(to, from, N - 1);
  to[N - 1] = '\0';
  // Never end in the middle of a UTF-8 character.
  size_t end = std::strlen(to);
  while (end > 0 && (static_cast<unsigned char>(to[end - 1]) & 0xC0) == 0x80) {
    size_t start = end - 1;
    while (start > 0 && (static_cast<unsigned char>(to[start]) & 0xC0) == 0x80) --start;
    const unsigned char lead = static_cast<unsigned char>(to[start]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (end - start >= need) break;
    to[start] = '\0';
    end = start;
  }
}

static std::string state_word(const tessera::Plugin *plugin, const char *state) {
  std::string key = "state_";
  key += state;
  const char *word = plugin->text(key.c_str());
  return word && *word ? word : plugin->text("state_idle");
}

// A state that wants the person's eye is written in ink, the rest muted: colours on small words read badly.
static theme::Role state_role(const char *state) {
  if (!std::strcmp(state, "ask") || !std::strcmp(state, "perm")) return theme::INK;
  if (!std::strcmp(state, "work")) return theme::INK_SOFT;
  return theme::MUTED;
}

static std::string percent_word(const tessera::Plugin *plugin, int percent) {
  if (percent < 0) return plugin->text("limit_unknown");
  return std::to_string(percent) + "%";
}

static std::string ctx_word(const tessera::Plugin *plugin, int percent) {
  if (percent < 0) return plugin->text("ctx_unknown");
  return tessera::format(plugin->text("ctx"), percent);
}

// "5h 44% · week 12%", with the reset of the five-hour window when the clock knows it.
static std::string limits_line(const tessera::Plugin *plugin, const Overview &o, uint32_t epoch) {
  if (!o.valid) return "";
  std::string line = tessera::fill(
      tessera::fill(plugin->text("limits"), "a", percent_word(plugin, o.l5h)).c_str(), "b", percent_word(plugin, o.lwk));
  if (o.l5h_at && epoch && o.l5h_at > epoch) {
    const uint32_t left = o.l5h_at - epoch;
    char when[16];
    // Cast: uint32_t is unsigned long on the ESP32-P4 toolchain, so "%u" would warn there.
    if (left >= 3600)
      std::snprintf(when, sizeof(when), "%uh %02um", (unsigned) (left / 3600), (unsigned) ((left % 3600) / 60));
    else
      std::snprintf(when, sizeof(when), "%um", (unsigned) (left / 60));
    line += " · ";
    line += tessera::fill(plugin->text("reset_in"), "n", when);
  }
  return line;
}

static void hide(lv_obj_t *obj, bool hidden) {
  if (!obj) return;
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static bool inside(lv_obj_t *obj, lv_obj_t *room, const tessera::Point &p) {
  if (!obj || !room || p.x < 0) return false;
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
  lv_area_t a, r;
  lv_obj_get_coords(obj, &a);
  lv_obj_get_coords(room, &r);
  const int x = p.x + r.x1, y = p.y + r.y1;
  return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

// Working, asking or waiting for approval: what the activity tile counts (the registry's ACTIVE_STATES).
static bool is_active(const char *state) {
  return !std::strcmp(state, "work") || !std::strcmp(state, "ask") || !std::strcmp(state, "perm");
}

// "12 min", "2 h 05 min", "3 d 4 h": a span of seconds, as the activity tile writes it.
static std::string span_word(const tessera::Plugin *plugin, uint32_t seconds) {
  if (seconds < 60) return plugin->text("span_now");
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) return tessera::format(plugin->text("span_min"), static_cast<int>(minutes));
  if (minutes < 24 * 60) {
    char mins[4];
    std::snprintf(mins, sizeof(mins), "%02u", (unsigned) (minutes % 60));
    return tessera::fill(tessera::fill(plugin->text("span_hm"), "a", std::to_string(minutes / 60)).c_str(), "b", mins);
  }
  return tessera::fill(tessera::fill(plugin->text("span_dh"), "a", std::to_string(minutes / 1440)).c_str(), "b",
                       std::to_string((minutes % 1440) / 60));
}

// ---- Parsing what Home Assistant sends ----

static void parse_limits(Overview &o, JsonObjectConst kinds) {
  if (kinds.isNull()) return;
  JsonObjectConst five = kinds["five_hour"];
  JsonObjectConst week = kinds["seven_day"];
  if (!five.isNull()) o.l5h = five["percent"] | -1;
  if (!week.isNull()) o.lwk = week["percent"] | -1;
}

void ClaudeSessions::take_tile_data(JsonObjectConst data) {
  JsonObjectConst a = data["attributes"];
  if (a.isNull()) return;
  Overview &o = overview_;
  o.rows.clear();
  JsonArrayConst h = a["h"], names = a["names"], states = a["states"], ctx = a["ctx"], asks = a["asks"], q = a["q"];
  const size_t n = h.size();
  for (size_t i = 0; i < n; ++i) {
    SessionRow row;
    copy(row.h, h[i] | "");
    copy(row.name, i < names.size() ? (names[i] | "") : "");
    copy(row.state, i < states.size() ? (states[i] | "idle") : "idle");
    row.ctx = i < ctx.size() ? (ctx[i] | -1) : -1;
    row.asks = i < asks.size() ? (asks[i] | 0) : 0;
    copy(row.q, i < q.size() ? (q[i] | "") : "");
    o.rows.push_back(row);
  }
  o.count = std::atoi(data["state"] | "0");
  o.open = a["open"] | 0;
  o.l5h = a["l5h"] | -1;
  o.lwk = a["lwk"] | -1;
  o.l5h_at = a["l5h_at"] | 0u;
  o.lwk_at = a["lwk_at"] | 0u;
  o.active = a["active"] | 0;
  o.active_since = a["active_since"] | 0u;
  o.last_active = a["last_active"] | 0u;
  o.valid = true;
  o.at = tessera::epoch();
  ESP_LOGD(TAG, "Tile data: %d sessions, %d open, rev %d, limits 5h %d%% week %d%%", o.count, o.open,
           (int) (a["rev"] | 0), o.l5h, o.lwk);
}

static void parse_overview(Overview &o, JsonObjectConst result) {
  o.rows.clear();
  int open = 0;
  for (JsonObjectConst s : result["sessions"].as<JsonArrayConst>()) {
    SessionRow row;
    copy(row.h, s["h"] | "");
    copy(row.name, s["name"] | "");
    copy(row.pc, s["pc"] | "");
    copy(row.state, s["state"] | "idle");
    row.ctx = s["ctx"] | -1;
    row.asks = s["asks"] | 0;
    copy(row.q, s["q"] | "");
    copy(row.detail, s["detail"] | "");
    open += row.asks;
    if (std::strcmp(row.state, "end") != 0) o.rows.push_back(row);
  }
  o.count = static_cast<int>(o.rows.size());
  o.open = open;
  o.active = static_cast<int>(
      std::count_if(o.rows.begin(), o.rows.end(), [](const SessionRow &r) { return is_active(r.state); }));
  parse_limits(o, result["limits"]["kinds"]);
  o.valid = true;
  o.at = tessera::epoch();
}

static void parse_question(Question &q, JsonObjectConst result) {
  q.parts.clear();
  copy(q.h, result["h"] | "");
  q.rev = result["rev"] | 0;
  copy(q.session, result["session"] | "");
  copy(q.name, result["name"] | "");
  copy(q.status, result["status"] | "unknown");
  for (JsonObjectConst p : result["parts"].as<JsonArrayConst>()) {
    Part part;
    copy(part.id, p["id"] | "");
    copy(part.header, p["header"] | "");
    part.multi = p["multi"] | false;
    for (JsonVariantConst line : p["lines"].as<JsonArrayConst>()) {
      if (part.lines.size() >= MAX_LINES) break;
      part.lines.push_back(roomy::text(std::string(line | "")));
    }
    if (part.lines.empty()) part.lines.push_back(roomy::text(std::string(p["text"] | "")));
    for (JsonObjectConst o : p["options"].as<JsonArrayConst>()) {
      if (part.options.size() >= MAX_OPTIONS) break;
      Option option;
      copy(option.id, o["id"] | "");
      copy(option.label, o["label"] | "");
      copy(option.desc, o["description"] | "");
      part.options.push_back(option);
    }
    q.parts.push_back(part);
  }
  q.valid = !q.parts.empty();
}

// ---- The tile: the sessions and their states, the open questions, the limits ----

class OverviewTile : public tessera::Tile {
 public:
  explicit OverviewTile(ClaudeSessions *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    columns_ = c.columns;
    rows_ = c.rows;
    entity_ = c.entity ? c.entity : "";
    tile_ = c.tile;
    name_ = c.name && *c.name ? c.name : plugin_->text("title");
    const bool large = ui::large();
    gap_ = ui::px(large ? 6 : 4);
    title_ = ui::label(c.parent, Font::TITLE, theme::INK);
    lv_obj_set_pos(title_, 0, 0);
    // The badge with the open questions, on the title's line at the right.
    badge_h_ = ui::line_height(Font::BODY) + ui::px(large ? 6 : 4);
    badge_ = ui::block(c.parent, theme::ACCENT);
    lv_obj_set_style_radius(badge_, LV_RADIUS_CIRCLE, 0);
    badge_text_ = ui::label(badge_, Font::BODY, theme::ON_ACCENT);
    lv_obj_set_style_text_align(badge_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(badge_text_);
    head_ = std::max(ui::line_height(Font::TITLE), badge_h_);
    big_ = ui::label(c.parent, Font::VALUE, theme::INK);
    lv_obj_set_width(big_, width_);
    lv_obj_set_style_text_align(big_, LV_TEXT_ALIGN_CENTER, 0);
    sub_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(sub_, width_);
    lv_obj_set_style_text_align(sub_, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < MAX_TILE_ROWS; ++i) {
      row_name_[i] = ui::label(c.parent, Font::BODY_LARGE, theme::INK);
      row_state_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(row_state_[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    limits_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(limits_, width_);
    lv_obj_set_pos(limits_, 0, height_ - ui::line_height(Font::BODY));
  }

  void on_state(JsonObjectConst data) override {
    wait_ = data["wait"] | "";
    // A tile without an entity chosen gets no "state": it says what to choose, whatever other tiles know.
    const bool has = !data["state"].isNull();
    if (has) plugin_->take_tile_data(data);
    has_entity_ = has;
    paint();
  }

  void on_tick(uint32_t) override { paint(); }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    ui::set_color(big_, theme::INK);
    ui::set_color(sub_, theme::MUTED);
    ui::set_color(limits_, theme::MUTED);
    for (int i = 0; i < MAX_TILE_ROWS; ++i) ui::set_color(row_name_[i], theme::INK);
    paint();
  }

  void on_tap() override { tessera::open_card(plugin_->plugin_id(), "sessions", roomy::plain(entity_), tile_); }

 private:
  void paint() {
    const Overview &o = plugin_->overview();
    ui::set_text(title_, roomy::plain(name_));
    const bool compact = rows_ <= 1 && columns_ <= 1;
    // The badge: how many questions wait.
    const bool asks = o.valid && o.open > 0;
    hide(badge_, !asks);
    if (asks) {
      const std::string count = std::to_string(o.open);
      const int w = std::max(badge_h_, ui::text_width(count, Font::BODY) + ui::px(10));
      lv_obj_set_size(badge_, w, badge_h_);
      lv_obj_set_pos(badge_, width_ - w, (head_ - badge_h_) / 2);
      ui::set_text(badge_text_, count);
    }
    lv_obj_set_width(title_, std::max(1, width_ - (asks ? badge_h_ * 2 : 0)));
    lv_obj_set_pos(title_, 0, (head_ - ui::line_height(Font::TITLE)) / 2);

    if (!has_entity_ || !o.valid) {
      // Nothing to show yet: say what to do.
      hide(big_, true);
      hide(limits_, true);
      for (int i = 0; i < MAX_TILE_ROWS; ++i) { hide(row_name_[i], true); hide(row_state_[i], true); }
      hide(sub_, false);
      lv_obj_set_style_text_align(sub_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_pos(sub_, 0, head_ + gap_);
      ui::set_text(sub_, plugin_->text(wait_ == "wrong_entity" || !has_entity_ ? "choose_sensor" : "loading"));
      return;
    }

    const int body_h = ui::line_height(Font::BODY);
    if (compact) {
      // A number of sessions, and the questions under it.
      for (int i = 0; i < MAX_TILE_ROWS; ++i) { hide(row_name_[i], true); hide(row_state_[i], true); }
      hide(limits_, true);
      hide(big_, false);
      hide(sub_, false);
      const std::string big = o.count > 0 ? std::to_string(o.count) : plugin_->text("none");
      const int room = height_ - head_;
      Font face = Font::BODY_LARGE;
      for (Font f : {Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
        face = f;
        if (ui::text_width(big, f) <= width_ && ui::line_height(f) + body_h <= room) break;
      }
      ui::set_font(big_, face);
      ui::set_text(big_, big);
      const int big_h = ui::line_height(face);
      const int top = head_ + std::max(0, (room - big_h - body_h) / 2);
      lv_obj_set_pos(big_, 0, top);
      lv_obj_set_height(big_, big_h);
      lv_obj_set_style_text_align(sub_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(sub_, 0, top + big_h);
      ui::set_text(sub_, o.count > 0 ? tessera::format(plugin_->text("questions"), o.open)
                                      : tessera::format(plugin_->text("sessions"), 0));
      ui::set_color(sub_, o.open > 0 ? theme::INK : theme::MUTED);
      return;
    }

    // A row per session, as many as fit, and the limits on the last line when there is room for them.
    hide(big_, true);
    const std::string limits = rows_ >= 2 ? limits_line(plugin_, o, tessera::epoch()) : "";
    hide(limits_, limits.empty());
    ui::set_text(limits_, limits);
    const int bottom = limits.empty() ? height_ : height_ - body_h - gap_;
    const int row_h = ui::line_height(Font::BODY_LARGE);
    const int step = row_h + gap_;
    int y = head_ + gap_;
    const int fit = std::max(0, (bottom - y + gap_) / step);
    int shown = 0;
    if (o.rows.empty()) {
      hide(sub_, false);
      lv_obj_set_style_text_align(sub_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_pos(sub_, 0, y);
      ui::set_text(sub_, plugin_->text("none"));
      ui::set_color(sub_, theme::MUTED);
    } else {
      hide(sub_, true);
    }
    for (int i = 0; i < MAX_TILE_ROWS; ++i) {
      const bool show = i < static_cast<int>(o.rows.size()) && shown < fit;
      hide(row_name_[i], !show);
      hide(row_state_[i], !show);
      if (!show) continue;
      const SessionRow &row = o.rows[i];
      std::string state = state_word(plugin_, row.state);
      if (row.asks > 1) state = std::to_string(row.asks) + " · " + state;
      // Every session's context fill, idle ones too: on a tile two or more columns wide there is room for it.
      if (columns_ >= 2 && row.ctx >= 0) state = ctx_word(plugin_, row.ctx) + " · " + state;
      const int state_w = std::min(width_ / 2, ui::text_width(state, Font::BODY) + ui::px(4));
      lv_obj_set_size(row_state_[i], state_w, body_h);
      lv_obj_set_pos(row_state_[i], width_ - state_w, y + (row_h - body_h) / 2);
      ui::set_text(row_state_[i], state);
      ui::set_color(row_state_[i], state_role(row.state));
      lv_obj_set_size(row_name_[i], std::max(1, width_ - state_w - ui::px(6)), row_h);
      lv_obj_set_pos(row_name_[i], 0, y);
      ui::set_text(row_name_[i], row.name);
      y += step;
      shown += 1;
    }
  }

  ClaudeSessions *plugin_;
  tessera::Text entity_, name_, wait_;
  int width_ = 0, height_ = 0, columns_ = 1, rows_ = 1, tile_ = -1, head_ = 0, gap_ = 0, badge_h_ = 0;
  bool has_entity_ = false;
  lv_obj_t *title_{}, *badge_{}, *badge_text_{}, *big_{}, *sub_{}, *limits_{};
  lv_obj_t *row_name_[MAX_TILE_ROWS]{}, *row_state_[MAX_TILE_ROWS]{};
};

// ---- The activity tile's motion: a small picture that works, sleeps and waits, one step a second ----
//
// Made of blocks and labels only (no arc, line or transform, which not every screen's LVGL has), moved by the tile's
// on_tick: a tile has no faster clock of its own. Three kinds; the person picks one in the tile's options.

enum class MotionKind : uint8_t { NONE, MOSAIC, CHOMPER, SPARK };
enum class Mood : uint8_t { REST, WORK, WAIT, ASK };

static MotionKind motion_kind(const char *value) {
  if (!value) return MotionKind::MOSAIC;
  if (!std::strcmp(value, "none")) return MotionKind::NONE;
  if (!std::strcmp(value, "chomper")) return MotionKind::CHOMPER;
  if (!std::strcmp(value, "spark")) return MotionKind::SPARK;
  return MotionKind::MOSAIC;
}

class TileMotion {
 public:
  static constexpr int STONES = 5;                 // the muncher's row
  static constexpr int ARMS = 8, DOTS = 3;         // the spark
  static constexpr int LANE = STONES + 3;          // one muncher lane: body, mouth, eye and its stones
  static constexpr int SPARK = 1 + ARMS * DOTS;    // one spark: its core and its dots
  static constexpr int CLASSIC = 15;               // the mosaic of a small room: 5 x 3 stones
  // The most a room can ask: the mosaic 7 x 8 stones, 2 muncher lanes, 2 sparks. The tile makes only what its biggest
  // room needs, so a small screen keeps few parts and the rest of this stays unused.
  static constexpr int MAX_COLS = 7, MAX_ROWS = 8, MAX_LANES = 2, MAX_SPARKS = 2;
  static constexpr int MAX_PARTS = MAX_COLS * MAX_ROWS;
  static_assert(MAX_LANES * LANE <= MAX_PARTS && MAX_SPARKS * SPARK <= MAX_PARTS && CLASSIC <= MAX_PARTS,
                "every motion's most parts fit one list");

  // `wide`: the tile has the right half under its head for the picture, and `max_w` x `max_h` is the most that room
  // can be (the tile's own size, with nothing grounded). Narrow tiles pass 0 x 0 and keep the small picture.
  void create(lv_obj_t *parent, MotionKind kind, const tessera::Plugin *plugin, bool wide, int max_w, int max_h) {
    kind_ = kind;
    if (kind_ == MotionKind::NONE) return;
    room_ = lv_obj_create(parent);
    lv_obj_remove_style_all(room_);
    lv_obj_remove_flag(room_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(room_, LV_OBJ_FLAG_SCROLLABLE);
    sleep_ = plugin->text("sleep_mark");
    ask_ = plugin->text("ask_mark");
    // Every part the biggest room can use, made now (objects are made only here) and hidden by place() when unused.
    if (kind_ == MotionKind::MOSAIC) {
      const Grid g = grid(max_w, max_h, wide, MAX_PARTS);
      count_ = std::max(CLASSIC, g.cols * g.rows);
      for (int i = 0; i < count_; ++i) part_[i] = dot(theme::TRACK, false);
    } else if (kind_ == MotionKind::CHOMPER) {
      count_ = lanes(max_w, max_h, wide) * LANE;
      for (int l = 0; l < count_ / LANE; ++l) {
        // The body, its mouth (a bite of the tile's own ground), its eye; the stones last, so the one in its mouth shows.
        part_[l * LANE] = dot(theme::ACCENT, true);
        part_[l * LANE + 1] = dot(theme::CARD, true);
        part_[l * LANE + 2] = dot(theme::CARD, true);
        for (int i = 0; i < STONES; ++i) part_[l * LANE + 3 + i] = dot(theme::ACCENT, false);
      }
    } else {
      count_ = sparks(max_w, max_h, wide) * SPARK;
      for (int i = 0; i < count_; ++i) part_[i] = dot(theme::ACCENT, true);  // each spark's core, then its arms' dots
    }
    // A sleeping "z z z", and the "?" of a question.
    mark_ = ui::label(room_, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(mark_, LV_TEXT_ALIGN_LEFT, 0);
    hide(mark_, true);
    hide(room_, true);
  }

  bool on() const { return kind_ != MotionKind::NONE; }

  // Where the picture stands on the tile; nothing when the room is too small for it. `whole`: a wide tile's right
  // half, which the picture fills (more rows, lanes or sparks as the room is tall); else the small picture of a
  // narrow tile.
  void place(int x, int y, int w, int h, bool whole) {
    if (!room_) return;
    const bool fits = w >= ui::px(28) && h >= ui::px(18);
    hide(room_, !fits);
    if (!fits) {
      w_ = h_ = 0;
      return;
    }
    lv_obj_set_pos(room_, x, y);
    if (w == w_ && h == h_ && whole == whole_) return;
    w_ = w;
    h_ = h;
    whole_ = whole;
    lv_obj_set_size(room_, w, h);
    layout();
  }

  // Paint one step. `frame` counts the tile's ticks; `ground` says the tile is red (a question), so the parts go white.
  void paint(Mood mood, uint32_t frame, bool ground) {
    if (!room_ || !w_) return;
    mood_ = mood;
    frame_ = frame;
    ground_ = ground;
    if (kind_ == MotionKind::MOSAIC) mosaic();
    else if (kind_ == MotionKind::CHOMPER) chomper();
    else spark();
  }

 private:
  // The mosaic's grid: square stones, `size` across, a `gap` apart at least. In a tall room (`spread`) as many rows
  // as the room holds and as many columns as then fit, the stones spread so the grid fills the room to its edges;
  // else the classic 5 x 3, as big as the room allows, in the middle.
  struct Grid {
    int cols = 5, rows = 3, size = 2, gap = 2;
    bool spread = false;
  };

  static Grid grid(int w, int h, bool whole, int max_cells) {
    Grid m;
    const int g = std::max(2, ui::px(3));
    const int s5 = (w - g * 4) / 5;  // a stone when five of them stand across
    m.gap = g;
    m.size = std::max(2, std::min(s5, (h - g * 2) / 3));
    if (!whole || s5 < 2) return m;
    const int unit = s5 + g;
    if ((h + g) * 2 < unit * 7) return m;  // not even three and a half rows of those stones: the small picture
    int rows = std::min(MAX_ROWS, ((h + g) * 2 + unit) / (unit * 2)), cols = 5, size = 2;
    for (;;) {
      const int size_h = (h - (rows - 1) * g) / rows;
      cols = std::max(3, std::min(MAX_COLS, ((w + g) * 2 + size_h + g) / ((size_h + g) * 2)));
      size = std::min(size_h, (w - (cols - 1) * g) / cols);
      if (cols * rows <= max_cells || rows <= 3) break;
      --rows;
    }
    if (cols * rows > max_cells || size < 2) return m;
    m.cols = cols;
    m.rows = rows;
    m.size = size;
    m.spread = true;
    return m;
  }

  // How many muncher lanes the room holds (one in a low room), and how many sparks (one unless it is taller than wide).
  static int lanes(int w, int h, bool whole) {
    if (!whole || w <= 0) return 1;
    const int lane = std::max(1, w * 2 / (2 * STONES + 3) * 13 / 10);  // a muncher and some air above and below
    return std::max(1, std::min(MAX_LANES, (h * 2 + lane) / (lane * 2)));
  }

  static int sparks(int w, int h, bool whole) {
    if (!whole || w <= 0) return 1;
    return std::max(1, std::min(MAX_SPARKS, (h * 2 + w) / (w * 2)));
  }

  // Stone i of n along a room: the first at one edge, the last at the other.
  static int spot(int i, int n, int room, int size) { return n > 1 ? i * (room - size) / (n - 1) : (room - size) / 2; }

  lv_obj_t *dot(theme::Role role, bool round) {
    lv_obj_t *o = ui::block(room_, role);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    if (round) lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
  }

  static void box(lv_obj_t *o, int x, int y, int w, int h) {
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, std::max(1, w), std::max(1, h));
  }

  // A block's colour and how much of it shows: a tone, or white on the red ground.
  void fill(lv_obj_t *o, Tone tone, theme::Role normal, lv_opa_t opa = LV_OPA_COVER) {
    if (ground_) ui::set_fill(o, Tone::NORMAL, theme::ON_ACCENT);
    else ui::set_fill(o, tone, normal);
    if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(o, opa, 0);
  }

  // The tile's own ground under the picture: the card, or red while a question waits.
  void ground_fill(lv_obj_t *o) { ui::set_fill(o, ground_ ? Tone::ALERT : Tone::NORMAL, theme::CARD); }

  bool blink() const { return frame_ % 2 == 0; }

  void mark(const std::string &text, int x, int y, Font font, Tone tone, theme::Role normal) {
    hide(mark_, text.empty());
    if (text.empty()) return;
    ui::set_font(mark_, font);
    ui::set_text(mark_, text);
    if (ground_) ui::set_color(mark_, theme::ON_ACCENT);
    else ui::set_tone(mark_, tone, normal);
    lv_obj_set_size(mark_, std::max(1, ui::text_width(text, font) + ui::px(2)), ui::line_height(font));
    lv_obj_set_pos(mark_, x, y);
  }

  // "z", "z z", "z z z", nothing: four steps of sleep.
  std::string zzz() const {
    std::string out;
    for (uint32_t i = 0; i < frame_ % 4; ++i) {
      if (!out.empty()) out += ' ';
      out += sleep_.c_str();
    }
    return out;
  }

  // The parts this room uses stand where they belong; the rest of those made stay hidden.
  void layout() {
    if (kind_ == MotionKind::MOSAIC) {
      grid_ = grid(w_, h_, whole_, count_);
      const int n = grid_.cols * grid_.rows, g = grid_.gap, s = grid_.size;
      const int x0 = (w_ - (s * grid_.cols + g * (grid_.cols - 1))) / 2, y0 = (h_ - (s * grid_.rows + g * (grid_.rows - 1))) / 2;
      // A scattered order of the stones: a step through them that has no common divisor with their number.
      step_ = std::max(1, n * 5 / 8);
      while (std::gcd(step_, n) != 1) ++step_;
      for (int i = 0; i < count_; ++i) {
        hide(part_[i], i >= n);
        if (i >= n) continue;
        const int col = i % grid_.cols, row = i / grid_.cols;
        box(part_[i], grid_.spread ? spot(col, grid_.cols, w_, s) : x0 + col * (s + g),
            grid_.spread ? spot(row, grid_.rows, h_, s) : y0 + row * (s + g), s, s);
        lv_obj_set_style_radius(part_[i], std::max(1, s / 5), 0);
      }
    } else if (kind_ == MotionKind::CHOMPER) {
      lanes_ = std::max(1, std::min(lanes(w_, h_, whole_), count_ / LANE));
      // A pitch per stone; the body a little wider than a pitch, as high as its lane allows.
      pitch_ = std::min(w_ * 2 / (2 * STONES + 3), h_ / lanes_ * 10 / 11);
      size_ = std::max(3, pitch_ * 2 / 5);
      radius_ = std::max(4, pitch_ * 11 / 20);
      x0_ = (w_ - pitch_ * (STONES + 1)) / 2 + pitch_ / 2;
      for (int i = 0; i < count_; ++i) hide(part_[i], i >= lanes_ * LANE);
      for (int l = 0; l < lanes_; ++l) {
        const int cy = (2 * l + 1) * h_ / (2 * lanes_);
        for (int i = 0; i < STONES; ++i) {
          lv_obj_t *s = part_[l * LANE + 3 + i];
          box(s, x0_ + (i + 1) * pitch_ - size_ / 2, cy - size_ / 2, size_, size_);
          lv_obj_set_style_radius(s, std::max(1, size_ / 5), 0);
        }
      }
    } else {
      sparks_ = std::max(1, std::min(sparks(w_, h_, whole_), count_ / SPARK));
      // Round, as big as the room's narrower side allows; in a tall room the arms reach further up and down.
      const int cell = h_ / sparks_;
      radius_ = std::max(6, std::min(w_, cell) * 23 / 50);
      reach_ = whole_ ? std::max(radius_, cell * 23 / 50) : radius_;
      for (int i = 0; i < count_; ++i) hide(part_[i], i >= sparks_ * SPARK);
    }
  }

  // Building a mosaic: a stone a second in a scattered order, then it starts anew.
  void mosaic() {
    static const uint8_t ORDER[CLASSIC] = {7, 2, 12, 0, 9, 14, 4, 11, 1, 6, 13, 3, 10, 5, 8};
    static const uint8_t SHADE[CLASSIC] = {255, 170, 220, 140, 200, 255, 160, 230, 190, 150, 240, 180, 210, 165, 245};
    const int cols = grid_.cols, rows = grid_.rows, n = cols * rows;
    // Stone j of the building order, and a stone's shade.
    auto order = [&](int j) { return n == CLASSIC ? static_cast<int>(ORDER[j]) : j * step_ % n; };
    auto shade = [&](int i) { return static_cast<lv_opa_t>(n == CLASSIC ? SHADE[i] : 160 + i * 37 % 96); };
    bool lit[MAX_PARTS] = {};
    int filled = 0;
    if (mood_ == Mood::WORK) filled = std::min<int>(frame_ % (n + 3), n);
    else if (mood_ == Mood::WAIT) filled = n * 8 / CLASSIC;
    for (int j = 0; j < filled; ++j) lit[order(j)] = true;
    for (int i = 0; i < n; ++i) {
      lv_obj_t *c = part_[i];
      const int col = i % cols, row = i / cols;
      if (mood_ == Mood::ASK) {
        // A "!" of stones on the red ground, blinking: the stem down the middle, a gap, the dot at the foot.
        const bool middle = cols % 2 ? col == cols / 2 : col == cols / 2 - 1 || col == cols / 2;
        const bool mark = middle && (rows <= 3 || row <= rows - 3 || row == rows - 1);
        fill(c, Tone::NORMAL, theme::ON_ACCENT, mark ? (blink() ? LV_OPA_COVER : LV_OPA_50) : LV_OPA_20);
      } else if (lit[i]) {
        fill(c, Tone::ACCENT, theme::ACCENT, shade(i));
      } else if (mood_ == Mood::WAIT && i == order(filled)) {
        fill(c, blink() ? Tone::BUSY : Tone::NORMAL, theme::TRACK);  // the next stone waits for the approval
      } else if (mood_ == Mood::REST && row == rows / 2 && col == cols / 2 && (frame_ / 2) % 2 == 0) {
        fill(c, Tone::NORMAL, theme::MUTED, LV_OPA_60);  // a cursor glimmering now and then
      } else {
        fill(c, Tone::NORMAL, theme::TRACK);
      }
    }
    hide(mark_, true);
  }

  // The mosaic muncher: eats a stone a second, sleeps at rest, stands before a locked stone or a "?". In a tall room
  // several lanes of it, each a few steps behind the one above.
  void chomper() {
    const int with_mark = mood_ == Mood::ASK ? lanes_ / 2 : 0;  // the "?" over the middle lane, the "z z z" at the top
    bool marked = false;
    for (int l = 0; l < lanes_; ++l) marked = lane(l, l == with_mark) || marked;
    if (!marked) hide(mark_, true);
  }

  // One lane. Returns whether it set the mark.
  bool lane(int l, bool with_mark) {
    lv_obj_t **p = part_ + l * LANE;
    lv_obj_t *body = p[0], *mouth = p[1], *eye = p[2];
    const int y0 = (2 * l + 1) * h_ / (2 * lanes_);
    const uint32_t f = frame_ + static_cast<uint32_t>(l) * 3;
    int pos;  // the body's place in pitches: -1 (coming in) to STONES (gone)
    if (mood_ == Mood::WORK) pos = static_cast<int>(f % (STONES + 2)) - 1;
    else if (mood_ == Mood::REST) pos = -1;
    else pos = 0;
    const bool open = mood_ == Mood::WORK && f % 2 == 0;
    const bool asleep = mood_ == Mood::REST;
    int r = asleep ? radius_ * 4 / 5 : radius_;
    int cx = asleep ? r + ui::px(2) : x0_ + pos * pitch_, cy = asleep ? y0 + r / 5 : y0;
    // Asleep, or one of several lanes, the body stays inside the room (alone it may slide in from beyond its edge).
    if (asleep || lanes_ > 1) cx = std::max(cx, r);
    box(body, cx - r, cy - r, 2 * r, 2 * r);
    fill(body, asleep ? Tone::NORMAL : Tone::ACCENT, asleep ? theme::MUTED : theme::ACCENT);
    // The mouth: a round bite of the ground at its front.
    hide(mouth, !open);
    if (open) {
      const int b = r * 31 / 50;
      box(mouth, cx + r - b, cy - b, 2 * b, 2 * b);
      ground_fill(mouth);
    }
    // The eye: a dot, or a line while it sleeps.
    const int e = std::max(2, r / 4);
    if (asleep) box(eye, cx, cy - r / 2, r * 2 / 5, std::max(2, ui::px(2)));
    else box(eye, cx + r / 8, cy - r / 2 - e / 2, e, e);
    ground_fill(eye);
    // The stones: eaten up to the body; at rest all of them, dim.
    for (int i = 0; i < STONES; ++i) {
      lv_obj_t *s = p[3 + i];
      const bool eaten = !asleep && i < pos;
      const bool gate = (mood_ == Mood::WAIT || mood_ == Mood::ASK) && i == 0;  // the stone right before it
      hide(s, eaten || (mood_ == Mood::ASK && gate));
      if (asleep) fill(s, Tone::NORMAL, theme::MUTED, LV_OPA_50);
      else if (gate) fill(s, blink() ? Tone::BUSY : Tone::NORMAL, theme::TRACK);
      else fill(s, i % 2 ? Tone::ACCENT : Tone::NORMAL, i % 2 ? theme::ACCENT : theme::MUTED);
    }
    if (!with_mark) return false;
    if (asleep) {
      mark(zzz(), cx + r, std::max(0, cy - r - ui::line_height(Font::BODY) / 2), Font::BODY, Tone::NORMAL, theme::MUTED);
      return true;
    }
    if (mood_ == Mood::ASK) {
      const int h = ui::line_height(Font::TITLE);
      const std::string ask = ask_.c_str();
      const int x = x0_ + pitch_ - ui::text_width(ask, Font::TITLE) / 2;
      mark(ask, x, y0 - h / 2 - (blink() ? ui::px(3) : 0), Font::TITLE, Tone::NORMAL, theme::INK);
      return true;
    }
    return false;
  }

  // The spark: eight arms of three dots. At work a wave runs round it; at rest it breathes; waiting, one arm points.
  // In a tall room two of them, one above the other, each a few steps on.
  void spark() {
    const int with_mark = mood_ == Mood::ASK ? sparks_ / 2 : 0;  // the "?" by the lower spark of two, the "z z z" up top
    bool marked = false;
    for (int k = 0; k < sparks_; ++k) marked = spark_one(k, k == with_mark) || marked;
    if (!marked) hide(mark_, true);
  }

  // One spark. Returns whether it set the mark.
  bool spark_one(int k, bool with_mark) {
    static const int16_t COS[ARMS] = {1000, 707, 0, -707, -1000, -707, 0, 707};
    static const int16_t SIN[ARMS] = {0, 707, 1000, 707, 0, -707, -1000, -707};
    lv_obj_t **p = part_ + k * SPARK;
    const int R = radius_, RY = reach_, cx = w_ / 2, cy = (2 * k + 1) * h_ / (2 * sparks_);
    const int core = std::max(3, R * 3 / 10);
    box(p[0], cx - core / 2, cy - core / 2, core, core);
    const bool rest = mood_ == Mood::REST;
    fill(p[0], rest ? Tone::NORMAL : Tone::ACCENT, rest ? theme::MUTED : theme::ACCENT);
    const uint32_t f = frame_ + static_cast<uint32_t>(k) * 3;
    const int lead = static_cast<int>(f % ARMS);
    for (int a = 0; a < ARMS; ++a) {
      int reach;  // how many dots of this arm show
      Tone tone = Tone::ACCENT;
      theme::Role role = theme::ACCENT;
      if (mood_ == Mood::WORK) {
        const int behind = (lead - a + ARMS) % ARMS;
        reach = behind == 0 ? 3 : behind == 1 ? 2 : 1;
      } else if (mood_ == Mood::ASK) {
        reach = 3;
      } else if (mood_ == Mood::WAIT && a == 0) {
        reach = blink() ? 3 : 2;
        tone = blink() ? Tone::BUSY : Tone::NORMAL;
        role = theme::MUTED;
      } else {
        reach = rest && frame_ % 4 == 0 ? 2 : 1;
        tone = Tone::NORMAL;
        role = theme::MUTED;
      }
      for (int d = 0; d < DOTS; ++d) {
        lv_obj_t *o = p[1 + a * DOTS + d];
        hide(o, d >= reach);
        if (d >= reach) continue;
        const int dx = R * (3 + 3 * d) / 10, dy = RY * (3 + 3 * d) / 10, size = std::max(2, R * (26 - 4 * d) / 100);
        box(o, cx + dx * COS[a] / 1000 - size / 2, cy + dy * SIN[a] / 1000 - size / 2, size, size);
        fill(o, tone, role);
      }
    }
    if (!with_mark) return false;
    if (rest) {
      mark(zzz(), cx + R / 2, std::max(0, cy - RY - ui::line_height(Font::BODY) / 3), Font::BODY, Tone::NORMAL, theme::MUTED);
      return true;
    }
    if (mood_ == Mood::ASK) {
      const int h = ui::line_height(Font::TITLE);
      const std::string ask = ask_.c_str();
      mark(ask, std::min(w_ - ui::text_width(ask, Font::TITLE), cx + R + ui::px(2)),
           cy - RY / 2 - h / 2 - (blink() ? ui::px(3) : 0), Font::TITLE, Tone::NORMAL, theme::INK);
      return true;
    }
    return false;
  }

  MotionKind kind_ = MotionKind::NONE;
  Mood mood_ = Mood::REST;
  uint32_t frame_ = 0;
  bool ground_ = false, whole_ = false;
  int w_ = 0, h_ = 0, size_ = 0, x0_ = 0, pitch_ = 0, radius_ = 0, reach_ = 0;
  int count_ = 0;                            // the parts made
  int lanes_ = 1, sparks_ = 1, step_ = 1;    // what this room uses: muncher lanes, sparks, the mosaic's scatter step
  Grid grid_;
  tessera::Text sleep_, ask_;  // "z" and "?", from the translations
  lv_obj_t *room_{}, *mark_{};
  lv_obj_t *part_[MAX_PARTS]{};
};

// ---- The activity tile: whether Claude is at work, since when, in how many sessions, and whether one asks ----

class ActivityTile : public tessera::Tile {
 public:
  explicit ActivityTile(ClaudeSessions *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    columns_ = c.columns;
    rows_ = c.rows;
    entity_ = c.entity ? c.entity : "";
    tile_ = c.tile;
    name_ = c.name && *c.name ? c.name : plugin_->text("activity_title");
    const bool large = ui::large();
    gap_ = ui::px(large ? 6 : 4);
    pad_ = ui::px(large ? 8 : 6);
    // The ground: the whole tile red while a question waits, so it is seen from across the room.
    ground_ = ui::block(c.parent, theme::CARD);
    lv_obj_set_pos(ground_, 0, 0);
    lv_obj_set_size(ground_, width_, height_);
    lv_obj_set_style_radius(ground_, ui::px(large ? 12 : 8), 0);
    lv_obj_set_style_pad_all(ground_, 0, 0);
    lv_obj_set_style_bg_opa(ground_, LV_OPA_TRANSP, 0);
    // The parts stand on the ground, so their colours are read against it.
    body_ = lv_obj_create(ground_);
    lv_obj_remove_style_all(body_);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    // The head: the icon, the name, and at the right the badge with the open questions.
    icon_ = ui::label(body_, Font::ICON_SMALL, theme::MUTED);
    title_ = ui::label(body_, Font::TITLE, theme::INK);
    badge_h_ = ui::line_height(Font::BODY) + ui::px(large ? 6 : 4);
    badge_ = ui::block(body_, theme::ACCENT);
    lv_obj_set_style_radius(badge_, LV_RADIUS_CIRCLE, 0);
    badge_text_ = ui::label(badge_, Font::BODY, theme::ON_ACCENT);
    lv_obj_set_style_text_align(badge_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(badge_text_);
    head_ = std::max({ui::line_height(Font::TITLE), ui::line_height(Font::ICON_SMALL), badge_h_});
    big_ = ui::label(body_, Font::VALUE, theme::INK);
    since_ = ui::label(body_, Font::BODY_LARGE, theme::INK);
    of_ = ui::label(body_, Font::BODY, theme::MUTED);
    for (int i = 0; i < MAX_TILE_ROWS; ++i) {
      row_name_[i] = ui::label(body_, Font::BODY, theme::INK_SOFT);
      row_state_[i] = ui::label(body_, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(row_state_[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    // The motion the person picked (the manifest fills in the default); its parts only.
    // A wide tile gives it the right half under the head, down to its foot: it makes the parts of the tallest such room.
    const bool wide = columns_ >= 2;
    motion_.create(body_, motion_kind(c.options["motion"] | "mosaic"), plugin_, wide, wide ? width_ / 2 - gap_ : 0,
                   wide ? height_ - head_ - gap_ / 2 : 0);
  }

  void on_state(JsonObjectConst data) override {
    wait_ = data["wait"] | "";
    const bool has = !data["state"].isNull();
    if (has) plugin_->take_tile_data(data);
    has_entity_ = has;
    paint(tessera::epoch());
  }

  // Every second: the duration counts on, and the motion takes a step.
  void on_tick(uint32_t epoch) override {
    ++frame_;
    paint(epoch);
  }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    for (int i = 0; i < MAX_TILE_ROWS; ++i) ui::set_color(row_name_[i], theme::INK_SOFT);
    paint(tessera::epoch());
  }

  // A question waits: straight to it. Otherwise the sessions card.
  void on_tap() override {
    const Overview &o = plugin_->overview();
    if (o.valid && o.open > 0) {
      for (const SessionRow &row : o.rows) {
        if (row.q[0]) {
          plugin_->open_question(row.q);
          return;
        }
      }
    }
    tessera::open_card(plugin_->plugin_id(), "sessions", roomy::plain(entity_), tile_);
  }

 private:
  void paint(uint32_t epoch) {
    const Overview &o = plugin_->overview();
    ui::set_text(title_, roomy::plain(name_));
    const bool ready = has_entity_ && o.valid;
    bool waits = false;
    for (const SessionRow &row : o.rows) waits = waits || !std::strcmp(row.state, "perm");
    const bool asks = ready && o.open > 0;
    const bool busy = ready && o.active > 0;
    waits = waits && busy;
    // Red while a question waits, amber while an approval waits at the PC, the accent while it works, muted at rest.
    const Tone tone = asks ? Tone::ALERT : waits ? Tone::BUSY : busy ? Tone::ACCENT : Tone::NORMAL;
    grounded_ = asks;
    if (grounded_) ui::set_fill(ground_, Tone::ALERT, theme::CARD);
    lv_obj_set_style_bg_opa(ground_, grounded_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    // On the ground the parts keep off its edge: a little at the sides, at the top and foot only what the tile
    // spares beyond its head and two lines.
    const int spare = height_ - head_ - ui::line_height(Font::BODY_LARGE) - ui::line_height(Font::BODY) - gap_;
    const int in_x = grounded_ ? pad_ / 2 : 0, in_y = grounded_ ? std::max(0, std::min(pad_ / 2, spare / 2)) : 0;
    bw_ = std::max(1, width_ - 2 * in_x);
    bh_ = std::max(1, height_ - 2 * in_y);
    lv_obj_set_pos(body_, in_x, in_y);
    lv_obj_set_size(body_, bw_, bh_);
    ink(title_, Tone::NORMAL, theme::INK);
    ui::set_text(icon_, ui::icon(asks ? ICON_ROBOT_ASKS : waits ? ICON_LOCK : ICON_ROBOT));
    ink(icon_, tone, theme::MUTED);

    hide(badge_, !asks);
    if (asks) {
      const std::string count = std::to_string(o.open);
      const int w = std::max(badge_h_, ui::text_width(count, Font::BODY) + ui::px(10));
      lv_obj_set_size(badge_, w, badge_h_);
      lv_obj_set_pos(badge_, bw_ - w, (head_ - badge_h_) / 2);
      // On the red ground the badge turns round: light, its number red.
      ui::set_fill(badge_, grounded_ ? Tone::NORMAL : Tone::ALERT, grounded_ ? theme::ON_ACCENT : theme::ACCENT);
      if (grounded_) ui::set_tone(badge_text_, Tone::ALERT, theme::ON_ACCENT);
      else ui::set_color(badge_text_, theme::ON_ACCENT);
      ui::set_text(badge_text_, count);
    }
    const int icon_w = ui::line_height(Font::ICON_SMALL) + ui::px(4);
    lv_obj_set_pos(icon_, 0, (head_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_pos(title_, icon_w, (head_ - ui::line_height(Font::TITLE)) / 2);
    lv_obj_set_width(title_, std::max(1, bw_ - icon_w - (asks ? badge_h_ * 2 : 0)));

    for (int i = 0; i < MAX_TILE_ROWS; ++i) {
      hide(row_name_[i], true);
      hide(row_state_[i], true);
    }
    const int body_h = ui::line_height(Font::BODY);
    if (!ready) {
      // Nothing to show yet: say what to do.
      hide(big_, true);
      hide(since_, true);
      hide(of_, false);
      lv_obj_set_style_text_align(of_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_size(of_, bw_, body_h);
      lv_obj_set_pos(of_, 0, head_ + gap_);
      ink(of_, Tone::NORMAL, theme::MUTED);
      ui::set_text(of_, plugin_->text(wait_ == "wrong_entity" || !has_entity_ ? "choose_sensor" : "loading"));
      motion_.place(0, 0, 0, 0, false);
      return;
    }

    // The words: since when it works (or when it last did), and of how many sessions.
    std::string since;
    if (asks) {
      since = plugin_->text("activity_asks");
    } else if (waits) {
      since = plugin_->text("activity_waits");
    } else if (busy) {
      since = epoch && o.active_since && epoch >= o.active_since
                  ? tessera::fill(plugin_->text("active_for"), "n", span_word(plugin_, epoch - o.active_since))
                  : plugin_->text("activity_on");
    } else if (o.count > 0 && epoch && o.last_active && epoch >= o.last_active) {
      since = tessera::fill(plugin_->text("last_active"), "n", span_word(plugin_, epoch - o.last_active));
    } else {
      since = plugin_->text("activity_rest");
    }
    const std::string of = o.count > 0 ? tessera::format(plugin_->text("of_sessions"), o.count) : plugin_->text("none");
    const Tone word_tone = asks ? Tone::ALERT : waits ? Tone::BUSY : Tone::NORMAL;

    // The big number: how many sessions are active. On a tile two rows high the number and the words take the first
    // row, the active sessions' names the rest.
    const std::string big = std::to_string(o.active);
    const bool wide = columns_ >= 2;
    const int top_room = rows_ >= 2 ? std::max(2 * body_h, bh_ / rows_ - head_) : bh_ - head_;
    // The motion: the right half of a wide tile, under the head down to the bottom (the number, the words and the
    // names keep the left half); above the number on a narrow one.
    int anim_w = 0, anim_h = 0;
    if (motion_.on() && wide) {
      anim_w = bw_ / 2 - gap_;
      if (anim_w < ui::px(28)) anim_w = 0;
    } else if (motion_.on()) {
      anim_h = top_room * 2 / 5;
      if (anim_h - gap_ / 2 < ui::px(18)) anim_h = 0;
    }
    const int area = anim_w ? bw_ - anim_w - gap_ : bw_;  // what the number, the words and the names keep
    const int number_w = wide ? area * 2 / 5 : bw_;
    const int under = wide ? 0 : body_h;
    Font face = Font::BODY_LARGE;
    for (Font f : {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      face = f;
      if (f == Font::DIGITS && !ui::covers(big, f)) continue;
      if (ui::text_width(big, f) <= number_w && ui::line_height(f) + under <= top_room - anim_h) break;
    }
    const int big_h = ui::line_height(face);
    hide(big_, false);
    ui::set_font(big_, face);
    ui::set_text(big_, big);
    ink(big_, tone, busy ? theme::INK : theme::MUTED);
    hide(of_, false);

    int bottom;
    if (wide) {
      // Beside the number: the time on the first line, "of N sessions" under it.
      const int body_large_h = ui::line_height(Font::BODY_LARGE);
      const int text_x = std::min(area - 1, ui::text_width(big, face) + ui::px(12));
      const int text_w = std::max(1, area - text_x);
      const int words_h = body_large_h + body_h;
      const int block_h = std::max(big_h, words_h);
      const int top = head_ + std::max(0, (top_room - block_h) / 2);
      lv_obj_set_size(big_, text_x, big_h);
      lv_obj_set_style_text_align(big_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_pos(big_, 0, top + (block_h - big_h) / 2);
      hide(since_, false);
      lv_obj_set_size(since_, text_w, body_large_h);
      lv_obj_set_pos(since_, text_x, top + (block_h - words_h) / 2);
      ui::set_text(since_, since);
      ink(since_, word_tone, busy ? theme::INK : theme::MUTED);
      lv_obj_set_style_text_align(of_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_size(of_, text_w, body_h);
      lv_obj_set_pos(of_, text_x, top + (block_h - words_h) / 2 + body_large_h);
      ui::set_text(of_, of);
      ink(of_, Tone::NORMAL, theme::MUTED);
      bottom = top + block_h + gap_;
      // The right half, all the way down: two rows high the names stay at the left beside it.
      motion_.place(bw_ - anim_w, head_ + gap_ / 2, anim_w, bh_ - head_ - gap_ / 2, true);
    } else {
      // Under the motion the number, and under it one word of state.
      motion_.place(0, head_ + gap_ / 2, bw_, anim_h - gap_ / 2, false);
      const int top = head_ + anim_h + std::max(0, (top_room - anim_h - big_h - body_h) / 2);
      lv_obj_set_size(big_, bw_, big_h);
      lv_obj_set_style_text_align(big_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(big_, 0, top);
      hide(since_, true);
      lv_obj_set_style_text_align(of_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_size(of_, bw_, body_h);
      lv_obj_set_pos(of_, 0, top + big_h);
      ui::set_text(of_, asks || waits ? since
                        : busy        ? plugin_->text("activity_on")
                        : o.count > 0 ? plugin_->text("activity_rest")
                                      : plugin_->text("none"));
      ink(of_, word_tone, theme::MUTED);
      bottom = top + big_h + body_h + gap_;
    }
    motion_.paint(asks ? Mood::ASK : waits ? Mood::WAIT : busy ? Mood::WORK : Mood::REST, frame_, grounded_);

    // Two rows high or more: the names of the active sessions.
    if (rows_ < 2) return;
    const int step = body_h + gap_ / 2;
    int y = bottom, shown = 0;
    for (const SessionRow &row : o.rows) {
      if (shown >= MAX_TILE_ROWS || y + body_h > bh_) break;
      if (!is_active(row.state)) continue;
      const std::string state = state_word(plugin_, row.state);
      const int state_w = std::min(area / 2, ui::text_width(state, Font::BODY) + ui::px(4));
      lv_obj_t *name = row_name_[shown], *word = row_state_[shown];
      hide(name, false);
      hide(word, false);
      lv_obj_set_size(word, state_w, body_h);
      lv_obj_set_pos(word, area - state_w, y);
      ui::set_text(word, state);
      const Tone row_tone = !std::strcmp(row.state, "ask")    ? Tone::ALERT
                            : !std::strcmp(row.state, "perm") ? Tone::BUSY
                                                              : Tone::NORMAL;
      ink(word, row_tone, theme::MUTED);
      lv_obj_set_size(name, std::max(1, area - state_w - ui::px(6)), body_h);
      lv_obj_set_pos(name, 0, y);
      ui::set_text(name, row.name);
      ink(name, Tone::NORMAL, theme::INK_SOFT);
      y += step;
      shown += 1;
    }
  }

  // A label's colour: its tone, or white on the red ground.
  void ink(lv_obj_t *label, Tone tone, theme::Role normal) {
    if (grounded_) ui::set_color(label, theme::ON_ACCENT);
    else ui::set_tone(label, tone, normal);
  }

  ClaudeSessions *plugin_;
  tessera::Text entity_, name_, wait_;
  int width_ = 0, height_ = 0, columns_ = 1, rows_ = 1, tile_ = -1, head_ = 0, gap_ = 0, badge_h_ = 0;
  int pad_ = 0, bw_ = 1, bh_ = 1;
  bool has_entity_ = false, grounded_ = false;
  uint32_t frame_ = 0;
  TileMotion motion_;
  lv_obj_t *ground_{}, *body_{}, *icon_{}, *title_{}, *badge_{}, *badge_text_{}, *big_{}, *since_{}, *of_{};
  lv_obj_t *row_name_[MAX_TILE_ROWS]{}, *row_state_[MAX_TILE_ROWS]{};
};

// ---- The limits tile: the account's windows (five hours, the week, and any window of one model the sessions
// report, a Fable or Opus week one day), each a bar with a mark where an even pace would stand ----

static constexpr uint32_t ICON_GAUGE = 0xF029A;  // gauge
static constexpr int MAX_LIMIT_ROWS = 4;

// Which windows the tile shows (its option "window"): both bars and the model windows, or one window alone, large.
enum class LimitWindow : uint8_t { BOTH, FIVE, WEEK };

static LimitWindow limit_window(const char *value) {
  if (value && !std::strcmp(value, "five")) return LimitWindow::FIVE;
  if (value && !std::strcmp(value, "week")) return LimitWindow::WEEK;
  return LimitWindow::BOTH;
}

class LimitsTile : public tessera::Tile {
 public:
  explicit LimitsTile(ClaudeSessions *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    entity_ = c.entity ? c.entity : "";
    tile_ = c.tile;
    name_ = c.name && *c.name ? c.name : plugin_->text("limits_title");
    window_ = limit_window(c.options["window"] | "both");
    const bool large = ui::large();
    gap_ = ui::px(large ? 6 : 4);
    bar_h_ = std::max(4, ui::px(large ? 10 : 7));
    icon_ = ui::label(c.parent, Font::ICON_SMALL, theme::MUTED);
    title_ = ui::label(c.parent, Font::TITLE, theme::INK);
    head_ = std::max(ui::line_height(Font::TITLE), ui::line_height(Font::ICON_SMALL));
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    for (int i = 0; i < MAX_LIMIT_ROWS; ++i) {
      Row &r = row_[i];
      r.name = ui::label(c.parent, Font::BODY, theme::INK_SOFT);
      r.pct = ui::label(c.parent, Font::TITLE, theme::INK);
      // One window alone: its percent stands large at the left, as a meter tile's value does.
      lv_obj_set_style_text_align(r.pct, window_ != LimitWindow::BOTH && i == 0 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT,
                                  0);
      r.track = ui::block(c.parent, theme::TRACK);
      r.fill = ui::block(r.track, theme::ACCENT);
      r.pace = ui::block(c.parent, theme::INK);
      r.reset = ui::label(c.parent, Font::BODY, theme::MUTED);
      for (lv_obj_t *o : {r.track, r.fill, r.pace}) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
      }
    }
    // The reset in the head's top right corner, for one window alone.
    corner_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(corner_, LV_TEXT_ALIGN_RIGHT, 0);
    hide(corner_, true);
  }

  void on_state(JsonObjectConst data) override {
    wait_ = data["wait"] | "";
    has_entity_ = !data["state"].isNull();
    JsonObjectConst a = data["attributes"];
    if (has_entity_ && !a.isNull()) {
      // Only the limits: the other tiles of the same sensor keep the sessions model to themselves.
      l5h_ = a["l5h"] | -1;
      lwk_ = a["lwk"] | -1;
      l5h_at_ = a["l5h_at"] | 0u;
      lwk_at_ = a["lwk_at"] | 0u;
      JsonArrayConst names = a["lx_names"], lx = a["lx"];
      extra_ = 0;
      for (size_t i = 0; i < names.size() && extra_ < MAX_LIMIT_ROWS - 2; ++i) {
        copy(extra_name_[extra_], names[i] | "");
        extra_pct_[extra_] = i < lx.size() ? (lx[i] | -1) : -1;
        extra_ += 1;
      }
      valid_ = true;
    }
    paint(tessera::epoch());
  }

  // The resets count down: once a minute is enough.
  void on_tick(uint32_t epoch) override {
    if (epoch / 60 != painted_minute_) paint(epoch);
  }

  void on_theme() override { paint(tessera::epoch()); }

  void on_tap() override {
    tessera::open_card(plugin_->plugin_id(), "sessions", roomy::plain(entity_), tile_);
  }

 private:
  struct Row {
    lv_obj_t *name{}, *pct{}, *track{}, *fill{}, *pace{}, *reset{};
  };

  struct Window {
    std::string name;
    int pct;
    uint32_t at, span;
  };

  // The colour of a fill: the accent, amber from 70 %, red from 90 %.
  static Tone fill_tone(int percent) { return percent >= 90 ? Tone::ALERT : percent >= 70 ? Tone::BUSY : Tone::NORMAL; }

  // Whether the clock knows when the window starts anew.
  static bool timed(const Window &w, uint32_t epoch) {
    return w.span && w.at && epoch && w.at > epoch && w.at - epoch <= w.span;
  }

  // The number stays ink (amber text reads poorly on a light theme); red only near the end.
  static void ink_percent(lv_obj_t *label, int percent) {
    if (percent < 0) ui::set_color(label, theme::MUTED);
    else if (percent >= 90) ui::set_tone(label, Tone::ALERT, theme::INK);
    else ui::set_color(label, theme::INK);
  }

  // The bar, and the even pace: how much of the window has passed, so a fill beyond the mark spends ahead.
  void paint_bar(Row &r, const Window &w, int bar_y, uint32_t epoch) {
    hide(r.track, false);
    lv_obj_set_pos(r.track, 0, bar_y);
    lv_obj_set_size(r.track, width_, bar_h_);
    ui::set_fill(r.track, Tone::NORMAL, theme::TRACK);
    const int fill_w = w.pct > 0 ? std::max(bar_h_, width_ * std::min(w.pct, 100) / 100) : 0;
    hide(r.fill, fill_w == 0);
    if (fill_w) {
      lv_obj_set_pos(r.fill, 0, 0);
      lv_obj_set_size(r.fill, fill_w, bar_h_);
      ui::set_fill(r.fill, fill_tone(w.pct), theme::ACCENT);
    }
    const bool shown = timed(w, epoch);
    hide(r.pace, !shown);
    if (shown) {
      const uint32_t passed = w.span - (w.at - epoch);
      const int mark_w = std::max(2, ui::px(3)), over = std::max(2, ui::px(3));
      const int x = static_cast<int>(static_cast<uint64_t>(width_ - mark_w) * passed / w.span);
      lv_obj_set_pos(r.pace, x, bar_y - over);
      lv_obj_set_size(r.pace, mark_w, bar_h_ + 2 * over);
      ui::set_fill(r.pace, Tone::NORMAL, theme::INK);
      lv_obj_move_foreground(r.pace);
    }
  }

  void paint(uint32_t epoch) {
    painted_minute_ = epoch / 60;
    const bool ready = has_entity_ && valid_;
    ui::set_text(title_, roomy::plain(name_));
    ui::set_color(title_, theme::INK);
    ui::set_text(icon_, ui::icon(ICON_GAUGE));
    int worst = std::max(l5h_, lwk_);
    for (int i = 0; i < extra_; ++i) worst = std::max(worst, extra_pct_[i]);
    ui::set_tone(icon_, ready && worst >= 70 ? fill_tone(worst) : Tone::NORMAL, theme::MUTED);
    const int icon_w = ui::line_height(Font::ICON_SMALL) + ui::px(4);
    lv_obj_set_pos(icon_, 0, (head_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_pos(title_, icon_w, (head_ - ui::line_height(Font::TITLE)) / 2);
    lv_obj_set_width(title_, std::max(1, width_ - icon_w));
    hide(corner_, true);

    for (Row &r : row_)
      for (lv_obj_t *o : {r.name, r.pct, r.track, r.pace, r.reset}) hide(o, true);
    if (!ready) {
      hide(note_, false);
      lv_obj_set_size(note_, width_, ui::line_height(Font::BODY));
      lv_obj_set_pos(note_, 0, head_ + gap_);
      ui::set_color(note_, theme::MUTED);
      ui::set_text(note_, plugin_->text(wait_ == "wrong_entity" || !has_entity_ ? "choose_sensor" : "loading"));
      return;
    }
    hide(note_, true);

    // The windows: five hours and the week always, then the model windows the sessions reported.
    Window win[MAX_LIMIT_ROWS];
    int n = 0;
    win[n++] = {plugin_->text("limit_5h"), l5h_, l5h_at_, 5 * 3600};
    win[n++] = {plugin_->text("limit_week"), lwk_, lwk_at_, 7 * 86400};
    for (int i = 0; i < extra_ && n < MAX_LIMIT_ROWS; ++i)
      win[n++] = {tessera::fill(plugin_->text("limit_model"), "n", extra_name_[i]), extra_pct_[i], 0, 0};
    if (window_ != LimitWindow::BOTH) {
      paint_single(win[window_ == LimitWindow::FIVE ? 0 : 1], epoch);
      return;
    }

    // As many as the tile holds; with the reset under each bar when there is room for it.
    const int body_h = ui::line_height(Font::BODY), line_h = std::max(body_h, ui::line_height(Font::TITLE));
    const int room = height_ - head_ - gap_;
    const int short_h = line_h + gap_ / 2 + bar_h_ + gap_, full_h = short_h + body_h;
    const int shown = std::max(1, std::min(n, room / short_h));
    const bool with_reset = shown * full_h <= room;
    const int row_h = with_reset ? full_h : short_h;
    const int step = std::max(row_h, room / shown);
    for (int i = 0; i < shown; ++i) {
      const Window &w = win[i];
      Row &r = row_[i];
      const int y = head_ + gap_ + i * step + (step - row_h) / 2;
      // The name at the left, the percent at the right, on one line.
      const std::string pct = percent_word(plugin_, w.pct);
      const int pct_w = std::min(width_ / 2, ui::text_width(pct, Font::TITLE) + ui::px(2));
      hide(r.name, false);
      hide(r.pct, false);
      ui::set_font(r.pct, Font::TITLE);
      lv_obj_set_size(r.pct, pct_w, line_h);
      lv_obj_set_pos(r.pct, width_ - pct_w, y);
      ui::set_text(r.pct, pct);
      ink_percent(r.pct, w.pct);
      lv_obj_set_size(r.name, std::max(1, width_ - pct_w - ui::px(4)), body_h);
      lv_obj_set_pos(r.name, 0, y + (line_h - body_h) / 2);
      ui::set_text(r.name, w.name);
      ui::set_color(r.name, theme::INK_SOFT);
      const int bar_y = y + line_h + gap_ / 2;
      paint_bar(r, w, bar_y, epoch);
      // Under the bar: when the window starts anew.
      if (with_reset && timed(w, epoch)) {
        hide(r.reset, false);
        lv_obj_set_size(r.reset, width_, body_h);
        lv_obj_set_pos(r.reset, 0, bar_y + bar_h_ + gap_ / 2);
        ui::set_text(r.reset, tessera::fill(plugin_->text("reset_in"), "n", span_word(plugin_, w.at - epoch)));
        ui::set_color(r.reset, theme::MUTED);
      }
    }
  }

  // One window alone, as Tessera's meter tile draws a value: the head with the reset in its top right corner, the
  // percent large, the window's name small under it, the bar with the pace mark at the foot.
  void paint_single(const Window &w, uint32_t epoch) {
    Row &r = row_[0];
    const int body_h = ui::line_height(Font::BODY);
    const int icon_w = ui::line_height(Font::ICON_SMALL) + ui::px(4);
    ui::set_tone(icon_, w.pct >= 70 ? fill_tone(w.pct) : Tone::NORMAL, theme::MUTED);

    // The reset: in the head's corner when it fits beside the icon and the name (the name may give up its end, down
    // to a few letters), else under the bar.
    const bool has_reset = timed(w, epoch);
    const std::string reset =
        has_reset ? tessera::fill(plugin_->text("reset_in"), "n", span_word(plugin_, w.at - epoch)) : std::string();
    const int title_w = ui::text_width(roomy::plain(name_), Font::TITLE) + ui::px(2);
    const int corner_w = has_reset ? ui::text_width(reset, Font::BODY) + ui::px(2) : 0;
    const bool corner = has_reset && icon_w + std::min(title_w, ui::px(56)) + gap_ + corner_w <= width_;
    bool under = has_reset && !corner;
    hide(corner_, !corner);
    if (corner) {
      lv_obj_set_size(corner_, corner_w, body_h);
      lv_obj_set_pos(corner_, width_ - corner_w, (head_ - body_h) / 2);
      ui::set_text(corner_, reset);
      ui::set_color(corner_, theme::MUTED);
      lv_obj_set_width(title_, std::max(1, std::min(title_w, width_ - icon_w - gap_ - corner_w)));
    }

    // The room between the head and the bar (the pace mark stands out over and under the bar).
    const int over = std::max(2, ui::px(3));
    const int top = head_ + gap_ / 2;
    auto room_for = [&](bool with_reset) {
      const int foot = height_ - (with_reset ? body_h + gap_ / 2 : 0);
      return foot - bar_h_ - 2 * over - gap_ / 2 - top;
    };
    int room = room_for(under);
    // A tile too low for the percent and the reset under the bar keeps the percent.
    if (under && room < ui::line_height(Font::BODY_LARGE)) {
      under = false;
      room = room_for(false);
    }
    const int bottom = height_ - (under ? body_h + gap_ / 2 : 0);
    const int bar_y = bottom - over - bar_h_;

    // The percent: the largest font that fits with the name under it; on the smallest the name stands beside it.
    const std::string pct = percent_word(plugin_, w.pct);
    Font face = Font::BODY_LARGE;
    bool name_under = false;
    for (Font f : {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      if (f == Font::DIGITS && !ui::covers(pct, f)) continue;
      if (ui::text_width(pct, f) <= width_ && ui::line_height(f) + body_h <= room) {
        face = f;
        name_under = true;
        break;
      }
    }
    const int big_h = ui::line_height(face);
    const int block_h = name_under ? big_h + body_h : big_h;
    const int y = top + std::max(0, (room - block_h) / 2);
    const int pct_w = std::min(width_, ui::text_width(pct, face) + ui::px(4));
    hide(r.pct, false);
    ui::set_font(r.pct, face);
    lv_obj_set_size(r.pct, pct_w, big_h);
    lv_obj_set_pos(r.pct, 0, y);
    ui::set_text(r.pct, pct);
    ink_percent(r.pct, w.pct);
    // The window's name: under the percent, or beside it at its foot when the tile is low.
    const int name_x = name_under ? 0 : pct_w + ui::px(4);
    const bool name_fits = width_ - name_x >= ui::px(24);
    hide(r.name, !name_fits);
    if (name_fits) {
      lv_obj_set_size(r.name, width_ - name_x, body_h);
      lv_obj_set_pos(r.name, name_x, name_under ? y + big_h : y + big_h - body_h);
      ui::set_text(r.name, w.name);
      ui::set_color(r.name, theme::INK_SOFT);
    }

    paint_bar(r, w, bar_y, epoch);
    if (under) {
      hide(r.reset, false);
      lv_obj_set_size(r.reset, width_, body_h);
      lv_obj_set_pos(r.reset, 0, bottom + gap_ / 2);
      ui::set_text(r.reset, reset);
      ui::set_color(r.reset, theme::MUTED);
    }
  }

  ClaudeSessions *plugin_;
  tessera::Text entity_, name_, wait_;
  LimitWindow window_ = LimitWindow::BOTH;
  int width_ = 0, height_ = 0, tile_ = -1, head_ = 0, gap_ = 0, bar_h_ = 6;
  int l5h_ = -1, lwk_ = -1, extra_ = 0;
  int extra_pct_[MAX_LIMIT_ROWS]{};
  char extra_name_[MAX_LIMIT_ROWS][20]{};
  uint32_t l5h_at_ = 0, lwk_at_ = 0, painted_minute_ = 0;
  bool has_entity_ = false, valid_ = false;
  lv_obj_t *icon_{}, *title_{}, *note_{}, *corner_{};
  Row row_[MAX_LIMIT_ROWS];
};

// ---- The background tile: what runs in the background of the sessions (shells, agents, monitors, workflows), and
// since when ----

static uint32_t kind_icon(const char *kind) {
  if (!std::strcmp(kind, "agent")) return ICON_ROBOT;
  if (!std::strcmp(kind, "monitor")) return ICON_WATCH;
  if (!std::strcmp(kind, "workflow")) return ICON_WORKFLOW;
  return ICON_SHELL;
}

// A task runs long: the accent in its first hour, amber from one hour, red from four.
static Tone age_tone(const BackgroundRow &row, uint32_t epoch) {
  if (!row.at || !epoch || epoch < row.at) return Tone::ACCENT;
  const uint32_t age = epoch - row.at;
  return age >= 4 * 3600 ? Tone::ALERT : age >= 3600 ? Tone::BUSY : Tone::ACCENT;
}

static bool has_progress(const BackgroundRow &row) { return row.total > 0 && row.done >= 0 && row.done <= row.total; }

// How far a task is, in thousandths: the bar FILLS. By the steps it reported (done / total), else by its age against
// eight hours (0.1.12: it used to show what was left). Never less than a sliver while it runs.
static int progress_share_pm(const BackgroundRow &row, uint32_t epoch) {
  static constexpr uint32_t AGE_SPAN = 8 * 3600;
  int pm = 0;
  if (has_progress(row)) {
    pm = row.done * 1000 / row.total;
  } else if (row.at && epoch >= row.at) {
    pm = static_cast<int>(std::min<uint32_t>(epoch - row.at, AGE_SPAN) * 1000 / AGE_SPAN);
  }
  return std::max(60, std::min(1000, pm));
}

// A bar of two blocks made in create()/open(): the track, the fill inside it as long as `pm` of it, in the tone.
static void paint_bar(lv_obj_t *track, lv_obj_t *fill, int x, int y, int w, int h, int pm, Tone tone) {
  w = std::max(1, w);
  hide(track, false);
  lv_obj_set_pos(track, x, y);
  lv_obj_set_size(track, w, h);
  ui::set_fill(track, Tone::NORMAL, theme::TRACK);
  hide(fill, false);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_size(fill, std::min(w, std::max(h, w * pm / 1000)), h);
  ui::set_fill(fill, tone, theme::ACCENT);
}

static lv_obj_t *bar_block(lv_obj_t *parent, theme::Role role) {
  lv_obj_t *o = ui::block(parent, role);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  return o;
}

// "a/b" when the task reported steps, else "" .
static std::string progress_word(const tessera::Plugin *plugin, const BackgroundRow &row) {
  if (!has_progress(row)) return "";
  return tessera::fill(tessera::fill(plugin->text("background_done"), "a", std::to_string(row.done)).c_str(), "b",
                       std::to_string(row.total));
}

// Steps are known only when the numbers make sense: at least one step, and no more done than there are.
// Counts are capped first, so the bar's `(total - done) * 1000` stays far inside an int.
static constexpr int MAX_STEPS = 10000, MAX_CALLS = 1000000;
static void sane(BackgroundRow &row) {
  row.total = std::min(row.total, MAX_STEPS);
  row.done = std::min(row.done, MAX_STEPS);
  row.calls = std::min(row.calls, MAX_CALLS);
  if (row.total < 1 || row.done < 0 || row.done > row.total) {
    row.done = -1;
    row.total = 0;
  }
  // Tool calls are an agent's: a shell keeps none, and a count below zero is unknown.
  if (row.calls < 0 || std::strcmp(row.kind, "shell") == 0) row.calls = -1;
}

// The rows of the background sensor: its parallel lists, with the steps from the mod (-1 and 0 where unknown).
static void parse_background(tessera::List<BackgroundRow> &tasks, JsonObjectConst a) {
  tasks.clear();
  JsonArrayConst titles = a["bg_titles"], kinds = a["bg_kinds"], sess = a["bg_sess"], hm = a["bg_hm"];
  // A list shorter than the titles (or missing, from an older integration) leaves its rows unknown.
  JsonArrayConst at = a["bg_at"], done = a["bg_done"], total = a["bg_total"], calls = a["bg_calls"];
  for (size_t i = 0; i < titles.size() && i < static_cast<size_t>(MAX_TASK_ROWS); ++i) {
    BackgroundRow row;
    copy(row.title, titles[i] | "");
    copy(row.kind, i < kinds.size() ? (kinds[i] | "shell") : "shell");
    copy(row.sess, i < sess.size() ? (sess[i] | "") : "");
    copy(row.hm, i < hm.size() ? (hm[i] | "") : "");
    row.at = i < at.size() ? (at[i] | 0u) : 0u;
    row.done = i < done.size() ? (done[i] | -1) : -1;
    row.total = i < total.size() ? (total[i] | 0) : 0;
    row.calls = i < calls.size() ? (calls[i] | -1) : -1;
    sane(row);
    tasks.push_back(row);
  }
}

// The answer of get_background: the same rows as objects, with the note the task last gave.
static void parse_background_result(tessera::List<BackgroundRow> &tasks, JsonObjectConst result) {
  tasks.clear();
  for (JsonObjectConst t : result["tasks"].as<JsonArrayConst>()) {
    if (tasks.size() >= static_cast<size_t>(MAX_TASK_ROWS)) break;
    BackgroundRow row;
    copy(row.title, t["title"] | "");
    copy(row.kind, t["kind"] | "shell");
    copy(row.sess, t["session"] | "");
    copy(row.hm, t["hm"] | "");
    copy(row.note, t["note"] | "");
    row.at = t["at"] | 0u;
    row.done = t["done"] | -1;
    row.total = t["total"] | 0;
    row.calls = t["calls"] | -1;
    sane(row);
    tasks.push_back(row);
  }
}

class BackgroundTile : public tessera::Tile {
 public:
  explicit BackgroundTile(ClaudeSessions *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    columns_ = c.columns;
    rows_ = c.rows;
    entity_ = c.entity ? c.entity : "";
    tile_ = c.tile;
    name_ = c.name && *c.name ? c.name : plugin_->text("background_title");
    const bool large = ui::large();
    gap_ = ui::px(large ? 6 : 4);
    bar_h_ = std::max(3, ui::px(large ? 6 : 4));
    // The head: the icon, the name, and at the right the badge with the number of tasks.
    icon_ = ui::label(c.parent, Font::ICON_SMALL, theme::MUTED);
    title_ = ui::label(c.parent, Font::TITLE, theme::INK);
    badge_h_ = ui::line_height(Font::BODY) + ui::px(large ? 6 : 4);
    badge_ = ui::block(c.parent, theme::ACCENT);
    lv_obj_set_style_radius(badge_, LV_RADIUS_CIRCLE, 0);
    badge_text_ = ui::label(badge_, Font::BODY, theme::ON_ACCENT);
    lv_obj_set_style_text_align(badge_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(badge_text_);
    head_ = std::max({ui::line_height(Font::TITLE), ui::line_height(Font::ICON_SMALL), badge_h_});
    big_ = ui::label(c.parent, Font::VALUE, theme::INK);
    lv_obj_set_style_text_align(big_, LV_TEXT_ALIGN_CENTER, 0);
    note_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    for (int i = 0; i < MAX_TASK_ROWS; ++i) {
      row_icon_[i] = ui::label(c.parent, Font::ICON_SMALL, theme::MUTED);
      row_title_[i] = ui::label(c.parent, Font::BODY, theme::INK_SOFT);
      row_when_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(row_when_[i], LV_TEXT_ALIGN_RIGHT, 0);
      // Under each task: what is left of it.
      row_track_[i] = bar_block(c.parent, theme::TRACK);
      row_fill_[i] = bar_block(row_track_[i], theme::ACCENT);
    }
  }

  void on_state(JsonObjectConst data) override {
    wait_ = data["wait"] | "";
    has_entity_ = !data["state"].isNull();
    JsonObjectConst a = data["attributes"];
    if (has_entity_ && !a.isNull() && !a["bg_titles"].isNull()) {
      parse_background(tasks_, a);
      count_ = std::atoi(data["state"] | "0");
      valid_ = true;
      ESP_LOGD(TAG, "Background data: %d tasks", count_);
    }
    paint(tessera::epoch());
  }

  // The running times count on: once a minute is enough.
  void on_tick(uint32_t epoch) override {
    if (epoch / 60 != painted_minute_) paint(epoch);
  }

  void on_theme() override { paint(tessera::epoch()); }

  // The background card: every task with its steps and its note.
  void on_tap() override { tessera::open_card(plugin_->plugin_id(), "background", roomy::plain(entity_), tile_); }

 private:
  void paint(uint32_t epoch) {
    painted_minute_ = epoch / 60;
    const bool ready = has_entity_ && valid_;
    const bool busy = ready && count_ > 0 && !tasks_.empty();
    ui::set_text(title_, roomy::plain(name_));
    ui::set_color(title_, theme::INK);
    ui::set_text(icon_, ui::icon(ICON_TASKS));
    ui::set_tone(icon_, busy ? Tone::ACCENT : Tone::NORMAL, theme::MUTED);
    const int icon_w = ui::line_height(Font::ICON_SMALL) + ui::px(4);
    lv_obj_set_pos(icon_, 0, (head_ - ui::line_height(Font::ICON_SMALL)) / 2);
    lv_obj_set_pos(title_, icon_w, (head_ - ui::line_height(Font::TITLE)) / 2);

    // The badge: how many run, while a list of them is shown under the head.
    const bool listed = busy && (rows_ >= 2 || columns_ >= 2);
    hide(badge_, !listed);
    int badge_w = 0;
    if (listed) {
      const std::string n = std::to_string(count_);
      badge_w = std::max(badge_h_, ui::text_width(n, Font::BODY) + ui::px(10));
      lv_obj_set_size(badge_, badge_w, badge_h_);
      lv_obj_set_pos(badge_, width_ - badge_w, (head_ - badge_h_) / 2);
      ui::set_color(badge_text_, theme::ON_ACCENT);
      ui::set_text(badge_text_, n);
    }
    lv_obj_set_width(title_, std::max(1, width_ - icon_w - (listed ? badge_w + ui::px(4) : 0)));

    for (int i = 0; i < MAX_TASK_ROWS; ++i) {
      hide(row_icon_[i], true);
      hide(row_title_[i], true);
      hide(row_when_[i], true);
      hide(row_track_[i], true);
    }
    const int body_h = ui::line_height(Font::BODY);
    if (!busy) {
      // Nothing to show: what to choose, or that nothing runs.
      hide(big_, true);
      hide(note_, false);
      lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_size(note_, width_, body_h);
      lv_obj_set_pos(note_, 0, head_ + gap_);
      ui::set_color(note_, theme::MUTED);
      const char *key = !has_entity_ || wait_ == "wrong_entity" ? "choose_background_sensor"
                        : !ready                                 ? "loading"
                                                                 : "background_none";
      ui::set_text(note_, plugin_->text(key));
      return;
    }

    if (!listed) {
      // One cell: the number big, under it since when the oldest runs.
      const std::string big = std::to_string(count_);
      const int room = height_ - head_;
      Font face = Font::BODY_LARGE;
      for (Font f : {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
        face = f;
        if (f == Font::DIGITS && !ui::covers(big, f)) continue;
        if (ui::text_width(big, f) <= width_ && ui::line_height(f) + body_h <= room) break;
      }
      const int big_h = ui::line_height(face);
      const int top = head_ + std::max(0, (room - big_h - body_h) / 2);
      hide(big_, false);
      ui::set_font(big_, face);
      ui::set_text(big_, big);
      ui::set_tone(big_, Tone::ACCENT, theme::INK);
      lv_obj_set_size(big_, width_, big_h);
      lv_obj_set_pos(big_, 0, top);
      // The newest come first: the oldest is the last.
      const BackgroundRow &oldest = tasks_[tasks_.size() - 1];
      hide(note_, false);
      lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_size(note_, width_, body_h);
      lv_obj_set_pos(note_, 0, top + big_h);
      ui::set_color(note_, theme::MUTED);
      ui::set_text(note_, oldest.hm[0] ? tessera::fill(plugin_->text("background_since"), "n", oldest.hm)
                                       : std::string(plugin_->text("background_runs")));
      return;
    }

    // A list: per task its kind, its title, and at the right when it started and, where there is room, for how long
    // (or how many of its steps are done); under it a bar that fills with its progress, coloured by its age.
    hide(big_, true);
    hide(note_, true);
    const int icon_h = ui::line_height(Font::ICON_SMALL);
    const int line_h = std::max(body_h, icon_h);
    const int bar_gap = std::max(1, gap_ / 2);
    const int row_h = line_h + bar_gap + bar_h_;
    const int step = row_h + std::max(1, gap_ / 2);
    const bool roomy_row = columns_ >= 2;
    const int total = static_cast<int>(tasks_.size());
    int y = head_ + gap_, shown = 0;
    for (const BackgroundRow &row : tasks_) {
      if (shown >= MAX_TASK_ROWS || y + row_h > height_) break;
      // The last line that fits says how many more there are, when more than it would show.
      const int more = total - shown;
      if (more > 1 && shown > 0 && (y + step + row_h > height_ || shown == MAX_TASK_ROWS - 1)) {
        lv_obj_t *title = row_title_[shown];
        hide(title, false);
        lv_obj_set_size(title, width_, body_h);
        lv_obj_set_pos(title, 0, y + (line_h - body_h) / 2);
        ui::set_color(title, theme::MUTED);
        ui::set_text(title, tessera::format(plugin_->text("background_more"), more));
        break;
      }
      // At the right: "14:05 · 2/5" with steps, else "14:05 · 12 min"; one column wide only "2/5" or "14:05".
      const std::string done = progress_word(plugin_, row);
      std::string when = roomy_row || done.empty() ? row.hm : "";
      if (!done.empty()) {
        if (!when.empty()) when += " · ";
        when += done;
      } else if (roomy_row && row.at && epoch >= row.at) {
        if (!when.empty()) when += " · ";
        when += span_word(plugin_, epoch - row.at);
      }
      const int when_w = std::min(width_ / 2, ui::text_width(when, Font::BODY) + ui::px(4));
      lv_obj_t *icon = row_icon_[shown], *title = row_title_[shown], *at = row_when_[shown];
      hide(icon, false);
      hide(title, false);
      hide(at, false);
      ui::set_text(icon, ui::icon(kind_icon(row.kind)));
      ui::set_color(icon, theme::MUTED);
      lv_obj_set_pos(icon, 0, y + (line_h - icon_h) / 2);
      lv_obj_set_size(at, when_w, body_h);
      lv_obj_set_pos(at, width_ - when_w, y + (line_h - body_h) / 2);
      ui::set_text(at, when);
      ui::set_color(at, theme::MUTED);
      lv_obj_set_size(title, std::max(1, width_ - icon_w - when_w - ui::px(6)), body_h);
      lv_obj_set_pos(title, icon_w, y + (line_h - body_h) / 2);
      ui::set_text(title, row.title);
      ui::set_color(title, theme::INK_SOFT);
      paint_bar(row_track_[shown], row_fill_[shown], icon_w, y + line_h + bar_gap, width_ - icon_w, bar_h_,
                progress_share_pm(row, epoch), age_tone(row, epoch));
      y += step;
      shown += 1;
    }
  }

  ClaudeSessions *plugin_;
  tessera::Text entity_, name_, wait_;
  tessera::List<BackgroundRow> tasks_;
  int width_ = 0, height_ = 0, columns_ = 1, rows_ = 1, tile_ = -1, head_ = 0, gap_ = 0, badge_h_ = 0, count_ = 0;
  int bar_h_ = 4;
  bool has_entity_ = false, valid_ = false;
  uint32_t painted_minute_ = 0;
  lv_obj_t *icon_{}, *title_{}, *badge_{}, *badge_text_{}, *big_{}, *note_{};
  lv_obj_t *row_icon_[MAX_TASK_ROWS]{}, *row_title_[MAX_TASK_ROWS]{}, *row_when_[MAX_TASK_ROWS]{};
  lv_obj_t *row_track_[MAX_TASK_ROWS]{}, *row_fill_[MAX_TASK_ROWS]{};
};

// ---- The sessions card: every session, a page at a time; a tap on one with a question opens it ----

class SessionsCard : public tessera::Card {
 public:
  explicit SessionsCard(ClaudeSessions *plugin) : plugin_(plugin) {}
  ~SessionsCard() override {
    if (plugin_->sessions_card_ == this) plugin_->sessions_card_ = nullptr;
  }

  void open(const tessera::CardContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    const bool large = ui::large();
    gap_ = ui::px(large ? 10 : 6);
    line1_ = ui::line_height(Font::BODY_LARGE);
    line2_ = ui::line_height(Font::BODY);
    row_h_ = line1_ + line2_ + ui::px(large ? 4 : 2);
    note_ = ui::label(parent_, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_width(note_, width_);
    ui::set_text(note_, plugin_->text("loading"));
    for (int i = 0; i < MAX_PAGE_ROWS; ++i) {
      row_bg_[i] = ui::block(parent_, theme::CARD);
      row_name_[i] = ui::label(row_bg_[i], Font::BODY_LARGE, theme::INK);
      row_state_[i] = ui::label(row_bg_[i], Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(row_state_[i], LV_TEXT_ALIGN_RIGHT, 0);
      row_line_[i] = ui::label(row_bg_[i], Font::BODY, theme::MUTED);
      row_badge_[i] = ui::block(row_bg_[i], theme::ACCENT);
      lv_obj_set_style_radius(row_badge_[i], LV_RADIUS_CIRCLE, 0);
      row_badge_text_[i] = ui::label(row_badge_[i], Font::BODY, theme::ON_ACCENT);
      lv_obj_set_style_text_align(row_badge_text_[i], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(row_badge_text_[i]);
    }
    // The footer: the limits, then back, the page, next.
    limits_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(limits_, width_);
    lv_obj_set_style_text_align(limits_, LV_TEXT_ALIGN_CENTER, 0);
    const int key = std::max(ui::mm(9), ui::line_height(Font::ICON_SMALL));
    footer_h_ = key;
    prev_ = ui::label(parent_, Font::ICON_SMALL, theme::INK);
    next_ = ui::label(parent_, Font::ICON_SMALL, theme::INK);
    ui::set_text(prev_, ui::icon(ICON_PREV));
    ui::set_text(next_, ui::icon(ICON_NEXT));
    lv_obj_set_size(prev_, key * 2, key);
    lv_obj_set_size(next_, key * 2, key);
    lv_obj_set_style_text_align(prev_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(next_, LV_TEXT_ALIGN_RIGHT, 0);
    page_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(page_, std::max(1, width_ - key * 4 - ui::px(8)));
    lv_obj_set_style_text_align(page_, LV_TEXT_ALIGN_CENTER, 0);
    plugin_->sessions_card_ = this;
    paint();
    plugin_->request_overview();
  }

  void on_state(JsonObjectConst data) override {
    if (!data["state"].isNull()) plugin_->take_tile_data(data);
    paint();
  }

  void on_theme() override {
    ui::set_color(note_, theme::MUTED);
    ui::set_color(limits_, theme::MUTED);
    ui::set_color(page_, theme::MUTED);
    ui::set_color(prev_, theme::INK);
    ui::set_color(next_, theme::INK);
    for (int i = 0; i < MAX_PAGE_ROWS; ++i) {
      ui::set_color(row_name_[i], theme::INK);
      ui::set_color(row_line_[i], theme::MUTED);
    }
    paint();
  }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    if (inside(prev_, parent_, p)) { if (page_index_ > 0) { page_index_ -= 1; paint(); } return; }
    if (inside(next_, parent_, p)) { if (page_index_ + 1 < pages_) { page_index_ += 1; paint(); } return; }
    const Overview &o = plugin_->overview();
    for (int i = 0; i < MAX_PAGE_ROWS; ++i) {
      if (!inside(row_bg_[i], parent_, p)) continue;
      const size_t index = page_index_ * per_page_ + i;
      if (index >= o.rows.size()) return;
      const SessionRow &row = o.rows[index];
      if (row.q[0]) plugin_->open_question(row.q);
      else if (!std::strcmp(row.state, "ask")) plugin_->request_overview();  // the handle comes with the overview
      return;
    }
  }

  void paint() {
    const Overview &o = plugin_->overview();
    const std::string limits = limits_line(plugin_, o, tessera::epoch());
    hide(limits_, limits.empty());
    ui::set_text(limits_, limits);
    const int footer_top = height_ - footer_h_;
    const int limits_top = limits.empty() ? footer_top : footer_top - line2_ - gap_ / 2;
    lv_obj_set_pos(limits_, 0, limits_top);
    const int list_bottom = limits_top - gap_ / 2;
    per_page_ = std::max(1, std::min(MAX_PAGE_ROWS, (list_bottom + gap_) / (row_h_ + gap_)));
    const int total = static_cast<int>(o.rows.size());
    pages_ = std::max(1, (total + per_page_ - 1) / per_page_);
    if (page_index_ >= pages_) page_index_ = pages_ - 1;
    const bool paged = pages_ > 1;
    hide(prev_, !paged);
    hide(next_, !paged);
    hide(page_, !paged);
    lv_obj_set_pos(prev_, 0, footer_top);
    lv_obj_set_pos(next_, width_ - footer_h_ * 2, footer_top);
    lv_obj_set_pos(page_, footer_h_ * 2 + ui::px(4), footer_top + (footer_h_ - line2_) / 2);
    if (paged) {
      ui::set_text(page_, tessera::fill(tessera::fill(plugin_->text("page"), "a", std::to_string(page_index_ + 1)).c_str(),
                                        "b", std::to_string(pages_)));
    }
    hide(note_, total > 0);
    if (total == 0) ui::set_text(note_, plugin_->text(o.valid ? "none" : "loading"));
    int y = 0;
    for (int i = 0; i < MAX_PAGE_ROWS; ++i) {
      const int index = page_index_ * per_page_ + i;
      const bool show = i < per_page_ && index < total;
      hide(row_bg_[i], !show);
      hide(row_badge_[i], !show);
      if (!show) continue;
      const SessionRow &row = o.rows[index];
      lv_obj_set_size(row_bg_[i], width_, row_h_);
      lv_obj_set_pos(row_bg_[i], 0, y);
      const int pad = ui::px(ui::large() ? 10 : 6);
      // The badge with the questions, at the right of the first line.
      int right = width_ - pad;
      if (row.asks > 0) {
        const std::string count = std::to_string(row.asks);
        const int bh = line2_ + ui::px(4);
        const int bw = std::max(bh, ui::text_width(count, Font::BODY) + ui::px(10));
        lv_obj_set_size(row_badge_[i], bw, bh);
        lv_obj_set_pos(row_badge_[i], right - bw, (line1_ - bh) / 2 + ui::px(2));
        ui::set_text(row_badge_text_[i], count);
        right -= bw + ui::px(6);
      } else {
        hide(row_badge_[i], true);
      }
      const std::string state = state_word(plugin_, row.state);
      const int state_w = std::min(width_ / 3, ui::text_width(state, Font::BODY) + ui::px(4));
      lv_obj_set_size(row_state_[i], state_w, line2_);
      lv_obj_set_pos(row_state_[i], right - state_w, ui::px(2) + (line1_ - line2_) / 2);
      ui::set_text(row_state_[i], state);
      ui::set_color(row_state_[i], state_role(row.state));
      lv_obj_set_size(row_name_[i], std::max(1, right - state_w - pad - ui::px(6)), line1_);
      lv_obj_set_pos(row_name_[i], pad, ui::px(2));
      ui::set_text(row_name_[i], row.name);
      // The second line: the computer, the context, and what it waits for.
      std::string line;
      if (row.pc[0]) line = row.pc;
      if (row.ctx >= 0) { if (!line.empty()) line += " · "; line += ctx_word(plugin_, row.ctx); }
      if (row.detail[0] && !std::strcmp(row.state, "perm")) { if (!line.empty()) line += " · "; line += row.detail; }
      if (line.empty()) line = ctx_word(plugin_, row.ctx);
      lv_obj_set_size(row_line_[i], width_ - 2 * pad, line2_);
      lv_obj_set_pos(row_line_[i], pad, ui::px(2) + line1_);
      ui::set_text(row_line_[i], line);
      y += row_h_ + gap_;
    }
  }

 private:
  ClaudeSessions *plugin_;
  lv_obj_t *parent_{}, *note_{}, *limits_{}, *prev_{}, *next_{}, *page_{};
  lv_obj_t *row_bg_[MAX_PAGE_ROWS]{}, *row_name_[MAX_PAGE_ROWS]{}, *row_state_[MAX_PAGE_ROWS]{}, *row_line_[MAX_PAGE_ROWS]{};
  lv_obj_t *row_badge_[MAX_PAGE_ROWS]{}, *row_badge_text_[MAX_PAGE_ROWS]{};
  int width_ = 0, height_ = 0, gap_ = 0, line1_ = 0, line2_ = 0, row_h_ = 0, footer_h_ = 0;
  int per_page_ = 1, pages_ = 1, page_index_ = 0;
};

// ---- The background card: every task that runs, with its session, since when, its steps and the note it gave ----

static constexpr int MAX_BG_PAGE_ROWS = 4;

class BackgroundCard : public tessera::Card {
 public:
  explicit BackgroundCard(ClaudeSessions *plugin) : plugin_(plugin) {}
  ~BackgroundCard() override {
    if (plugin_->background_card_ == this) plugin_->background_card_ = nullptr;
  }

  void open(const tessera::CardContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    const bool large = ui::large();
    gap_ = ui::px(large ? 10 : 6);
    pad_ = ui::px(large ? 10 : 6);
    line1_ = std::max(ui::line_height(Font::BODY_LARGE), ui::line_height(Font::ICON_SMALL));
    line2_ = ui::line_height(Font::BODY);
    bar_h_ = std::max(3, ui::px(large ? 6 : 4));
    bar_gap_ = ui::px(large ? 4 : 3);
    // Per row: the head line, the session line, the bar, the steps line.
    row_h_ = ui::px(2) + line1_ + line2_ + bar_gap_ + bar_h_ + bar_gap_ + line2_ + ui::px(large ? 6 : 4);
    note_ = ui::label(parent_, Font::BODY_LARGE, theme::MUTED);
    lv_obj_set_width(note_, width_);
    ui::set_text(note_, plugin_->text("loading"));
    for (int i = 0; i < MAX_BG_PAGE_ROWS; ++i) {
      row_bg_[i] = ui::block(parent_, theme::CARD);
      row_icon_[i] = ui::label(row_bg_[i], Font::ICON_SMALL, theme::MUTED);
      row_title_[i] = ui::label(row_bg_[i], Font::BODY_LARGE, theme::INK);
      row_line_[i] = ui::label(row_bg_[i], Font::BODY, theme::MUTED);
      row_track_[i] = bar_block(row_bg_[i], theme::TRACK);
      row_fill_[i] = bar_block(row_track_[i], theme::ACCENT);
      row_step_[i] = ui::label(row_bg_[i], Font::BODY, theme::INK_SOFT);
    }
    // The footer: back, the page, next.
    const int key = std::max(ui::mm(9), ui::line_height(Font::ICON_SMALL));
    footer_h_ = key;
    prev_ = ui::label(parent_, Font::ICON_SMALL, theme::INK);
    next_ = ui::label(parent_, Font::ICON_SMALL, theme::INK);
    ui::set_text(prev_, ui::icon(ICON_PREV));
    ui::set_text(next_, ui::icon(ICON_NEXT));
    lv_obj_set_size(prev_, key * 2, key);
    lv_obj_set_size(next_, key * 2, key);
    lv_obj_set_style_text_align(prev_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(next_, LV_TEXT_ALIGN_RIGHT, 0);
    page_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(page_, std::max(1, width_ - key * 4 - ui::px(8)));
    lv_obj_set_style_text_align(page_, LV_TEXT_ALIGN_CENTER, 0);
    plugin_->background_card_ = this;
    plugin_->clear_background();  // a re-opened card shows no old notes while the new ones are on their way
    paint(tessera::epoch());
    // The rows with their notes come from the integration; the sensor alone has no notes.
    plugin_->request_background();
  }

  // The tile's sensor changed: take its rows (keeping the notes); the hub then asks for the notes anew.
  void on_state(JsonObjectConst data) override {
    JsonObjectConst a = data["attributes"];
    if (!data["state"].isNull() && !a.isNull() && !a["bg_titles"].isNull()) plugin_->take_background(a);
    paint(tessera::epoch());
  }

  void on_tick(uint32_t epoch) override {
    if (epoch / 60 != painted_minute_) paint(epoch);
  }

  void on_theme() override {
    ui::set_color(note_, theme::MUTED);
    ui::set_color(page_, theme::MUTED);
    ui::set_color(prev_, theme::INK);
    ui::set_color(next_, theme::INK);
    for (int i = 0; i < MAX_BG_PAGE_ROWS; ++i) {
      ui::set_color(row_title_[i], theme::INK);
      ui::set_color(row_line_[i], theme::MUTED);
    }
    paint(tessera::epoch());
  }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    if (inside(prev_, parent_, p)) {
      if (page_index_ > 0) {
        page_index_ -= 1;
        paint(tessera::epoch());
      }
      return;
    }
    if (inside(next_, parent_, p) && page_index_ + 1 < pages_) {
      page_index_ += 1;
      paint(tessera::epoch());
    }
  }

  void paint(uint32_t epoch) {
    painted_minute_ = epoch / 60;
    const tessera::List<BackgroundRow> &tasks = plugin_->background();
    const int footer_top = height_ - footer_h_;
    const int list_bottom = footer_top - gap_ / 2;
    per_page_ = std::max(1, std::min(MAX_BG_PAGE_ROWS, (list_bottom + gap_) / (row_h_ + gap_)));
    const int total = static_cast<int>(tasks.size());
    pages_ = std::max(1, (total + per_page_ - 1) / per_page_);
    if (page_index_ >= pages_) page_index_ = pages_ - 1;
    const bool paged = pages_ > 1;
    hide(prev_, !paged);
    hide(next_, !paged);
    hide(page_, !paged);
    lv_obj_set_pos(prev_, 0, footer_top);
    lv_obj_set_pos(next_, width_ - footer_h_ * 2, footer_top);
    lv_obj_set_pos(page_, footer_h_ * 2 + ui::px(4), footer_top + (footer_h_ - line2_) / 2);
    if (paged) {
      ui::set_text(page_, tessera::fill(tessera::fill(plugin_->text("page"), "a", std::to_string(page_index_ + 1)).c_str(),
                                        "b", std::to_string(pages_)));
    }
    hide(note_, total > 0);
    if (total == 0) ui::set_text(note_, plugin_->text(plugin_->background_valid() ? "background_none" : "loading"));
    const int icon_w = ui::line_height(Font::ICON_SMALL) + ui::px(4);
    const int inner = std::max(1, width_ - 2 * pad_);
    int y = 0;
    for (int i = 0; i < MAX_BG_PAGE_ROWS; ++i) {
      const int index = page_index_ * per_page_ + i;
      const bool show = i < per_page_ && index < total;
      hide(row_bg_[i], !show);
      if (!show) continue;
      const BackgroundRow &row = tasks[index];
      const Tone tone = age_tone(row, epoch);
      lv_obj_set_size(row_bg_[i], width_, row_h_);
      lv_obj_set_pos(row_bg_[i], 0, y);
      int top = ui::px(2);
      // The head: the kind, in the colour of its age, and the title.
      ui::set_text(row_icon_[i], ui::icon(kind_icon(row.kind)));
      ui::set_tone(row_icon_[i], tone, theme::MUTED);
      lv_obj_set_pos(row_icon_[i], pad_, top + (line1_ - ui::line_height(Font::ICON_SMALL)) / 2);
      lv_obj_set_size(row_title_[i], std::max(1, inner - icon_w), ui::line_height(Font::BODY_LARGE));
      lv_obj_set_pos(row_title_[i], pad_ + icon_w, top + (line1_ - ui::line_height(Font::BODY_LARGE)) / 2);
      ui::set_text(row_title_[i], row.title);
      top += line1_;
      // The session, since when, for how long.
      std::string line = row.sess;
      if (row.hm[0]) {
        if (!line.empty()) line += " · ";
        line += tessera::fill(plugin_->text("background_since"), "n", row.hm);
      }
      if (row.at && epoch >= row.at) {
        if (!line.empty()) line += " · ";
        line += span_word(plugin_, epoch - row.at);
      }
      lv_obj_set_size(row_line_[i], inner, line2_);
      lv_obj_set_pos(row_line_[i], pad_, top);
      ui::set_text(row_line_[i], line);
      top += line2_ + bar_gap_;
      // How far it is (the bar fills), coloured by its age.
      paint_bar(row_track_[i], row_fill_[i], pad_, top, inner, bar_h_, progress_share_pm(row, epoch), tone);
      top += bar_h_ + bar_gap_;
      // The steps and the note, or that the task reports none.
      std::string step;
      if (has_progress(row)) {
        step = tessera::fill(tessera::fill(plugin_->text("background_step"), "a", std::to_string(row.done)).c_str(), "b",
                             std::to_string(row.total));
      }
      if (row.note[0]) {
        if (!step.empty()) step += " · ";
        step += row.note;
      }
      // Neither steps nor a note: an agent's tool calls so far, if the integration counts them (0.2.6).
      if (step.empty() && row.calls >= 0) step = tessera::format(plugin_->text("background_calls"), row.calls);
      const bool quiet = step.empty();
      if (quiet) step = plugin_->text("background_no_progress");
      lv_obj_set_size(row_step_[i], inner, line2_);
      lv_obj_set_pos(row_step_[i], pad_, top);
      ui::set_text(row_step_[i], step);
      ui::set_color(row_step_[i], quiet ? theme::MUTED : theme::INK_SOFT);
      y += row_h_ + gap_;
    }
  }

 private:
  ClaudeSessions *plugin_;
  lv_obj_t *parent_{}, *note_{}, *prev_{}, *next_{}, *page_{};
  lv_obj_t *row_bg_[MAX_BG_PAGE_ROWS]{}, *row_icon_[MAX_BG_PAGE_ROWS]{}, *row_title_[MAX_BG_PAGE_ROWS]{};
  lv_obj_t *row_line_[MAX_BG_PAGE_ROWS]{}, *row_track_[MAX_BG_PAGE_ROWS]{}, *row_fill_[MAX_BG_PAGE_ROWS]{};
  lv_obj_t *row_step_[MAX_BG_PAGE_ROWS]{};
  int width_ = 0, height_ = 0, gap_ = 0, pad_ = 0, line1_ = 0, line2_ = 0, bar_h_ = 4, bar_gap_ = 3, row_h_ = 0;
  int footer_h_ = 0, per_page_ = 1, pages_ = 1, page_index_ = 0;
  uint32_t painted_minute_ = 0;
};

// ---- The question card: one part at a time, its options as keys; the answer goes to the integration ----

class QuestionCard : public tessera::Card {
 public:
  explicit QuestionCard(ClaudeSessions *plugin) : plugin_(plugin) {}
  ~QuestionCard() override {
    if (plugin_->question_card_ == this) plugin_->question_card_ = nullptr;
  }

  void open(const tessera::CardContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    const bool large = ui::large();
    gap_ = ui::px(large ? 8 : 5);
    pad_ = ui::px(large ? 10 : 6);
    copy(handle_, plugin_->pending_question());
    // The head: the part's header as a chip, the session's name beside it.
    chip_ = ui::block(parent_, theme::TRACK);
    chip_text_ = ui::label(chip_, Font::BODY, theme::INK);
    lv_obj_center(chip_text_);
    chip_h_ = ui::line_height(Font::BODY) + ui::px(6);
    lv_obj_set_height(chip_, chip_h_);
    who_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(who_, LV_TEXT_ALIGN_RIGHT, 0);
    // The question, several lines.
    text_ = ui::text(parent_, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(text_, width_);
    // The options: a key each, the label and its description.
    for (int i = 0; i < MAX_OPTIONS; ++i) {
      opt_bg_[i] = ui::block(parent_, theme::CARD);
      opt_mark_[i] = ui::label(opt_bg_[i], Font::ICON_SMALL, theme::ACCENT);
      opt_label_[i] = ui::label(opt_bg_[i], Font::BODY_LARGE, theme::INK);
      opt_desc_[i] = ui::label(opt_bg_[i], Font::BODY, theme::MUTED);
    }
    // The row that turns to the next options when they do not all fit.
    more_ = ui::block(parent_, theme::TRACK);
    more_text_ = ui::label(more_, Font::BODY_LARGE, theme::INK);
    lv_obj_center(more_text_);
    // The foot: a note, the PC key on the left, the next or send key on the right.
    note_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(note_, width_);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    key_h_ = std::max(ui::mm(ui::large() ? 9 : 7), ui::line_height(Font::BODY_LARGE) + ui::px(6));
    pc_ = ui::block(parent_, theme::TRACK);
    pc_text_ = ui::label(pc_, Font::BODY_LARGE, theme::INK);
    lv_obj_center(pc_text_);
    ui::set_text(pc_text_, plugin_->text("on_pc"));
    go_ = ui::block(parent_, theme::ACCENT);
    go_text_ = ui::label(go_, Font::BODY_LARGE, theme::ON_ACCENT);
    lv_obj_center(go_text_);
    plugin_->question_card_ = this;
    refresh();
    plugin_->request_question(handle_);
  }

  void on_tick(uint32_t) override {
    if (close_in_ticks_ > 0 && --close_in_ticks_ == 0) tessera::close_card();
  }

  void on_theme() override {
    ui::set_color(chip_text_, theme::INK);
    ui::set_color(who_, theme::MUTED);
    ui::set_color(text_, theme::INK);
    ui::set_color(note_, theme::MUTED);
    ui::set_color(pc_text_, theme::INK);
    ui::set_color(go_text_, theme::ON_ACCENT);
    for (int i = 0; i < MAX_OPTIONS; ++i) {
      ui::set_color(opt_label_[i], theme::INK);
      ui::set_color(opt_desc_[i], theme::MUTED);
      ui::set_color(opt_mark_[i], theme::ACCENT);
    }
    refresh();
  }

  void on_tap() override {
    if (done_) return;
    const tessera::Point p = tessera::tap_point();
    Question &q = plugin_->question();
    if (inside(pc_, parent_, p)) { tessera::close_card(); return; }
    if (!q.valid || part_ >= q.parts.size()) return;
    Part &part = q.parts[part_];
    if (inside(go_, parent_, p)) {
      if (!any_chosen(part)) return;
      advance();
      return;
    }
    if (inside(more_, parent_, p)) {
      opt_page_ += 1;
      refresh();
      return;
    }
    for (int i = 0; i < MAX_OPTIONS; ++i) {
      if (!inside(opt_bg_[i], parent_, p)) continue;
      const int index = opt_first_ + i;
      if (index >= static_cast<int>(part.options.size())) return;
      // A tap marks; the send key sends. One tap no longer answers a single choice at once: a finger that meant the
      // popup or the card itself landed on the first option and answered the model before the person read it.
      if (part.multi) {
        part.options[index].chosen = !part.options[index].chosen;
      } else {
        for (Option &o : part.options) o.chosen = false;
        part.options[index].chosen = true;
      }
      refresh();
      return;
    }
  }

  // The question (or its status) came from the integration.
  void refresh() {
    const Question &q = plugin_->question();
    if (q.valid && handle_[0] == '\0') copy(handle_, q.h);
    const bool mine = q.valid && !std::strcmp(q.h, handle_);
    const bool over = mine && std::strcmp(q.status, "open") != 0;
    if (!mine || over) {
      // Nothing to answer: say so and offer the way out. Answered elsewhere (another screen, the PC), it also closes
      // itself after a moment.
      if (over && !done_ && close_in_ticks_ == 0) close_in_ticks_ = 3;
      hide(chip_, true);
      hide(who_, true);
      hide(text_, true);
      for (int i = 0; i < MAX_OPTIONS; ++i) hide(opt_bg_[i], true);
      hide(more_, true);
      hide(go_, true);
      hide(note_, false);
      lv_obj_set_pos(note_, 0, 0);
      ui::set_text(note_, plugin_->text(!q.valid && !failed_ ? "loading" : over ? "gone" : failed_ ? "failed" : "loading"));
      place_pc();
      return;
    }
    if (part_ >= q.parts.size()) part_ = 0;
    const Part &part = q.parts[part_];
    int y = 0;
    // The head.
    hide(chip_, part.header[0] == '\0');
    if (part.header[0]) {
      std::string head = part.header;
      if (q.parts.size() > 1) head += " " + std::to_string(part_ + 1) + "/" + std::to_string(q.parts.size());
      ui::set_text(chip_text_, head);
      lv_obj_set_size(chip_, ui::text_width(head, Font::BODY) + ui::px(16), chip_h_);
      lv_obj_set_pos(chip_, 0, y);
    }
    hide(who_, false);
    lv_obj_set_size(who_, width_ / 2, ui::line_height(Font::BODY));
    lv_obj_set_pos(who_, width_ - width_ / 2, y + (chip_h_ - ui::line_height(Font::BODY)) / 2);
    ui::set_text(who_, q.name);
    y += chip_h_ + gap_;
    // The question.
    std::string text;
    for (const tessera::Text &line : part.lines) {
      if (!text.empty()) text += " ";
      text += roomy::plain(line);
    }
    hide(text_, false);
    const int lh = ui::line_height(Font::BODY_LARGE);
    const int n = static_cast<int>(part.options.size());
    // What is left for the question and the options, under the head and over the foot.
    const bool noted = part.multi;
    const int foot = key_h_ + gap_ + (noted ? ui::line_height(Font::BODY) + gap_ / 2 : 0);
    const int room = height_ - y - foot;
    // The options take their room first: with their descriptions when all fit beside one line of the question,
    // else as one line each; what does not fit even so goes on a next page behind a "more" row.
    bool with_desc = true;
    int opt_h = option_height(part, true);
    if (n * (opt_h + gap_) + lh > room) {
      with_desc = false;
      opt_h = option_height(part, false);
    }
    // Two lines of the question stay at least, when it needs them; the options page behind a "more" row.
    const int keep = ui::text_width(text, Font::BODY_LARGE) > width_ ? 2 : 1;
    int slots = std::max(1, (room - keep * lh - gap_) / (opt_h + gap_));
    int shown_n = n, first = 0;
    const bool paged = slots < n;
    if (paged) {
      const int per = std::max(1, slots - 1);
      const int pages = (n + per - 1) / per;
      if (opt_page_ >= pages) opt_page_ = 0;
      first = opt_page_ * per;
      shown_n = std::min(per, n - first);
    }
    opt_first_ = first;
    const int taken = shown_n * (opt_h + gap_) + (paged ? opt_h + gap_ : 0);
    int lines = std::max(1, (room - taken) / lh);
    lines = std::min(lines, std::min(MAX_LINES, ui::text_width(text, Font::BODY_LARGE) / std::max(1, width_) + 2));
    lv_obj_set_size(text_, width_, lines * lh);
    lv_obj_set_pos(text_, 0, y);
    ui::set_text(text_, text);
    y += lines * lh + gap_;
    // The options.
    for (int i = 0; i < MAX_OPTIONS; ++i) {
      const bool show = i < shown_n;
      hide(opt_bg_[i], !show);
      if (!show) continue;
      const Option &o = part.options[first + i];
      lv_obj_set_size(opt_bg_[i], width_, opt_h);
      lv_obj_set_pos(opt_bg_[i], 0, y);
      ui::set_fill(opt_bg_[i], o.chosen ? Tone::ACCENT : Tone::NORMAL, theme::CARD);
      const int mark_w = part.multi ? ui::line_height(Font::ICON_SMALL) + ui::px(4) : 0;
      hide(opt_mark_[i], !part.multi);
      if (part.multi) {
        lv_obj_set_size(opt_mark_[i], mark_w, ui::line_height(Font::ICON_SMALL));
        lv_obj_set_pos(opt_mark_[i], pad_, (opt_h - ui::line_height(Font::ICON_SMALL)) / 2);
        ui::set_text(opt_mark_[i], o.chosen ? ui::icon(ICON_CHECK) : "");
        ui::set_color(opt_mark_[i], o.chosen ? theme::ON_ACCENT : theme::ACCENT);
      }
      const int x = pad_ + mark_w;
      const bool has_desc = with_desc && o.desc[0] != '\0';
      const int label_h = ui::line_height(Font::BODY_LARGE), desc_h = ui::line_height(Font::BODY);
      const int top = has_desc ? (opt_h - label_h - desc_h) / 2 : (opt_h - label_h) / 2;
      lv_obj_set_size(opt_label_[i], std::max(1, width_ - x - pad_), label_h);
      lv_obj_set_pos(opt_label_[i], x, top);
      ui::set_text(opt_label_[i], o.label);
      ui::set_color(opt_label_[i], o.chosen ? theme::ON_ACCENT : theme::INK);
      hide(opt_desc_[i], !has_desc);
      if (has_desc) {
        lv_obj_set_size(opt_desc_[i], std::max(1, width_ - x - pad_), desc_h);
        lv_obj_set_pos(opt_desc_[i], x, top + label_h);
        ui::set_text(opt_desc_[i], o.desc);
        ui::set_color(opt_desc_[i], o.chosen ? theme::ON_ACCENT : theme::MUTED);
      }
      y += opt_h + gap_;
    }
    hide(more_, !paged);
    if (paged) {
      lv_obj_set_size(more_, width_, opt_h);
      lv_obj_set_pos(more_, 0, y);
      ui::set_text(more_text_, tessera::fill(tessera::fill(plugin_->text("more"), "a", std::to_string(first + shown_n)).c_str(),
                                             "b", std::to_string(n)));
      y += opt_h + gap_;
    }
    // The foot.
    hide(note_, !noted);
    lv_obj_set_pos(note_, 0, height_ - key_h_ - gap_ / 2 - ui::line_height(Font::BODY));
    ui::set_text(note_, note_text(part));
    place_pc();
    const bool needs_go = true;
    hide(go_, !needs_go);
    if (needs_go) {
      const bool last = part_ + 1 >= q.parts.size();
      ui::set_text(go_text_, plugin_->text(last ? "send" : "next"));
      lv_obj_set_size(go_, width_ / 2 - gap_ / 2, key_h_);
      lv_obj_set_pos(go_, width_ / 2 + gap_ / 2, height_ - key_h_);
      ui::set_fill(go_, any_chosen(part) ? Tone::ACCENT : Tone::NORMAL, theme::TRACK);
      ui::set_color(go_text_, any_chosen(part) ? theme::ON_ACCENT : theme::MUTED);
    }
  }

  // The integration's word on the answer.
  void answered(bool accepted, const char *reason) {
    done_ = true;
    for (int i = 0; i < MAX_OPTIONS; ++i) hide(opt_bg_[i], true);
    hide(more_, true);
    hide(go_, true);
    hide(text_, true);
    hide(chip_, true);
    hide(who_, true);
    hide(note_, false);
    lv_obj_set_pos(note_, 0, 0);
    std::string key = "refused_unknown";
    if (accepted) key = "sent";
    else if (!std::strcmp(reason, "answered")) key = "refused_answered";
    else if (!std::strcmp(reason, "stale")) key = "refused_stale";
    else if (!std::strcmp(reason, "cancelled")) key = "refused_cancelled";
    else if (!std::strcmp(reason, "ended")) key = "refused_ended";
    else if (!std::strcmp(reason, "bad_answer")) key = "refused_bad";
    else if (!std::strcmp(reason, "failed")) key = "failed";
    ui::set_text(note_, plugin_->text(key.c_str()));
    ui::set_color(note_, theme::INK);
    place_pc();
    close_in_ticks_ = accepted ? 2 : 4;
  }

  void failed() {
    failed_ = true;
    refresh();
  }

 private:
  static bool any_chosen(const Part &part) {
    for (const Option &o : part.options) if (o.chosen) return true;
    return false;
  }

  int option_height(const Part &part, bool with_desc) const {
    bool desc = false;
    if (with_desc)
      for (const Option &o : part.options) if (o.desc[0]) desc = true;
    const int inner = ui::line_height(Font::BODY_LARGE) + (desc ? ui::line_height(Font::BODY) : 0);
    return std::max(ui::mm(ui::large() ? 9 : 7), inner + ui::px(ui::large() ? 8 : 4));
  }

  std::string note_text(const Part &part) const {
    if (part.multi) return plugin_->text("pick_several");
    return "";
  }

  void place_pc() {
    hide(pc_, false);
    const Question &q = plugin_->question();
    const bool half = q.valid && !done_ && part_ < q.parts.size() && std::strcmp(q.status, "open") == 0;
    lv_obj_set_size(pc_, half ? width_ / 2 - gap_ / 2 : width_, key_h_);
    lv_obj_set_pos(pc_, 0, height_ - key_h_);
  }

  // The part is answered: the next one, or the whole answer to the integration.
  void advance() {
    Question &q = plugin_->question();
    if (part_ + 1 < q.parts.size()) {
      part_ += 1;
      opt_page_ = 0;
      refresh();
      return;
    }
    hide(go_, true);
    hide(more_, true);
    for (int i = 0; i < MAX_OPTIONS; ++i) hide(opt_bg_[i], true);
    hide(note_, false);
    ui::set_text(note_, plugin_->text("loading"));
    if (!plugin_->send_answer()) answered(false, "failed");
  }

  ClaudeSessions *plugin_;
  char handle_[12] = "";
  lv_obj_t *parent_{}, *chip_{}, *chip_text_{}, *who_{}, *text_{}, *note_{}, *pc_{}, *pc_text_{}, *go_{}, *go_text_{};
  lv_obj_t *more_{}, *more_text_{};
  int opt_page_ = 0, opt_first_ = 0;
  lv_obj_t *opt_bg_[MAX_OPTIONS]{}, *opt_mark_[MAX_OPTIONS]{}, *opt_label_[MAX_OPTIONS]{}, *opt_desc_[MAX_OPTIONS]{};
  int width_ = 0, height_ = 0, gap_ = 0, pad_ = 0, chip_h_ = 0, key_h_ = 0;
  size_t part_ = 0;
  bool done_ = false, failed_ = false;
  int close_in_ticks_ = 0;
};

// ---- The popup: a new question (a tap opens it), or a session waiting for approval on the PC ----

class AskPopup : public tessera::Popup {
 public:
  explicit AskPopup(ClaudeSessions *plugin) : plugin_(plugin) {}
  ~AskPopup() override {
    if (plugin_->popup_shown_ == this) plugin_->popup_shown_ = nullptr;
  }

  void open(const tessera::PopupContext &c) override {
    const PopupView &v = plugin_->popup_view();
    const bool large = ui::large();
    const int gap = ui::px(large ? 8 : 5);
    const int key = c.key > 0 ? c.key : ui::px(large ? 48 : 32);
    // The icon in a disc, as big as the close key, on one line with it; the title beside.
    disc_ = ui::block(c.parent, theme::ACCENT);
    lv_obj_set_size(disc_, key, key);
    lv_obj_set_style_radius(disc_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(disc_, 0, 0);
    ui::set_fill(disc_, v.question ? Tone::BUSY : Tone::ALERT, theme::ACCENT);
    icon_ = ui::label(disc_, Font::ICON_SMALL, theme::ON_ACCENT);
    lv_obj_set_width(icon_, key);
    lv_obj_set_style_text_align(icon_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(icon_);
    ui::set_text(icon_, ui::icon(v.question ? ICON_ROBOT_ASKS : ICON_LOCK));
    title_ = ui::label(c.parent, Font::TITLE, theme::INK);
    const int x = key + gap;
    lv_obj_set_size(title_, std::max(1, c.width - x - c.corner), ui::line_height(Font::TITLE));
    lv_obj_set_pos(title_, x, (key - ui::line_height(Font::TITLE)) / 2);
    ui::set_text(title_, plugin_->text(v.question ? "asks_you" : "waits_pc"));
    int y = key + gap;
    who_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(who_, c.width, ui::line_height(Font::BODY));
    lv_obj_set_pos(who_, 0, y);
    ui::set_text(who_, v.name);
    y += ui::line_height(Font::BODY) + gap / 2;
    std::string text;
    for (const tessera::Text &line : v.lines) {
      if (!text.empty()) text += " ";
      text += roomy::plain(line);
    }
    text_ = ui::text(c.parent, Font::BODY_LARGE, theme::INK);
    const int lh = ui::line_height(Font::BODY_LARGE);
    const int hint_h = ui::line_height(Font::BODY) + gap;
    int lines = std::min(MAX_LINES, std::max(1, (c.height - y - hint_h) / lh));
    lv_obj_set_size(text_, c.width, lines * lh);
    lv_obj_set_pos(text_, 0, y);
    ui::set_text(text_, text);
    y += lines * lh + gap;
    hint_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_size(hint_, c.width, ui::line_height(Font::BODY));
    lv_obj_set_pos(hint_, 0, y);
    ui::set_text(hint_, plugin_->text(v.question ? "tap_to_answer" : "tap_to_close"));
    plugin_->popup_shown_ = this;
  }

  void on_theme() override {
    ui::set_color(title_, theme::INK);
    ui::set_color(who_, theme::MUTED);
    ui::set_color(text_, theme::INK);
    ui::set_color(hint_, theme::SUBTLE);
  }

  void on_tap() override {
    const PopupView &v = plugin_->popup_view();
    char handle[12];
    copy(handle, v.handle);
    const bool question = v.question;
    plugin_->popup_shown_ = nullptr;
    tessera::close_popup(plugin_);
    if (question) plugin_->open_question(handle);
  }

  void on_dismiss() override {
    if (plugin_->popup_shown_ == this) plugin_->popup_shown_ = nullptr;
  }

 private:
  ClaudeSessions *plugin_;
  lv_obj_t *disc_{}, *icon_{}, *title_{}, *who_{}, *text_{}, *hint_{};
};

// ---- The plugin ----

void ClaudeSessions::setup() {
  add_tile("overview", [this]() {
    ESP_LOGD(TAG, "Overview tile created");
    return new OverviewTile(this);
  });
  add_tile("activity", [this]() { return new ActivityTile(this); });
  add_tile("background", [this]() { return new BackgroundTile(this); });
  add_tile("limits", [this]() { return new LimitsTile(this); });
  add_card("sessions", [this]() { return new SessionsCard(this); });
  add_card("background", [this]() { return new BackgroundCard(this); });
  add_card("question", [this]() { return new QuestionCard(this); });
  add_bar_item("asks", [this]() { return bar(); });
}

int ClaudeSessions::open_count() const {
  if (open_ && open_->has_state()) return std::atoi(open_->state.c_str());
  return overview_.valid ? overview_.open : 0;
}

tessera::BarItem ClaudeSessions::bar() const {
  tessera::BarItem item;
  const int open = open_count();
  if (open <= 0) return item;
  item.shown = true;
  item.icon = ICON_ROBOT_ASKS;
  item.tone = Tone::BUSY;
  item.text = tessera::format(text("bar_questions"), open);
  return item;
}

uint32_t ClaudeSessions::ask(const char *service, JsonDocument &data) {
  JsonDocument request;
  request["ask"] = std::string("call_service:tessera_claude.") + service;
  request["data"] = data.as<JsonObjectConst>();
  return tessera::send(this, request.as<JsonObjectConst>());
}

void ClaudeSessions::request_overview() {
  JsonDocument data;
  data.to<JsonObject>();
  asked_overview_ = ask("get_overview", data);
  last_overview_ms_ = millis();
  overview_due_ = false;
}

void ClaudeSessions::request_background() {
  JsonDocument data;
  data.to<JsonObject>();
  asked_background_ = ask("get_background", data);
  last_background_ms_ = millis();
  background_due_ = false;
}

void ClaudeSessions::take_background(JsonObjectConst attributes) {
  tessera::List<BackgroundRow> fresh;
  parse_background(fresh, attributes);
  // The sensor has no notes: a row that stays (same title, same start) keeps the one it had.
  for (BackgroundRow &row : fresh) {
    for (const BackgroundRow &old : background_) {
      if (old.at == row.at && !std::strcmp(old.title, row.title)) {
        copy(row.note, old.note);
        break;
      }
    }
  }
  background_.swap(fresh);
  background_valid_ = true;
  // While the card is open, its notes come anew from the integration.
  // (Not while the card's own request is still out: its answer brings the notes.)
  if (background_card_ && !asked_background_) background_due_ = true;
}

void ClaudeSessions::clear_background() {
  background_ = tessera::List<BackgroundRow>();
  background_valid_ = false;
}

void ClaudeSessions::request_question(const char *handle) {
  JsonDocument data;
  data["question"] = handle;
  asked_question_ = ask("get_question", data);
  if (!asked_question_ && question_card_) question_card_->failed();
  last_question_ms_ = millis();
}

void ClaudeSessions::request_hint(const char *handle) {
  JsonDocument data;
  data["session"] = handle;
  asked_hint_ = ask("get_hint", data);
}

bool ClaudeSessions::send_answer() {
  if (!question_.valid) return false;
  JsonDocument data;
  data["question"] = question_.h;
  data["rev"] = question_.rev;
  JsonObject answers = data["answers"].to<JsonObject>();
  for (const Part &part : question_.parts) {
    JsonArray chosen = answers[part.id].to<JsonArray>();
    for (const Option &o : part.options) if (o.chosen) chosen.add(o.id);
  }
  asked_answer_ = ask("answer", data);
  ESP_LOGD(TAG, "Answer sent for %s rev %d (request %u)", question_.h, question_.rev, (unsigned) asked_answer_);
  return asked_answer_ != 0;
}

void ClaudeSessions::open_question(const char *handle) {
  copy(pending_question_, handle);
  question_for_popup_ = false;
  tessera::open_card(plugin_id(), "question", "", -1, text("asks_you"));
}

void ClaudeSessions::show_popup_now() {
  tessera::show_popup(this, [this]() { return new AskPopup(this); }, tessera::PopupWidth::ALERT);
}

void ClaudeSessions::handle_event(const std::string &event) {
  const size_t colon = event.find(':');
  if (colon == std::string::npos) return;
  const std::string kind = event.substr(0, colon), handle = event.substr(colon + 1);
  if (handle.empty()) return;
  if (kind == "question") {
    if (!question_popup_ || !question_popup_->state) return;
  } else if (kind == "hint") {
    if (!hint_popup_ || !hint_popup_->state) return;
  } else {
    return;
  }
  pending_event_ = event;
  pending_tries_ = 0;
  send_event_request();
}

void ClaudeSessions::remember_event(const std::string &event) {
  if (event.rfind("question:", 0) == 0) last_question_event_ = event;
  else if (event.rfind("hint:", 0) == 0) last_hint_event_ = event;
}

void ClaudeSessions::popup_switch_changed(bool question, bool on) {
  const char *prefix = question ? "question:" : "hint:";
  ESP_LOGD(TAG, "%s popup switch %s", question ? "Question" : "Hint", on ? "on" : "off");
  if (!on) {
    // Off: a popup of this kind goes now, and a request still out for one opens nothing.
    if (popup_shown_ && popup_.question == question) {
      popup_shown_ = nullptr;
      tessera::close_popup(this);
    }
    if (pending_event_.rfind(prefix, 0) == 0) {
      pending_event_.clear();
      pending_tries_ = 0;
    }
    return;
  }
  // On: the newest event of this kind is asked again; the registry's reply decides (an open question, a hint that
  // still waits).
  if (!event_seen_) return;
  const std::string &event = question ? last_question_event_ : last_hint_event_;
  if (event.empty()) return;
  if (question && open_count() <= 0) return;
  event_handled_ms_ = millis();
  handle_event(event);
}

int ClaudeSessions::notify_index() const {
  const char *mode = pending_notify_[0]                   ? pending_notify_
                     : notify_ && notify_->has_state() ? notify_->state.c_str()
                                                       : "";
  for (int i = 0; i < NOTIFY_COUNT; ++i)
    if (!std::strcmp(mode, NOTIFY_MODES[i])) return i;
  return 0;  // an integration without the attribute announces by speech
}

void ClaudeSessions::request_notify(const char *mode) {
  copy(pending_notify_, mode);
  pending_notify_ms_ = millis();
  JsonDocument data;
  data["mode"] = mode;
  asked_notify_ = ask("set_notify", data);
  ESP_LOGD(TAG, "Notify %s sent (request %u)", mode, (unsigned) asked_notify_);
  if (!asked_notify_) pending_notify_[0] = '\0';
}

void ClaudeSessions::send_event_request() {
  const size_t colon = pending_event_.find(':');
  const std::string kind = pending_event_.substr(0, colon), handle = pending_event_.substr(colon + 1);
  pending_sent_ms_ = millis();
  pending_tries_ += 1;
  ESP_LOGD(TAG, "Event %s: request %u", pending_event_.c_str(), (unsigned) pending_tries_);
  if (kind == "question") {
    question_for_popup_ = true;
    request_question(handle.c_str());
  } else {
    request_hint(handle.c_str());
  }
}

void ClaudeSessions::on_interval(uint32_t now_ms) {
  // The popup switches act at once, whether turned on the glass or in Home Assistant.
  const bool question_on = question_popup_ && question_popup_->state;
  const bool hint_on = hint_popup_ && hint_popup_->state;
  const bool question_changed = popups_seen_ && question_on != question_popup_was_;
  const bool hint_changed = popups_seen_ && hint_on != hint_popup_was_;
  question_popup_was_ = question_on;
  hint_popup_was_ = hint_on;
  popups_seen_ = true;
  if (question_changed) popup_switch_changed(true, question_on);
  if (hint_changed) popup_switch_changed(false, hint_on);
  // The chosen announcement shows until the sensor says the same, or for NOTIFY_PENDING_MS.
  if (pending_notify_[0] && ((notify_ && notify_->has_state() && notify_->state == pending_notify_) ||
                             now_ms - pending_notify_ms_ >= NOTIFY_PENDING_MS))
    pending_notify_[0] = '\0';
  bool fresh = false;
  if (event_ && event_->has_state() && !roomy::same(event_->state, seen_event_)) {
    seen_event_ = roomy::text(event_->state);
    remember_event(roomy::plain(seen_event_));
    event_seen_ = true;
    // The first value after a start is handled too: a question asked while the screen was off or away is still
    // open, and the registry's reply decides (a popup only for an open question, for a hint still waiting).
    fresh = true;
  }
  if (event_rev_ && event_rev_->has_state() && !roomy::same(event_rev_->state, seen_event_rev_)) {
    // The counter moves with every event, the text only with a new handle: a second hint of the same session keeps
    // the text, and the counter alone says it is new. The first counter after a start says nothing the text did not.
    const bool first = !event_rev_seen_;
    seen_event_rev_ = roomy::text(event_rev_->state);
    event_rev_seen_ = true;
    if (!first && !fresh && now_ms - event_handled_ms_ >= EVENT_PAIR_MS) fresh = true;
  }
  if (fresh && event_seen_) {
    event_handled_ms_ = now_ms;
    handle_event(roomy::plain(seen_event_));
  }
  // A request the app did not answer (sent while the screen still waited for the app after a start, or lost with
  // the connection) is sent again, a few times, so a question open at that moment still gets its popup.
  if (!pending_event_.empty() && now_ms - pending_sent_ms_ >= EVENT_RETRY_MS) {
    if (pending_tries_ < EVENT_RETRIES) send_event_request();
    else pending_event_.clear();
  }
  if (rev_ && rev_->has_state() && !roomy::same(rev_->state, seen_rev_)) {
    seen_rev_ = roomy::text(rev_->state);
    if (!first_rev_ && (sessions_card_ || question_card_ || popup_shown_)) overview_due_ = true;
    // A report may bring a background task's new note without changing the sensor.
    if (!first_rev_ && background_card_) background_due_ = true;
    first_rev_ = false;
  }
  if (overview_due_) {
    // Each open card asks again, at most every OVERVIEW_SPACING_MS: the registry's rev moves with every report of
    // every session (several a second while sessions poll), and a request per change flooded Home Assistant.
    bool pending = false;
    if (sessions_card_) {
      if (now_ms - last_overview_ms_ >= OVERVIEW_SPACING_MS) request_overview();
      else pending = true;
    }
    // A question card learns of a change by asking again for its question.
    if (question_card_ && question_.h[0]) {
      if (now_ms - last_question_ms_ >= OVERVIEW_SPACING_MS) {
        question_for_popup_ = false;
        request_question(question_.h);
      } else {
        pending = true;
      }
    }
    // A popup asks whether what it shows still waits.
    if (popup_shown_) {
      if (now_ms - last_check_ms_ >= OVERVIEW_SPACING_MS) {
        JsonDocument data;
        if (popup_.question) data["question"] = popup_.handle;
        else data["session"] = popup_.handle;
        asked_check_ = ask(popup_.question ? "get_question" : "get_hint", data);
        last_check_ms_ = now_ms;
      } else {
        pending = true;
      }
    }
    overview_due_ = pending;
  }
  // The background card asks for its rows with notes again after the sensor changed, spaced like the overview.
  if (background_due_ && background_card_ && now_ms - last_background_ms_ >= OVERVIEW_SPACING_MS) request_background();
  if (!background_card_) background_due_ = false;
}

void ClaudeSessions::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  const bool ok = message["ok"] | false;
  JsonObjectConst result = message["result"];
  if (re && re == asked_background_) {
    asked_background_ = 0;
    ESP_LOGD(TAG, "Background reply %u: ok %d, %u tasks", (unsigned) re, (int) ok, (unsigned) (result["tasks"].size()));
    // An integration older than 0.2.5 does not know get_background: the card keeps the sensor's rows, without notes.
    if (ok && !result.isNull() && !result["tasks"].isNull()) parse_background_result(background_, result);
    background_valid_ = true;
    if (background_card_) background_card_->paint(tessera::epoch());
    tessera::refresh();
  } else if (re && re == asked_overview_) {
    asked_overview_ = 0;
    ESP_LOGD(TAG, "Overview reply %u: ok %d, %u sessions", (unsigned) re, (int) ok, (unsigned) (result["sessions"].size()));
    if (ok && !result.isNull()) parse_overview(overview_, result);
    if (sessions_card_) sessions_card_->paint();
    tessera::refresh();
  } else if (re && re == asked_question_) {
    asked_question_ = 0;
    pending_event_.clear();
    ESP_LOGD(TAG, "Question reply %u: ok %d, %s %s", (unsigned) re, (int) ok, result["h"] | "", result["status"] | "");
    if (ok && !result.isNull()) {
      parse_question(question_, result);
      if (question_for_popup_) {
        question_for_popup_ = false;
        if (question_.valid && !std::strcmp(question_.status, "open") && question_popup_ && question_popup_->state) {
          popup_.question = true;
          copy(popup_.handle, question_.h);
          copy(popup_.name, question_.name);
          popup_.lines = question_.parts[0].lines;
          show_popup_now();
        }
      } else if (question_card_) {
        question_card_->refresh();
      }
    } else if (question_card_) {
      question_card_->failed();
    }
  } else if (re && re == asked_hint_) {
    asked_hint_ = 0;
    pending_event_.clear();
    ESP_LOGD(TAG, "Hint reply %u: ok %d, %s %s", (unsigned) re, (int) ok, result["h"] | "", result["state"] | "");
    if (ok && !result.isNull() && !std::strcmp(result["state"] | "", "perm") && hint_popup_ && hint_popup_->state) {
      popup_.question = false;
      copy(popup_.handle, result["h"] | "");
      copy(popup_.name, result["name"] | "");
      popup_.lines.clear();
      popup_.lines.push_back(roomy::text(std::string(result["detail"] | "")));
      show_popup_now();
    }
  } else if (re && re == asked_check_) {
    asked_check_ = 0;
    // Answered on another screen or at the PC, or the approval was given: the popup has nothing left to say.
    const bool waits = popup_.question ? !std::strcmp(result["status"] | "", "open") : !std::strcmp(result["state"] | "", "perm");
    ESP_LOGD(TAG, "Popup check %u: ok %d, still waits %d", (unsigned) re, (int) ok, (int) waits);
    if (ok && !result.isNull() && !waits && popup_shown_) {
      popup_shown_ = nullptr;
      tessera::close_popup(this);
    }
  } else if (re && re == asked_notify_) {
    asked_notify_ = 0;
    ESP_LOGD(TAG, "Notify reply %u: ok %d, %s", (unsigned) re, (int) ok, result["notify"] | "");
    // Refused (an older integration, no consent for the service): the row goes back to what the sensor says.
    if (!ok || result.isNull()) pending_notify_[0] = '\0';
    else if (pending_notify_[0] && result["notify"].is<const char *>()) copy(pending_notify_, result["notify"] | "");
  } else if (re && re == asked_answer_) {
    asked_answer_ = 0;
    ESP_LOGD(TAG, "Answer reply %u: ok %d, accepted %d, reason %s", (unsigned) re, (int) ok, (int) (result["accepted"] | false),
             result["reason"] | "");
    if (question_card_) {
      if (!ok || result.isNull()) question_card_->answered(false, "failed");
      else question_card_->answered(result["accepted"] | false, result["reason"] | "");
    }
  }
}

void ClaudeSessions::on_cards_closed() {
  // The cards clear themselves in their destructors; nothing of them is kept here.
}

bool ClaudeSessions::settings(tessera::SettingsPage &page) {
  page.icon = "\U000F06A9";
  page.toggle(text("setting_question_popup"), [this]() { return question_popup_ && question_popup_->state; },
              [this](bool on) { if (question_popup_) { if (on) question_popup_->turn_on(); else question_popup_->turn_off(); } });
  page.toggle(text("setting_hint_popup"), [this]() { return hint_popup_ && hint_popup_->state; },
              [this](bool on) { if (hint_popup_) { if (on) hint_popup_->turn_on(); else hint_popup_->turn_off(); } });
  // The announcement lives in the integration (its options and tessera_claude.set_notify), one value for every screen.
  page.choice(text("setting_notify"), {text("notify_speech"), text("notify_tone"), text("notify_none")},
              [this]() { return notify_index(); },
              [this](int index) { if (index >= 0 && index < NOTIFY_COUNT) request_notify(NOTIFY_MODES[index]); });
  page.info(text("setting_sessions"), [this]() -> std::string {
    const Overview &o = overview_;
    if (!o.valid) return text("limit_unknown");
    return tessera::format(text("sessions"), o.count) + " · " + tessera::format(text("questions"), open_count());
  });
  page.card(text("open_card"), "\U000F06A9", "sessions");
  return true;
}

}  // namespace esphome::claude_sessions
