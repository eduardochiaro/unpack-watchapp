#include "ui.h"
#include "game.h"
#include "events.h"

// The readout band under the status line: the P/M/W pools in a column on the
// left, the highlighted option's icon in the middle, the running action and the
// clock on the right. Emery and gabbro have the pixels for a chunkier progress
// bar and a bigger icon; the narrower screens get a 24px one.
#if PBL_DISPLAY_WIDTH >= 200
#define BAR_H          8
#define ART_H         50
#else
#define BAR_H          3
#define ART_H         36
#endif
#define STAT_H        16     // one line of the P/M/W column

// A round screen runs the same three columns inside the circle rather than
// across the full width. The cap is too narrow to hold a row of text, so the
// columns start below it, and each row down the band is inset by the circle's
// own chord at that height -- so the columns widen as they descend instead of
// every row crowding into the narrowest one.
#ifdef PBL_ROUND
#if PBL_DISPLAY_WIDTH >= 200
#define BAND_TOP      24     // gabbro, r=130
#else
#define BAND_TOP      22     // chalk, r=90
#endif
#define BAND_BIAS      9     // icon nudged off-centre, see band_update
#else
#define BAND_TOP       0
#define BAND_PAD       3
#define BAND_BIAS      0
#endif
#if ART_H >= 3 * STAT_H
#define BAND_H        (BAND_TOP + ART_H)
#else
#define BAND_H        (BAND_TOP + 3 * STAT_H)
#endif

#define ROW_H         34
#define ROOT_PAD       5     // main-menu rows sit this far below their cell top
// The "UNPACK" band above the splash art. Wide screens have the room for a
// bigger face, so they get one and a taller band to sit in.
#if PBL_DISPLAY_WIDTH >= 200
#define TITLE_FONT    FONT_KEY_GOTHIC_24_BOLD
#define TITLE_H       34
#define TITLE_PAD      6
#else
#define TITLE_FONT    FONT_KEY_GOTHIC_14_BOLD
#define TITLE_H       21
#define TITLE_PAD      4
#endif

static Window *s_menu_window, *s_main_window, *s_event_window, *s_ledger_window;
static Layer *s_band_layer, *s_event_layer, *s_masthead;
static GBitmap *s_splash, *s_art;
static uint32_t s_art_id;   // resource currently in s_art, 0 = none loaded
static MenuLayer *s_root_menu, *s_menu;
static ScrollLayer *s_scroll;
static TextLayer *s_ledger_text;
static AppTimer *s_timer;
static bool s_running;      // the sim clock only runs while the main screen is up
static bool s_has_save;     // a session worth offering "Continue" for

static uint8_t s_idle_accum;
static uint8_t s_event_sel;
static uint32_t s_struct_sig;
static char s_ledger[LOG_MAX * (LOG_LEN + 1) + 8];

static GFont s_f14, s_f14b, s_title_font, s_fevent, s_feventb;

typedef struct { ActionKind act; } OpRow;
static OpRow s_ops[10];
static uint8_t s_ops_n;

static void ui_show_ledger(void);
static void ui_show_guide(void);
static void ui_show_event(void);

// ---- session persistence ---------------------------------------------------
// GameState is far larger than the 256-byte cap on a single persist key, so it
// is written as a run of chunks. The meta key is written last and validated
// first, so a half-finished write can never be mistaken for a save.

#define SAVE_VERSION   2   // Body gained a life stage; v1 blobs are not readable
#define KEY_META       0
#define KEY_CHUNK_BASE 1
#define SAVE_CHUNK     256
#define SAVE_CHUNKS    ((sizeof(GameState) + SAVE_CHUNK - 1) / SAVE_CHUNK)

static void session_clear(void) {
  persist_delete(KEY_META);
  for (uint32_t k = 0; k < SAVE_CHUNKS; k++) persist_delete(KEY_CHUNK_BASE + k);
}

static void session_save(void) {
  // A finished mission is a ledger, not a session to come back to.
  if (g.body_count == 0 || g.phase == PHASE_OVER) { session_clear(); return; }

  persist_delete(KEY_META);
  const uint8_t *p = (const uint8_t *)&g;
  for (uint32_t off = 0, k = 0; off < sizeof(GameState); off += SAVE_CHUNK, k++) {
    size_t len = sizeof(GameState) - off;
    if (len > SAVE_CHUNK) len = SAVE_CHUNK;
    if (persist_write_data(KEY_CHUNK_BASE + k, p + off, len) < (int)len) {
      session_clear();       // out of storage: no save beats half a save
      return;
    }
  }
  uint32_t meta[2] = { SAVE_VERSION, (uint32_t)sizeof(GameState) };
  persist_write_data(KEY_META, meta, sizeof(meta));
}

