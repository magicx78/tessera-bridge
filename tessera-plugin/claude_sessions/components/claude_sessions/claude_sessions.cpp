#include "claude_sessions.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
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

static constexpr int MAX_TILE_ROWS = 8;
static constexpr int MAX_PAGE_ROWS = 8;
static constexpr int MAX_LINES = 6;
static constexpr int MAX_OPTIONS = 4;
static constexpr uint32_t OVERVIEW_SPACING_MS = 1500;
// A question or hint request without a reply is repeated after this long, this many times in all.
static constexpr uint32_t EVENT_RETRY_MS = 10000;
static constexpr uint8_t EVENT_RETRIES = 6;

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
  JsonArrayConst h = a["h"], names = a["names"], states = a["states"], ctx = a["ctx"], asks = a["asks"];
  const size_t n = h.size();
  for (size_t i = 0; i < n; ++i) {
    SessionRow row;
    copy(row.h, h[i] | "");
    copy(row.name, i < names.size() ? (names[i] | "") : "");
    copy(row.state, i < states.size() ? (states[i] | "idle") : "idle");
    row.ctx = i < ctx.size() ? (ctx[i] | -1) : -1;
    row.asks = i < asks.size() ? (asks[i] | 0) : 0;
    o.rows.push_back(row);
  }
  o.count = std::atoi(data["state"] | "0");
  o.open = a["open"] | 0;
  o.l5h = a["l5h"] | -1;
  o.lwk = a["lwk"] | -1;
  o.l5h_at = a["l5h_at"] | 0u;
  o.lwk_at = a["lwk_at"] | 0u;
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
      // Nothing to answer: say so and offer the way out.
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
  add_card("sessions", [this]() { return new SessionsCard(this); });
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
  if (event_ && event_->has_state() && !roomy::same(event_->state, seen_event_)) {
    seen_event_ = roomy::text(event_->state);
    // The first value after a start is handled too: a question asked while the screen was off or away is still
    // open, and the registry's reply decides (a popup only for an open question, for a hint still waiting).
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
    if (!first_rev_ && (sessions_card_ || question_card_)) overview_due_ = true;
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
    overview_due_ = pending;
  }
}

void ClaudeSessions::on_message(JsonObjectConst message) {
  const uint32_t re = message["re"] | 0u;
  const bool ok = message["ok"] | false;
  JsonObjectConst result = message["result"];
  if (re && re == asked_overview_) {
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
        if (question_.valid && !std::strcmp(question_.status, "open")) {
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
    if (ok && !result.isNull() && !std::strcmp(result["state"] | "", "perm")) {
      popup_.question = false;
      copy(popup_.handle, result["h"] | "");
      copy(popup_.name, result["name"] | "");
      popup_.lines.clear();
      popup_.lines.push_back(roomy::text(std::string(result["detail"] | "")));
      show_popup_now();
    }
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
  page.info(text("setting_sessions"), [this]() -> std::string {
    const Overview &o = overview_;
    if (!o.valid) return text("limit_unknown");
    return tessera::format(text("sessions"), o.count) + " · " + tessera::format(text("questions"), open_count());
  });
  page.card(text("open_card"), "\U000F06A9", "sessions");
  return true;
}

}  // namespace esphome::claude_sessions