static bool session_load(void) {
  uint32_t meta[2];
  if (persist_read_data(KEY_META, meta, sizeof(meta)) != (int)sizeof(meta)) return false;
  if (meta[0] != SAVE_VERSION || meta[1] != sizeof(GameState)) { session_clear(); return false; }

  uint8_t *p = (uint8_t *)&g;
  for (uint32_t off = 0, k = 0; off < sizeof(GameState); off += SAVE_CHUNK, k++) {
    size_t len = sizeof(GameState) - off;
    if (len > SAVE_CHUNK) len = SAVE_CHUNK;
    if (persist_read_data(KEY_CHUNK_BASE + k, p + off, len) != (int)len) goto bad;
  }
  if (g.body_count == 0 || g.body_count > MAX_BODIES || g.phase == PHASE_OVER) goto bad;

  events_reactivate();   // a session saved mid-panel comes back to that panel
  return true;

bad:
  memset(&g, 0, sizeof(g));
  session_clear();
  return false;
}

// ---- colours ---------------------------------------------------------------

static GColor col_bg(void)     { return GColorBlack; }
static GColor col_fg(void)     { return GColorWhite; }
static GColor col_accent(void) { return PBL_IF_COLOR_ELSE(GColorMalachite, GColorWhite); }

static GColor col_level(int16_t v, int16_t warn, int16_t crit) {
#ifdef PBL_COLOR
  if (v <= crit) return GColorRed;
  if (v <= warn) return GColorChromeYellow;
  return GColorWhite;
#else
  (void)v; (void)warn; (void)crit;
  return GColorWhite;
#endif
}

// ---- shared cell drawing ---------------------------------------------------

// `pad` is dead space above the title, for rows that want to sit lower in the cell.
static void draw_cell(GContext *ctx, const Layer *cell, const char *title, const char *sub,
                      int16_t pad) {
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  const GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  graphics_context_set_text_color(ctx, hl ? GColorBlack : col_fg());
  graphics_draw_text(ctx, title, s_f14b, GRect(4, pad - 3, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
  graphics_draw_text(ctx, sub, s_f14, GRect(4, pad + 13, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
}

static void draw_header(GContext *ctx, const Layer *cell, const char *text) {
  graphics_context_set_text_color(ctx, col_accent());
  GRect b = layer_get_bounds(cell);
  graphics_draw_text(ctx, text, s_f14b, GRect(3, -3, b.size.w - 6, 18),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

// ---- row model -------------------------------------------------------------

static void build_ops(void) {
  s_ops_n = 0;
  if (!g.system_scanned) s_ops[s_ops_n++].act = ACT_SCAN_SYSTEM;
  s_ops[s_ops_n++].act = ACT_BUILD_POWER;
  s_ops[s_ops_n++].act = ACT_BUILD_WORKER;
  s_ops[s_ops_n++].act = ACT_BUILD_FACTORY;
  s_ops[s_ops_n++].act = ACT_FRAME;
  s_ops[s_ops_n++].act = ACT_RING;
  s_ops[s_ops_n++].act = ACT_LOG;
  s_ops[s_ops_n++].act = ACT_GUIDE;
}

// Cheap signature of everything that changes the *shape* of the menu.
static uint32_t struct_sig(void) {
  uint32_t s = (uint32_t)g.system_scanned + g.log_count * 31u + g.phase * 7u;
  for (uint8_t i = 0; i < g.body_count; i++) {
    const Body *b = &g.bodies[i];
    s = s * 3 + (uint32_t)(b->scanned | (b->rig << 1) | ((b->remaining > 0) << 2) |
                           (b->forfeited << 3) | (b->monitored << 4));
  }
  return s;
}

// ---- option art ------------------------------------------------------------
// Every row has an icon in the band under the HUD. Only the highlighted row's
// bitmap is resident: aplite has a few kilobytes of heap for the whole app, so
// twelve resident bitmaps is not a trade it can make.

static uint32_t art_for_selection(void) {
  MenuIndex idx = menu_layer_get_selected_index(s_menu);

  if (idx.section == 0) {
    if (!g.system_scanned || idx.row >= g.body_count) return RESOURCE_ID_ART_SURVEY;
    switch (g.bodies[idx.row].type) {
      case BT_ASTEROID: return RESOURCE_ID_ART_ASTEROID;
      case BT_MOON:     return RESOURCE_ID_ART_MOON;
      case BT_PLANET:   return RESOURCE_ID_ART_PLANET;
      default:          return RESOURCE_ID_ART_ANOMALY;
    }
  }

  if (idx.row >= s_ops_n) return RESOURCE_ID_ART_SURVEY;
  switch (s_ops[idx.row].act) {
    case ACT_BUILD_POWER:   return RESOURCE_ID_ART_POWER;
    case ACT_BUILD_WORKER:  return RESOURCE_ID_ART_WORKER;
    case ACT_BUILD_FACTORY: return RESOURCE_ID_ART_FACTORY;
    case ACT_FRAME:         return RESOURCE_ID_ART_FRAME;
    case ACT_RING:          return RESOURCE_ID_ART_RING;
    case ACT_LOG:           return RESOURCE_ID_ART_LOG;
    case ACT_GUIDE:         return RESOURCE_ID_ART_GUIDE;
    default:                return RESOURCE_ID_ART_SURVEY;
  }
}

// Swap the resident bitmap if the highlighted row calls for a different one.
static void art_sync(void) {
  uint32_t want = art_for_selection();
  if (want == s_art_id && s_art) return;

  if (s_art) { gbitmap_destroy(s_art); s_art = NULL; }
  s_art = gbitmap_create_with_resource(want);
  s_art_id = s_art ? want : 0;
  if (s_band_layer) layer_mark_dirty(s_band_layer);
}

static int16_t art_width(void) {
  return s_art ? gbitmap_get_bounds(s_art).size.w : 0;
}

static void draw_art(GContext *ctx, GRect cell) {
  if (!s_art) return;
  GSize sz = gbitmap_get_bounds(s_art).size;
  graphics_draw_bitmap_in_rect(ctx, s_art,
                               GRect(cell.origin.x + (cell.size.w - sz.w) / 2,
                                     cell.origin.y + (cell.size.h - sz.h) / 2, sz.w, sz.h));
}

static void menu_selection_changed(MenuLayer *m, MenuIndex now, MenuIndex before, void *c) {
  art_sync();
}

// ---- readout band ----------------------------------------------------------

// Progress bar for the running action; a thin rule when idle.
static void draw_bar(GContext *ctx, GRect r) {
  if (g.phase == PHASE_ACTION && g.action_total > 0) {
    uint16_t done = g.action_total - g.action_left;
    graphics_context_set_stroke_color(ctx, col_fg());
    graphics_draw_rect(ctx, r);
    graphics_context_set_fill_color(ctx, col_accent());
    graphics_fill_rect(ctx, GRect(r.origin.x, r.origin.y,
                                  (int16_t)((r.size.w * done) / g.action_total), r.size.h),
                       0, GCornerNone);
  } else {
    graphics_context_set_fill_color(ctx, col_fg());
    graphics_fill_rect(ctx, GRect(r.origin.x, r.origin.y + r.size.h / 2, r.size.w, 1),
                       0, GCornerNone);
  }
}

// How far in from the edge row `y` has to start. A rectangle answers the same
// for every row; a circle gives back its chord, so the rows step outwards as
// they go down and the band follows the bezel instead of squaring off inside it.
static int16_t band_pad(int16_t y) {
#ifdef PBL_ROUND
  const int32_t r = PBL_DISPLAY_WIDTH / 2;
  int32_t dy = r - y, w2 = r * r - dy * dy, w = 0;
  if (w2 < 0) w2 = 0;
  while ((w + 1) * (w + 1) <= w2) w++;
  return (int16_t)(r - w);
#else
  (void)y;
  return BAND_PAD;
#endif
}

static void draw_stat(GContext *ctx, char tag, int16_t v, GColor c, GRect r) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%c%d", tag, v);
  graphics_context_set_text_color(ctx, c);
  graphics_draw_text(ctx, buf, s_f14b, r,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void band_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  char buf[24];

  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // The icon is centred, then nudged left by BAND_BIAS: the pool figures are
  // three characters wide, an action name and "T+9999yr" are not, so the two
  // columns want different room. On a rectangle there is enough of both and the
  // bias is zero. The icon keeps its column; only the outer edges follow the
  // curve, row by row.
  const int16_t gap = 3;
  const int16_t iw = art_width();
  const int16_t ix = (b.size.w - iw) / 2 - BAND_BIAS;
  const int16_t rx = ix + iw + gap;

  for (uint8_t line = 0; line < 3; line++) {
    int16_t y = BAND_TOP + line * STAT_H, pad = band_pad(y);
    GRect l = GRect(pad, y - 3, ix - pad - gap, 18);
    GRect r = GRect(rx, y - 3, b.size.w - pad - rx, 18);

    switch (line) {
      case 0:
        draw_stat(ctx, 'P', g.power, col_fg(), l);
        graphics_context_set_text_color(ctx, col_fg());
        graphics_draw_text(ctx, g.phase == PHASE_ACTION ? game_action_name(g.action) : "IDLE",
                           s_f14b, r, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
        break;
      case 1:
        draw_stat(ctx, 'M', g.materials, col_level(g.materials, 20, 8), l);
        snprintf(buf, sizeof(buf), "T+%luyr", (unsigned long)g.cycle);
        graphics_context_set_text_color(ctx, col_fg());
        graphics_draw_text(ctx, buf, s_f14, r, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
        break;
      default:
        draw_stat(ctx, 'W', g.workers, col_fg(), l);
        draw_bar(ctx, GRect(rx, y + (STAT_H - BAR_H) / 2, r.size.w, BAR_H));
        break;
    }
  }

  draw_art(ctx, GRect(ix, BAND_TOP, iw, b.size.h - BAND_TOP - 1));

  // A rule closes the band off from the rows, the way the masthead does.
  graphics_context_set_stroke_color(ctx, col_fg());
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w, b.size.h - 1));
}

// ---- menu ------------------------------------------------------------------

static uint16_t menu_sections(MenuLayer *m, void *ctx) { return 2; }

static uint16_t menu_rows(MenuLayer *m, uint16_t section, void *ctx) {
  if (section == 0) return g.system_scanned ? g.body_count : 1;
  return s_ops_n;
}

static int16_t menu_header_h(MenuLayer *m, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static int16_t menu_cell_h(MenuLayer *m, MenuIndex *idx, void *ctx) { return ROW_H; }

static void menu_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *c) {
  draw_header(ctx, cell, section == 0 ? "SYSTEM" : "OPS");
}

static void body_row_text(uint8_t i, char *title, size_t tn, char *sub, size_t sn) {
  const Body *b = &g.bodies[i];
  snprintf(title, tn, "%s %s", b->name, body_type_name(b->type));

  if (!b->scanned) {
    snprintf(sub, sn, "unscanned - dist %s", tier_name(b->distance));
  } else if (b->forfeited) {
    snprintf(sub, sn, "left intact");
  } else if (b->monitored) {
    snprintf(sub, sn, "MONITORED - %d left", b->remaining);
  } else if (b->remaining <= 0) {
    snprintf(sub, sn, "depleted");
  } else if (b->rig) {
    snprintf(sub, sn, "RIG - %d left", b->remaining);
  } else {
    snprintf(sub, sn, "%s yield%s - %d", tier_name(b->yield), b->bio ? " BIO" : "", b->remaining);
  }
}

static void op_row_text(ActionKind k, char *title, size_t tn, char *sub, size_t sn) {
  switch (k) {
    case ACT_SCAN_SYSTEM:
      snprintf(title, tn, "System survey");
      snprintf(sub, sn, "free %dc", game_duration(k, 0));
      break;
    case ACT_BUILD_POWER:
      snprintf(title, tn, "Power array (%d)", g.arrays);
      snprintf(sub, sn, "%dM %dc", game_cost_mat(k), game_duration(k, 0));
      break;
    case ACT_BUILD_WORKER:
      snprintf(title, tn, "Worker unit (%d)", g.workers);
      snprintf(sub, sn, "%dM %dP %dc", game_cost_mat(k), game_cost_pow(k), game_duration(k, 0));
      break;
    case ACT_BUILD_FACTORY:
      snprintf(title, tn, "Factory (%d)", g.factories);
      snprintf(sub, sn, "%dM %dP %dc", game_cost_mat(k), game_cost_pow(k), game_duration(k, 0));
      break;
    case ACT_FRAME:
      snprintf(title, tn, "Colony frame (%d/%d)", g.frames, RING_FRAMES);
      snprintf(sub, sn, "%dM %dP %dW %dc", game_cost_mat(k), game_cost_pow(k), FRAME_WORKERS, game_duration(k, 0));
      break;
    case ACT_RING:
      snprintf(title, tn, "Orbital ring");
      // The ring is gated on the rest of the chain, so the row reports what is
      // still missing rather than a price the player cannot act on yet.
      if (g.frames < RING_FRAMES || g.factories < RING_FACTORIES)
        {
          int nf = RING_FRAMES - g.frames, nk = RING_FACTORIES - g.factories;
          if (nf < 0) nf = 0;
          if (nk < 0) nk = 0;
          snprintf(sub, sn, "%d frame%s, %d factor%s", nf, nf == 1 ? "" : "s",
                   nk, nk == 1 ? "y" : "ies");
        }
      else
        snprintf(sub, sn, "%dM %dP %dW %dc", game_cost_mat(k), game_cost_pow(k), RING_WORKERS, game_duration(k, 0));
      break;
    case ACT_GUIDE:
      snprintf(title, tn, "Guide");
      snprintf(sub, sn, "what the readouts mean");
      break;
    default:
      snprintf(title, tn, "Mission log");
      snprintf(sub, sn, "%d entries", g.log_count);
      break;
  }
  // The two reading screens cost nothing, so they never carry the "short" tag,
  // and a ring already reporting its missing prerequisites does not need it.
  bool gated_ring = (k == ACT_RING && (g.frames < RING_FRAMES || g.factories < RING_FACTORIES));
  if (k != ACT_LOG && k != ACT_GUIDE && !gated_ring && !game_affordable(k, 0)) {
    size_t l = strlen(sub);
    snprintf(sub + l, sn - l, " - short");
  }
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  char title[32] = "", sub[36] = "";

  if (idx->section == 0) {
    if (!g.system_scanned) {
      snprintf(title, sizeof(title), "No survey data");
      snprintf(sub, sizeof(sub), "run the system survey");
    } else {
      body_row_text((uint8_t)idx->row, title, sizeof(title), sub, sizeof(sub));
    }
  } else {
    op_row_text(s_ops[idx->row].act, title, sizeof(title), sub, sizeof(sub));
  }

  draw_cell(ctx, cell, title, sub, 0);
}

static void menu_select(MenuLayer *m, MenuIndex *idx, void *c) {
  if (g.phase == PHASE_OVER) { ui_show_ledger(); return; }

  ActionKind act = ACT_LOG;
  uint8_t target = 0;

  if (idx->section == 0) {
    if (!g.system_scanned) return;
    target = (uint8_t)idx->row;
    Body *b = &g.bodies[target];
    if (!b->scanned)                                    act = ACT_SCAN_BODY;
    else if (!b->rig && b->remaining > 0 && !b->forfeited) act = ACT_BUILD_RIG;
    else return;
  } else {
    act = s_ops[idx->row].act;
    if (act == ACT_LOG)   { ui_show_ledger(); return; }
    if (act == ACT_GUIDE) { ui_show_guide(); return; }
  }

  if (g.phase != PHASE_IDLE || !game_affordable(act, target)) {
    vibes_short_pulse();   // one action at a time, and only what you can pay for
    return;
  }
  game_start_action(act, target);
  layer_mark_dirty(s_band_layer);
  menu_layer_reload_data(s_menu);
  art_sync();
}

// ---- event panel -----------------------------------------------------------

// The whole panel scales together: emery and gabbro have the pixels for a
// finger-sized choice row, a bigger face in it, and the same face for the header
// and the prose; the 144-wide screens keep the compact row and the 14pt text.
// On a round screen the bottom of the display is the narrowest part of the
// circle, so the choice stack is lifted clear of the pinch rather than run to
// the edge.
#if PBL_DISPLAY_WIDTH >= 200
#define CHOICE_H      30
#define CHOICE_DY      3     // text inset inside its row, centres the glyphs
#define CHOICE_BOT    PBL_IF_ROUND_ELSE(26, 4)
#define EVENT_FONT    FONT_KEY_GOTHIC_18
#define EVENT_FONT_B  FONT_KEY_GOTHIC_18_BOLD
#define EVENT_HDR_H   24     // the header bar has to hold the bigger face
#else
#define CHOICE_H      17
#define CHOICE_DY     -3
#define CHOICE_BOT     2
#define EVENT_FONT    FONT_KEY_GOTHIC_14
#define EVENT_FONT_B  FONT_KEY_GOTHIC_14_BOLD
#define EVENT_HDR_H   18
#endif

// A round screen cannot use its top edge: the cap is too narrow to hold the
// header bar. The panel starts below it, at the same inset the main band uses.
#ifdef PBL_ROUND
#define EVENT_TOP     BAND_TOP
#else
#define EVENT_TOP     0
#endif

// Choices are bottom-anchored so they are never pushed off screen by long text.
// Drawing and the touch hit-test both start from here, so they cannot drift.
static int16_t choices_top(int16_t h, uint8_t n) { return h - n * CHOICE_H - CHOICE_BOT; }

static void event_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int pad = PBL_IF_ROUND_ELSE(18, 4);
  const uint8_t n = events_choice_count();

  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar. On a round screen it is pulled in to the circle's chord at its
  // own top edge -- its widest row is the one furthest down -- so the whole bar
  // lands inside the bezel instead of being sliced by it.
  const int16_t hx = PBL_IF_ROUND_ELSE(band_pad(EVENT_TOP), 0);
  GRect hb = GRect(hx, EVENT_TOP, b.size.w - 2 * hx, EVENT_HDR_H);
  graphics_context_set_fill_color(ctx, col_accent());
  graphics_fill_rect(ctx, hb, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, events_header(), s_feventb,
                     GRect(hb.origin.x, hb.origin.y - 3, hb.size.w, hb.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  int16_t choices_y = choices_top(b.size.h, n);
  const GTextAlignment tal = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  const int16_t ty = EVENT_TOP + EVENT_HDR_H + 2;
  const int16_t tx = PBL_IF_ROUND_ELSE(band_pad(ty), pad);
  GRect tr = GRect(tx, ty, b.size.w - 2 * tx, choices_y - ty - 2);

  // The bigger face is what the screen is for, but the longest event text does
  // not always fit in what the choices leave -- gabbro's circle in particular
  // gives back a narrow column. Measure it and step down a size rather than
  // wrap off the bottom edge.
  GFont tf = s_fevent;
  if (graphics_text_layout_get_content_size(events_text(), tf, tr,
                                            GTextOverflowModeWordWrap, tal).h > tr.size.h) {
    tf = s_f14;
  }
  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, events_text(), tf, tr, GTextOverflowModeWordWrap, tal, NULL);

  for (uint8_t i = 0; i < n; i++) {
    GRect row = GRect(0, choices_y + i * CHOICE_H, b.size.w, CHOICE_H);
    bool sel = (i == s_event_sel);
    if (sel) {
      graphics_context_set_fill_color(ctx, col_fg());
      graphics_fill_rect(ctx, row, 0, GCornerNone);
    }
    graphics_context_set_text_color(ctx, sel ? GColorBlack : col_fg());
    graphics_draw_text(ctx, events_choice(i), s_fevent,
                       GRect(pad, row.origin.y + CHOICE_DY, b.size.w - 2 * pad, CHOICE_H + 4),
                       GTextOverflowModeTrailingEllipsis,
                       PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
  }
}

static void event_up(ClickRecognizerRef r, void *c) {
  if (s_event_sel > 0) s_event_sel--;
  layer_mark_dirty(s_event_layer);
}

static void event_down(ClickRecognizerRef r, void *c) {
  if (s_event_sel + 1 < events_choice_count()) s_event_sel++;
  layer_mark_dirty(s_event_layer);
}

static void event_select(ClickRecognizerRef r, void *c) {
  events_resolve(s_event_sel);
  window_stack_remove(s_event_window, false);
  menu_layer_reload_data(s_menu);
  art_sync();
  layer_mark_dirty(s_band_layer);
  if (g.phase == PHASE_OVER) ui_show_ledger();
}

#ifdef PBL_TOUCH
// The panel is hand-drawn, so the system touch bridge -- which only knows how to
// drive a MenuLayer or a ScrollLayer -- has nothing here to map. This window
// takes the raw touch stream instead and hit-tests the choice rows itself.
static void event_tap(const Recognizer *r, RecognizerEvent ev) {
  if (ev != RecognizerEvent_Completed) return;
  const uint8_t n = events_choice_count();
  if (n == 0) return;

  int16_t y = tap_recognizer_get_tap_point(r).y;
  int16_t top = choices_top(layer_get_bounds(s_event_layer).size.h, n);
  if (y < top) return;                        // taps on the prose are not a choice

  uint8_t row = (uint8_t)((y - top) / CHOICE_H);
  if (row >= n) row = n - 1;                  // the last row owns the bottom margin
  s_event_sel = row;
  event_select(NULL, NULL);
}
#endif

static void event_clicks(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, event_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, event_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, event_select);
  window_single_click_subscribe(BUTTON_ID_BACK, event_select);   // no deferring an event
}

static void event_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_event_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_event_layer, event_update);
  layer_add_child(root, s_event_layer);

#ifdef PBL_TOUCH
  // The bridge has to be off for this window or the system recognizers swallow
  // the gesture before ours sees it. The window owns the recognizer from here.
  window_set_touch_bridge_disabled(w, true);
  window_attach_recognizer(w, tap_recognizer_create(event_tap, NULL));
#endif
}

static void event_unload(Window *w) { layer_destroy(s_event_layer); }

static void ui_show_event(void) {
  s_event_sel = 0;
  vibes_short_pulse();
  window_stack_push(s_event_window, false);
}

// ---- ledger and guide ------------------------------------------------------

// The guide text lives in flash as a raw resource and is pulled into the heap
// only while the window is up: aplite has ~4K of heap for the whole app, and a
// permanent 1.4K string in rodata was enough to push it over on menu scrolls.
static char *s_guide;
static bool s_show_guide;

static const char s_guide_oom[] = "Guide unavailable: not enough memory.";

static void build_ledger_text(void) {
  size_t at = 0;
  s_ledger[0] = '\0';
  // The log keeps its tail, so what is missing is the start of the mission.
  if (g.log_full) at = (size_t)snprintf(s_ledger, sizeof(s_ledger), "(earlier entries dropped)\n");
  for (uint8_t i = 0; i < g.log_count && at < sizeof(s_ledger) - 2; i++) {
    int n = snprintf(s_ledger + at, sizeof(s_ledger) - at, "%s\n", g.log[i]);
    if (n < 0) break;
    at += (size_t)n;
  }
}

static void ledger_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  const char *doc;
  if (s_show_guide) {
    ResHandle h = resource_get_handle(RESOURCE_ID_GUIDE);
    size_t n = resource_size(h);
    s_guide = malloc(n + 1);
    if (s_guide) {
      resource_load(h, (uint8_t *)s_guide, n);
      s_guide[n] = '\0';
    }
    doc = s_guide ? s_guide : s_guide_oom;
  } else {
    build_ledger_text();
    doc = s_ledger;
  }

  s_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_scroll, w);
  scroll_layer_set_shadow_hidden(s_scroll, true);

  const int pad = PBL_IF_ROUND_ELSE(16, 4);
  s_ledger_text = text_layer_create(GRect(pad, 2, b.size.w - 2 * pad, 2000));
  text_layer_set_text(s_ledger_text, doc);
  text_layer_set_font(s_ledger_text, s_f14);
  text_layer_set_background_color(s_ledger_text, GColorClear);
  text_layer_set_text_color(s_ledger_text, col_fg());
  text_layer_set_overflow_mode(s_ledger_text, GTextOverflowModeWordWrap);
  text_layer_set_text_alignment(s_ledger_text, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));

  GSize sz = text_layer_get_content_size(s_ledger_text);
  text_layer_set_size(s_ledger_text, GSize(b.size.w - 2 * pad, sz.h + 8));
  scroll_layer_set_content_size(s_scroll, GSize(b.size.w, sz.h + 14));
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_ledger_text));
  layer_add_child(root, scroll_layer_get_layer(s_scroll));

  // Once the mission is over the ledger replaces the main screen, and the run
  // stops being something to come back to.
  if (g.phase == PHASE_OVER && !s_show_guide) {
    window_stack_remove(s_main_window, false);
    s_has_save = false;
    session_clear();
  }
}

static void ledger_unload(Window *w) {
  text_layer_destroy(s_ledger_text);
  scroll_layer_destroy(s_scroll);
  free(s_guide);            // the guide text is only worth heap while it is on screen
  s_guide = NULL;
}

static void ui_show_ledger(void) {
  if (window_stack_contains_window(s_ledger_window)) return;
  s_show_guide = false;
  window_stack_push(s_ledger_window, false);
}

static void ui_show_guide(void) {
  if (window_stack_contains_window(s_ledger_window)) return;
  s_show_guide = true;
  window_stack_push(s_ledger_window, false);
}

// ---- clock -----------------------------------------------------------------

static void on_timer(void *ctx) {
  s_timer = NULL;   // the handle is spent the moment its callback runs
  if (!s_running) return;

  // Reading stops the clock. The log and the guide are reference, not play, so
  // no cycle passes while either is on screen -- the timer keeps ticking over so
  // the run picks straight back up on Back.
  if (window_stack_contains_window(s_ledger_window)) {
    s_timer = app_timer_register(TICK_MS, on_timer, NULL);
    return;
  }

  bool advanced = false;

  if (g.phase == PHASE_ACTION) {
    game_tick();
    advanced = true;
  } else if (g.phase == PHASE_IDLE) {
    // Idle is not safe time: the world keeps moving, just slower.
    if (++s_idle_accum >= IDLE_DIVISOR) {
      s_idle_accum = 0;
      game_tick();
      advanced = true;
    }
  }

  // Screen ownership follows the phase, not whether a cycle happened to advance.
  if (g.phase == PHASE_OVER) {
    ui_show_ledger();
  } else if (g.phase == PHASE_EVENT && !window_stack_contains_window(s_event_window)) {
    ui_show_event();
  }

  // Ending the mission takes the main window down with it -- the ledger replaces
  // it -- and main_unload has already freed the band and the menu by now. There
  // is nothing left below to redraw, and no run left to schedule a tick for.
  if (!s_running) return;

  if (advanced) {
    layer_mark_dirty(s_band_layer);
    uint32_t sig = struct_sig();
    if (sig != s_struct_sig) {
      s_struct_sig = sig;
      build_ops();
      menu_layer_reload_data(s_menu);
      art_sync();
    } else {
      layer_mark_dirty(menu_layer_get_layer(s_menu));
    }
  }

  s_timer = app_timer_register(TICK_MS, on_timer, NULL);
}

// ---- main window -----------------------------------------------------------

static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);

  build_ops();
  s_struct_sig = struct_sig();
  s_idle_accum = 0;

  s_band_layer = layer_create(GRect(0, 0, b.size.w, BAND_H));
  layer_set_update_proc(s_band_layer, band_update);
  layer_add_child(root, s_band_layer);

  s_menu = menu_layer_create(GRect(0, BAND_H, b.size.w, b.size.h - BAND_H));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_sections,
    .get_num_rows = menu_rows,
    .get_header_height = menu_header_h,
    .get_cell_height = menu_cell_h,
    .draw_header = menu_draw_header,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
    .selection_changed = menu_selection_changed,
  });
  menu_layer_set_normal_colors(s_menu, col_bg(), col_fg());
  menu_layer_set_highlight_colors(s_menu, col_accent(), GColorBlack);
#ifdef PBL_ROUND
  menu_layer_set_center_focused(s_menu, true);
#endif
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  art_sync();

  s_running = true;
  s_timer = app_timer_register(TICK_MS, on_timer, NULL);
}

static void main_unload(Window *w) {
  s_running = false;
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  menu_layer_destroy(s_menu);
  layer_destroy(s_band_layer);
  s_band_layer = NULL;
  if (s_art) { gbitmap_destroy(s_art); s_art = NULL; }
  s_art_id = 0;
}

// ---- main menu -------------------------------------------------------------

static uint16_t root_sections(MenuLayer *m, void *ctx) { return 1; }
static uint16_t root_rows(MenuLayer *m, uint16_t section, void *ctx) { return s_has_save ? 2 : 1; }
static int16_t root_header_h(MenuLayer *m, uint16_t section, void *ctx) { return 0; }
static int16_t root_cell_h(MenuLayer *m, MenuIndex *idx, void *ctx) { return ROW_H + ROOT_PAD; }

// The masthead is a fixed band above the menu: title, rule, splash art, rule.
// It sits outside the MenuLayer so it cannot scroll away with the rows.
static void masthead_update(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);

  // Drawn here rather than through draw_header: this title has its own font and
  // padding, and the in-game headers must keep theirs.
  graphics_context_set_text_color(ctx, col_accent());
  graphics_draw_text(ctx, "UNPACK", s_title_font,
                     GRect(3, TITLE_PAD - 3, b.size.w - 6, TITLE_H),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);

  graphics_context_set_stroke_color(ctx, col_fg());
  graphics_draw_line(ctx, GPoint(0, TITLE_H), GPoint(b.size.w, TITLE_H));
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w, b.size.h - 1));

  if (s_splash) {
    GSize sz = gbitmap_get_bounds(s_splash).size;
    graphics_draw_bitmap_in_rect(ctx, s_splash,
                                 GRect((b.size.w - sz.w) / 2, TITLE_H + 1, sz.w, sz.h));
  }
}

static bool row_is_continue(uint16_t row) { return s_has_save && row == 0; }

static void root_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  char title[24], sub[36];
  if (row_is_continue(idx->row)) {
    snprintf(title, sizeof(title), "Continue");
    snprintf(sub, sizeof(sub), "T+%luyr - M%d W%d", (unsigned long)g.cycle, g.materials, g.workers);
  } else {
    snprintf(title, sizeof(title), "New session");
    snprintf(sub, sizeof(sub), s_has_save ? "discards the saved run" : "an uncharted system");
  }
  draw_cell(ctx, cell, title, sub, ROOT_PAD);
}

static void root_select(MenuLayer *m, MenuIndex *idx, void *c) {
  if (!row_is_continue(idx->row)) {
    game_new();
    s_has_save = true;
  }
  window_stack_push(s_main_window, false);
}

static void root_appear(Window *w) {
  menu_layer_reload_data(s_root_menu);   // the Continue line reflects the live run
}

static void root_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);

  s_splash = gbitmap_create_with_resource(RESOURCE_ID_SPLASH);
  int16_t art_h = s_splash ? gbitmap_get_bounds(s_splash).size.h : 0;
  int16_t top = TITLE_H + 1 + art_h + 1;

  s_masthead = layer_create(GRect(0, 0, b.size.w, top));
  layer_set_update_proc(s_masthead, masthead_update);
  layer_add_child(root, s_masthead);

  // MenuLayer counts about 16px more content than the rows it actually draws, so
  // a frame that only just fits the rows still scrolls the last one under the art.
  // The splash bitmaps are cropped to leave that slack: 51px tall on the 144-wide
  // screens, 98px on emery.
  // A round screen centres the selected row in the menu's own frame, so the
  // frame has to stop at the bottom of the screen or the row centres on a point
  // below the bezel. A rectangle keeps the overhang: it is the scroll slack.
  s_root_menu = menu_layer_create(GRect(0, top, b.size.w,
                                        PBL_IF_ROUND_ELSE(b.size.h - top, b.size.h)));
  menu_layer_set_callbacks(s_root_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = root_sections,
    .get_num_rows = root_rows,
    .get_header_height = root_header_h,
    .get_cell_height = root_cell_h,
    .draw_row = root_draw_row,
    .select_click = root_select,
  });
  menu_layer_set_normal_colors(s_root_menu, col_bg(), col_fg());
  menu_layer_set_highlight_colors(s_root_menu, col_accent(), GColorBlack);
#ifdef PBL_ROUND
  menu_layer_set_center_focused(s_root_menu, true);
#endif
  menu_layer_set_click_config_onto_window(s_root_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_root_menu));
}

static void root_unload(Window *w) {
  menu_layer_destroy(s_root_menu);
  layer_destroy(s_masthead);
  if (s_splash) { gbitmap_destroy(s_splash); s_splash = NULL; }
}

void ui_init(void) {
  s_f14  = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_f14b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_title_font = fonts_get_system_font(TITLE_FONT);
  s_fevent  = fonts_get_system_font(EVENT_FONT);
  s_feventb = fonts_get_system_font(EVENT_FONT_B);

  s_has_save = session_load();

#ifdef PBL_TOUCH
  // Opt in to system touch navigation: on emery and gabbro it scrolls the menus
  // and the ledger by finger, taps a row to select it, and swipes back. Apps are
  // opted out by default. The event panel opts back out again, see event_load.
  app_touch_navigation_enable(true);
#endif

  s_menu_window = window_create();
  window_set_background_color(s_menu_window, col_bg());
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = root_load, .unload = root_unload, .appear = root_appear });

  s_main_window = window_create();
  window_set_background_color(s_main_window, col_bg());
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_load, .unload = main_unload });

  s_event_window = window_create();
  window_set_background_color(s_event_window, col_bg());
  window_set_click_config_provider(s_event_window, event_clicks);
  window_set_window_handlers(s_event_window, (WindowHandlers) {
    .load = event_load, .unload = event_unload });

  s_ledger_window = window_create();
  window_set_background_color(s_ledger_window, col_bg());
  window_set_window_handlers(s_ledger_window, (WindowHandlers) {
    .load = ledger_load, .unload = ledger_unload });

  window_stack_push(s_menu_window, false);
}

void ui_deinit(void) {
  s_running = false;
  if (s_timer) app_timer_cancel(s_timer);
  session_save();
  window_destroy(s_ledger_window);
  window_destroy(s_event_window);
  window_destroy(s_main_window);
  window_destroy(s_menu_window);
}
