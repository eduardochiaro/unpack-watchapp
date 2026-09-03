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
#define RIGHT_W       44     // the yield/duration column on the right of a row
#define CHAIN_CELLS    3     // FAC, FRM and the ring's own state
#else
#define BAR_H          3
#define ART_H         36
#define RIGHT_W       28
#define CHAIN_CELLS    2     // 144px holds two; the ring speaks through its row
#endif
#define STAT_H        16     // one framed cell of the P/M/W column
#define RULE_W         3     // the left rule that carries state and selection
#define CHAIN_H       14     // the FAC/FRM/RING line closing the band

// A round screen runs the same three columns inside the circle rather than
// across the full width. The cap is too narrow to hold a row of text, so the
// columns start below it, and each row down the band is inset by the circle's
// own chord at that height -- so the columns widen as they descend instead of
// every row crowding into the narrowest one.
#ifdef PBL_ROUND
#if PBL_DISPLAY_WIDTH >= 200
#define BAND_TOP      24     // gabbro, r=130
#else
#define BAND_TOP      26     // chalk, r=90 -- 4px lower buys the top cell width
#endif
#define BAND_BIAS      9     // icon nudged off-centre, see band_update
#else
#define BAND_TOP       0
#define BAND_PAD       3
#define BAND_BIAS      0
#endif
#if ART_H >= 3 * STAT_H
#define BAND_BODY_H   ART_H
#else
#define BAND_BODY_H   (3 * STAT_H)
#endif
#define BAND_H        (BAND_TOP + BAND_BODY_H + CHAIN_H)

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

static void on_timer(void *ctx);

// Pull the next tick forward. A screen that froze the run polls slowly, so the
// run would otherwise sit idle for the rest of that poll after it closes.
static void clock_kick(void) {
  if (!s_running) return;
  if (s_timer) app_timer_cancel(s_timer);
  s_timer = app_timer_register(TICK_MS, on_timer, NULL);
}

static uint8_t s_event_sel;
static uint32_t s_struct_sig;
static char s_ledger[LOG_MAX * (LOG_LEN + 1) + 8];

static GFont s_f14, s_f14b, s_title_font, s_fevent, s_feventb;

// The OPS list, in order. The system survey is the only row that ever leaves:
// once it has run there is nothing to survey, so the list is read from index 1.
static const ActionKind s_ops[] = {
  ACT_SCAN_SYSTEM, ACT_BUILD_POWER, ACT_BUILD_WORKER, ACT_BUILD_FACTORY,
  ACT_FRAME, ACT_RING, ACT_LOG, ACT_GUIDE,
};
#define OPS_N   (ARRAY_LENGTH(s_ops) - (g.system_scanned ? 1 : 0))
#define OPS_AT(row) (s_ops[(row) + (g.system_scanned ? 1 : 0)])

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

// Green is reserved for "operational", so the section headers take teal and the
// alerts take chrome yellow. Everything is on the 64-colour ramp; a one-bit
// screen collapses the lot back to black and white.
#define col_bg()  GColorBlack
#define col_fg()  GColorWhite
static GColor col_accent(void) { return PBL_IF_COLOR_ELSE(GColorMalachite, GColorWhite); }
static GColor col_alert(void)  { return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite); }
#ifndef PBL_ROUND
static GColor col_crit(void)   { return PBL_IF_COLOR_ELSE(GColorRed, GColorWhite); }
#endif
static GColor col_dim(void)    { return PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite); }
static GColor col_rule(void)   { return PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite); }
#if defined(PBL_COLOR) || !defined(PBL_ROUND)
static GColor col_head(void)   { return PBL_IF_COLOR_ELSE(GColorMidnightGreen, GColorBlack); }
#endif
#ifdef PBL_COLOR
static GColor col_tint(void)   { return GColorDarkGreen; }
#endif

static GColor col_level(int16_t v, int16_t warn, int16_t crit) {
#ifdef PBL_COLOR
  if (v <= crit) return GColorRed;
  if (v <= warn) return GColorChromeYellow;
  return GColorMalachite;
#else
  (void)v; (void)warn; (void)crit;
  return GColorWhite;
#endif
}

// ---- shared cell drawing ---------------------------------------------------

// Everything a row draws. The rule down the left carries the row's state; the
// right column carries the number the row is judged on -- a body's reserve, an
// op's duration -- so the title line never has to hold both.
typedef struct {
  const char *title;
  const char *sub;
  const char *right;    // NULL for a row with nothing to report on the right
  GColor rule;          // GColorClear: no state, so the rule only shows selection
  GColor title_col;
  GColor sub_col;
  int8_t pips;          // -1 for no yield bar, else how many of three are filled
} RowSpec;

// The yield bar: filled pips for the tier, hollow for the rest. Three squares
// read as a level at a glance where "high" is one more word to parse.
#ifndef PBL_ROUND
static void draw_pips(GContext *ctx, int16_t x, int16_t y, int8_t filled) {
  const int16_t sz = 5, gap = 2, n = 3;
  x += RIGHT_W - (n * sz + (n - 1) * gap);
  for (int16_t i = 0; i < n; i++) {
    GRect p = GRect(x + i * (sz + gap), y, sz, sz);
    if (i < filled) {
      graphics_context_set_fill_color(ctx, col_fg());
      graphics_fill_rect(ctx, p, 0, GCornerNone);
    } else {
      graphics_context_set_stroke_color(ctx, col_rule());
      graphics_draw_rect(ctx, p);
    }
  }
}
#endif

// `pad` is dead space above the title, for rows that want to sit lower in the cell.
static void draw_row_cell(GContext *ctx, const Layer *cell, const RowSpec *rs, int16_t pad) {
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  const GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  GColor tc = rs->title_col, sc = rs->sub_col;
  int16_t x = 4, w = b.size.w - 8;

#ifdef PBL_COLOR
  // Selection is a rule and a tint, not a flood of green: the row text stays
  // white and legible, and the rule doubles as the row's state indicator when
  // the row is not the selected one.
  GColor rule = rs->rule;
  if (hl) {
    graphics_context_set_fill_color(ctx, col_tint());
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    if (gcolor_equal(rule, GColorClear)) rule = col_accent();
  } else {
    graphics_context_set_stroke_color(ctx, col_rule());
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(b.size.w, 0));
  }
  if (!gcolor_equal(rule, GColorClear)) {
    graphics_context_set_fill_color(ctx, rule);
    graphics_fill_rect(ctx, GRect(0, 0, RULE_W, b.size.h), 0, GCornerNone);
  }
  x += RULE_W; w -= RULE_W;
#else
  // One bit has no tint to spend, so the highlight stays an inversion.
  if (hl) { tc = GColorBlack; sc = GColorBlack; }
#endif

#ifndef PBL_ROUND
  // The circle has no width to give a second column; a rectangle does.
  if (rs->right || rs->pips >= 0) {
    w -= RIGHT_W + 2;
    int16_t rx = x + w + 2;
    if (rs->pips >= 0) draw_pips(ctx, rx, pad + 5, rs->pips);
    if (rs->right) {
      graphics_context_set_text_color(ctx, sc);
      graphics_draw_text(ctx, rs->right, s_f14, GRect(rx, pad + 13, RIGHT_W, 18),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    }
  }
#endif

  graphics_context_set_text_color(ctx, tc);
  graphics_draw_text(ctx, rs->title, s_f14b, GRect(x, pad - 3, w, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
  graphics_context_set_text_color(ctx, sc);
  graphics_draw_text(ctx, rs->sub, s_f14, GRect(x, pad + 13, w, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);
}

// A section header is a filled teal bar with its own count on the right: how
// much of the system is surveyed, whether an op is running.
static void draw_header(GContext *ctx, const Layer *cell, const char *text, const char *stat) {
  GRect b = layer_get_bounds(cell);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, col_head());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
#endif
  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, text, s_f14b, GRect(3, -3, b.size.w - 6, 18),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
#ifndef PBL_ROUND
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(col_dim(), col_fg()));
  graphics_draw_text(ctx, stat, s_f14, GRect(3, -3, b.size.w - 6, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#else
  (void)stat;
#endif
}

// ---- row model -------------------------------------------------------------

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
// bitmap is resident: twelve at once buys nothing, since eleven of them are off
// screen.

static uint32_t body_art_id(BodyType t) {
  switch (t) {
    case BT_ASTEROID: return RESOURCE_ID_ART_ASTEROID;
    case BT_MOON:     return RESOURCE_ID_ART_MOON;
    case BT_PLANET:   return RESOURCE_ID_ART_PLANET;
    default:          return RESOURCE_ID_ART_ANOMALY;
  }
}

static uint32_t art_for_selection(void) {
  MenuIndex idx = menu_layer_get_selected_index(s_menu);

  if (idx.section == 0) {
    if (!g.system_scanned || idx.row >= g.body_count) return RESOURCE_ID_ART_SURVEY;
    return body_art_id(g.bodies[idx.row].type);
  }

  if (idx.row >= OPS_N) return RESOURCE_ID_ART_SURVEY;
  switch (OPS_AT(idx.row)) {
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

// Progress bar for the running action; a thin rule when idle. The fill is a
// one-pixel dither rather than a solid block -- the same pattern the scan
// overlay uses, and it reads as machine output instead of a game meter.
static void draw_bar(GContext *ctx, GRect r) {
  if (g.phase == PHASE_ACTION && g.action_total > 0) {
    uint16_t done = g.action_total - g.action_left;
    graphics_context_set_stroke_color(ctx, col_dim());
    graphics_draw_rect(ctx, r);
    int16_t fw = (int16_t)((r.size.w * done) / g.action_total);
    graphics_context_set_stroke_color(ctx, col_fg());
    for (int16_t x = r.origin.x + 1; x < r.origin.x + fw - 1; x += 2)
      graphics_draw_line(ctx, GPoint(x, r.origin.y + 1),
                              GPoint(x, r.origin.y + r.size.h - 2));
  } else {
    graphics_context_set_fill_color(ctx, col_rule());
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

// A pool is a framed cell with its letter on the left and its figure on the
// right: a bare number is a number, a number in a labelled box is a level.
static void draw_stat(GContext *ctx, char tag, int16_t v, GColor c, GRect r) {
  char buf[12];
  graphics_context_set_stroke_color(ctx, col_rule());
  graphics_draw_rect(ctx, r);

  // The letter needs a box a 'W' fits in -- 10px turned that row into an
  // ellipsis. Gothic 14 sits low in its own 18px line box, so the text starts
  // 3px above the cell to land centred in the frame rather than on its floor --
  // the same offset the right-hand column uses, so the two line up.
  const int16_t ty = r.origin.y - 3;
  snprintf(buf, sizeof(buf), "%c", tag);
  graphics_context_set_text_color(ctx, col_dim());
  graphics_draw_text(ctx, buf, s_f14, GRect(r.origin.x + 2, ty, 12, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  snprintf(buf, sizeof(buf), "%d", v);
  graphics_context_set_text_color(ctx, c);
  graphics_draw_text(ctx, buf, s_f14b,
                     GRect(r.origin.x + 14, ty, r.size.w - 16, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

// The construction chain, on its own line closing the band. The ring is the
// goal of the run and it was nowhere on screen before; three items will not fit
// beside the clock and the bar, so they get a line of their own.
static void draw_chain(GContext *ctx, GRect b) {
  const int16_t y = BAND_TOP + BAND_BODY_H;
  const int16_t pad = band_pad(y + CHAIN_H);
  const int16_t cw = (b.size.w - 2 * pad) / CHAIN_CELLS;
  char buf[14];

  for (uint8_t i = 0; i < CHAIN_CELLS; i++) {
    GColor c = col_dim();
    if (i == 0) {
      snprintf(buf, sizeof(buf), "FAC %d/%d", g.factories, RING_FACTORIES);
      if (g.factories >= RING_FACTORIES) c = col_accent();
    } else if (i == 1) {
      snprintf(buf, sizeof(buf), "FRM %d/%d", g.frames, RING_FRAMES);
      if (g.frames >= RING_FRAMES) c = col_accent();
    } else if (g.phase == PHASE_ACTION && g.action == ACT_RING) {
      uint16_t done = g.action_total - g.action_left;
      snprintf(buf, sizeof(buf), "RING %d%%", g.action_total ? (done * 100) / g.action_total : 0);
      c = col_alert();
    } else if (g.frames >= RING_FRAMES && g.factories >= RING_FACTORIES) {
      snprintf(buf, sizeof(buf), "RING RDY");
      c = col_accent();
    } else {
      snprintf(buf, sizeof(buf), "RING LOCK");
      c = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite);
    }
    graphics_context_set_text_color(ctx, c);
    graphics_draw_text(ctx, buf, s_f14, GRect(pad + i * cw, y - 3, cw, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
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
    GRect l = GRect(pad, y + 1, ix - pad - gap, STAT_H - 2);
    GRect r = GRect(rx, y - 3, b.size.w - pad - rx, 18);

    switch (line) {
      case 0:
        draw_stat(ctx, 'P', g.power, col_level(g.power, 8, 1), l);
        snprintf(buf, sizeof(buf), "T+%luyr", (unsigned long)g.cycle);
        graphics_context_set_text_color(ctx, col_accent());
        graphics_draw_text(ctx, buf, s_f14b, r, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
        break;
      case 1:
        draw_stat(ctx, 'M', g.materials, col_level(g.materials, 20, 8), l);
        graphics_context_set_text_color(ctx, col_fg());
        graphics_draw_text(ctx, g.phase == PHASE_ACTION ? game_action_name(g.action) : "IDLE",
                           s_f14, r, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentRight, NULL);
        break;
      default:
        draw_stat(ctx, 'W', g.workers, col_level(g.workers, 1, 0), l);
        draw_bar(ctx, GRect(rx, y + (STAT_H - BAR_H) / 2, r.size.w, BAR_H));
        break;
    }
  }

  draw_art(ctx, GRect(ix, BAND_TOP, iw, BAND_BODY_H));
  draw_chain(ctx, b);

  // A rule closes the band off from the rows, the way the masthead does.
  graphics_context_set_stroke_color(ctx, col_rule());
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w, b.size.h - 1));
}

// ---- menu ------------------------------------------------------------------

static uint16_t menu_sections(MenuLayer *m, void *ctx) { return 2; }

static uint16_t menu_rows(MenuLayer *m, uint16_t section, void *ctx) {
  if (section == 0) return g.system_scanned ? g.body_count : 1;
  return OPS_N;
}

static int16_t menu_header_h(MenuLayer *m, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static int16_t menu_cell_h(MenuLayer *m, MenuIndex *idx, void *ctx) { return ROW_H; }

static void menu_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *c) {
  char stat[16];
  if (section == 0) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < g.body_count; i++) if (g.bodies[i].scanned) n++;
    if (g.system_scanned) snprintf(stat, sizeof(stat), "%d/%d SCANNED", n, g.body_count);
    else                  snprintf(stat, sizeof(stat), "NO SURVEY");
  } else {
    snprintf(stat, sizeof(stat), "%s", g.phase == PHASE_ACTION ? "1 RUNNING" : "IDLE");
  }
  draw_header(ctx, cell, section == 0 ? "SYSTEM" : "OPS", stat);
}

// Distance in the row subtitle. "near/mid/far" says the same as "dist low" in
// half the pixels, and a 144px row has to give the right-hand column its width
// out of the same line.
static const char *dist_name(uint8_t t) {
  static const char *const d[3] = { "near", "mid", "far" };
  return d[t > 2 ? 2 : t];
}

// A body row: state on the rule, reserve on the right, what it is doing in the
// subtitle. An unscanned body reports nothing it has not measured.
static void body_row_spec(uint8_t i, char *title, size_t tn, char *sub, size_t sn,
                          char *right, size_t rn, RowSpec *rs) {
  const Body *b = &g.bodies[i];
  snprintf(title, tn, "%s %s", b->name, body_type_name(b->type));

  if (!b->scanned) {
    snprintf(sub, sn, "unscanned - %s", dist_name(b->distance));
    snprintf(right, rn, "?");
    rs->title_col = col_dim();
    rs->sub_col = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite);
    rs->pips = 0;
    rs->right = right;
    return;
  }

  rs->pips = (int8_t)(b->yield + 1);
  snprintf(right, rn, "%d", b->remaining);
  rs->right = right;

  if (b->forfeited) {
    snprintf(sub, sn, "left intact");
    rs->rule = col_alert();
    rs->title_col = rs->sub_col = col_alert();
  } else if (b->monitored) {
    snprintf(sub, sn, "monitored - %s", b->bio ? life_name(b->life) : "watched");
    rs->rule = col_alert();
    rs->title_col = rs->sub_col = col_alert();
  } else if (b->remaining <= 0) {
    snprintf(sub, sn, "depleted");
    rs->title_col = col_dim();
    rs->pips = 0;
  } else if (b->rig) {
    snprintf(sub, sn, "rig - %d/c - %s", b->yield + 1, dist_name(b->distance));
    rs->rule = col_accent();
  } else {
    snprintf(sub, sn, "%s yield%s - %s", tier_name(b->yield), b->bio ? " BIO" : "",
             dist_name(b->distance));
  }
}

// An op row: price in the subtitle, duration on the right. A row that cannot be
// committed says what is missing rather than only turning amber, and a locked
// row says what unlocks it -- the cost, the wait and the block are all stated
// before the commit, never after it.
static void op_row_spec(ActionKind k, char *title, size_t tn, char *sub, size_t sn,
                        char *right, size_t rn, RowSpec *rs) {
  bool gated = false, priced = true;

  switch (k) {
    case ACT_SCAN_SYSTEM:
      snprintf(title, tn, "System survey");
      snprintf(sub, sn, "free");
      break;
    case ACT_BUILD_POWER:
      snprintf(title, tn, "Power array (%d)", g.arrays);
      snprintf(sub, sn, "%dM", game_cost_mat(k));
      break;
    case ACT_BUILD_WORKER:
      snprintf(title, tn, "Worker unit (%d)", g.workers);
      snprintf(sub, sn, "%dM %dP", game_cost_mat(k), game_cost_pow(k));
      break;
    case ACT_BUILD_FACTORY:
      snprintf(title, tn, "Factory (%d)", g.factories);
      snprintf(sub, sn, "%dM %dP", game_cost_mat(k), game_cost_pow(k));
      break;
    case ACT_FRAME:
      snprintf(title, tn, "Colony frame (%d/%d)", g.frames, RING_FRAMES);
      snprintf(sub, sn, "%dM %dP %dW", game_cost_mat(k), game_cost_pow(k), FRAME_WORKERS);
      break;
    case ACT_RING:
      snprintf(title, tn, "Orbital ring");
      if (g.frames < RING_FRAMES || g.factories < RING_FACTORIES) {
        int nf = RING_FRAMES - g.frames, nk = RING_FACTORIES - g.factories;
        if (nf < 0) nf = 0;
        if (nk < 0) nk = 0;
        snprintf(sub, sn, "locked - %dFRM %dFAC", nf, nk);
        gated = true;
      } else {
        snprintf(sub, sn, "%dM %dP %dW", game_cost_mat(k), game_cost_pow(k), RING_WORKERS);
      }
      break;
    case ACT_GUIDE:
      snprintf(title, tn, "Guide");
      snprintf(sub, sn, "what the readouts mean");
      rs->title_col = col_dim();
      priced = false;
      break;
    default:
      snprintf(title, tn, "Mission log");
      snprintf(sub, sn, "%d entries - clock holds", g.log_count);
      priced = false;
      break;
  }

  if (gated) {
    snprintf(right, rn, "-");
    rs->right = right;
    rs->title_col = col_dim();
    rs->sub_col = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite);
  } else if (priced) {
    snprintf(right, rn, "%dc", game_duration(k, 0));
    rs->right = right;
    if (!game_affordable(k)) {
      int16_t dm = (int16_t)(game_cost_mat(k) - g.materials);
      int16_t dp = (int16_t)(game_cost_pow(k) - g.power);
      int16_t dw = (int16_t)((k == ACT_FRAME ? FRAME_WORKERS :
                              k == ACT_RING  ? RING_WORKERS : 0) - g.workers);
      char need[14] = "";
      if (dm > 0)      snprintf(need, sizeof(need), "needs %dM", dm);
      else if (dp > 0) snprintf(need, sizeof(need), "needs %dP", dp);
      else if (dw > 0) snprintf(need, sizeof(need), "needs %dW", dw);
      if (need[0]) {
#if PBL_DISPLAY_WIDTH >= 200
        size_t l = strlen(sub);
        snprintf(sub + l, sn - l, " - %s", need);
#else
        // 144px cannot hold price and shortfall both, and the shortfall is the
        // half that decides whether the row can be committed at all -- so it
        // goes first and the price is what the ellipsis takes.
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%s - %.22s", need, sub);
        snprintf(sub, sn, "%s", tmp);
#endif
      }
      rs->sub_col = col_alert();
    }
  }
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  char title[32] = "", sub[40] = "", right[10] = "";
  RowSpec rs = { title, sub, NULL, GColorClear, col_fg(), col_dim(), -1 };

  if (idx->section == 0) {
    if (!g.system_scanned) {
      snprintf(title, sizeof(title), "No survey data");
      snprintf(sub, sizeof(sub), "run the system survey");
    } else {
      body_row_spec((uint8_t)idx->row, title, sizeof(title), sub, sizeof(sub),
                    right, sizeof(right), &rs);
    }
  } else {
    op_row_spec(OPS_AT(idx->row), title, sizeof(title), sub, sizeof(sub),
                right, sizeof(right), &rs);
  }

  draw_row_cell(ctx, cell, &rs, 0);
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
    act = OPS_AT(idx->row);
    if (act == ACT_LOG)   { ui_show_ledger(); return; }
    if (act == ACT_GUIDE) { ui_show_guide(); return; }
  }

  if (g.phase != PHASE_IDLE || !game_affordable(act)) {
    vibes_short_pulse();   // one action at a time, and only what you can pay for
    return;
  }
  game_start_action(act, target);
  clock_kick();                 // idle polls slowly; the action starts now
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
// Every choice carries its consequence on the line under it -- materials, a
// ledger mark, a forfeit -- so a two-line panel still refuses a clean answer.
// 144px cannot afford that per row without eating the prose, so there the
// consequence of the *selected* choice gets one shared line above the stack.
#if PBL_DISPLAY_WIDTH >= 200
#define CHOICE_H      30     // title and its consequence line
#define CHOICE_DY      0
#define CHOICE_SUB     1
#define CHOICE_BOT    PBL_IF_ROUND_ELSE(26, 4)
#define EVENT_FONT    FONT_KEY_GOTHIC_18
#define EVENT_FONT_B  FONT_KEY_GOTHIC_18_BOLD
#define EVENT_HDR_H   24     // the header bar has to hold the bigger face
#define EVENT_YR_DY    3     // Gothic 14's baseline down onto Gothic 18's
#define EVENT_RESP    13     // the RESPONSE label over the choice stack
#define EVENT_ART      1
#define EVENT_EMOJI    1     // Gothic 18 carries the emoji block; Gothic 14 does not
#else
#define CHOICE_H      17
#define CHOICE_DY     -3
#define CHOICE_SUB     0
#define CHOICE_BOT     2
#define EVENT_FONT    FONT_KEY_GOTHIC_14
#define EVENT_FONT_B  FONT_KEY_GOTHIC_14_BOLD
#define EVENT_HDR_H   18
#define EVENT_YR_DY    0     // bar and clock are the same face here
#define EVENT_RESP     0
#define EVENT_ART      0
#define EVENT_EMOJI    0
#endif

// The shared consequence line, on the screens too narrow for one per choice.
#define EVENT_CONS    (CHOICE_SUB ? 0 : 14)
// The button hint only pays for itself where there is room left over for it.
#if EVENT_ART && !defined(PBL_ROUND)
#define EVENT_FOOT    14
#else
#define EVENT_FOOT     0
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
static int16_t choices_top(int16_t h, uint8_t n) {
  return h - n * CHOICE_H - CHOICE_BOT - EVENT_FOOT;
}

#if EVENT_ART
// The panel's own bitmap, resident only while the panel is up. The band's icon
// belongs to the row behind it and says nothing about the event.
static GBitmap *s_ev_art;

static uint32_t event_art_id(void) {
  switch (events_active()) {
    case EV_BIO:
    case EV_SPACEFLIGHT:
    case EV_RIG_FAULT:
    case EV_VEIN:         return body_art_id(g.bodies[g.active_target].type);
    case EV_DRIFT:        return RESOURCE_ID_ART_WORKER;
    case EV_STORM:        return RESOURCE_ID_ART_POWER;
    case EV_INTERFERENCE:
    case EV_SEED_DEFECT:
    case EV_DIRECTIVE:    return RESOURCE_ID_ART_SURVEY;
    default:              return RESOURCE_ID_ART_ANOMALY;
  }
}
#endif

static void event_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int pad = PBL_IF_ROUND_ELSE(18, 4);
  const uint8_t n = events_choice_count();

  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar. Amber, and the only place on any screen where a fill covers the
  // full width: it is how an event announces that the clock has stopped. On a
  // round screen it is pulled in to the circle's chord at its own top edge --
  // its widest row is the one furthest down -- so the whole bar lands inside
  // the bezel instead of being sliced by it.
  const int16_t hx = PBL_IF_ROUND_ELSE(band_pad(EVENT_TOP), 0);
  GRect hb = GRect(hx, EVENT_TOP, b.size.w - 2 * hx, EVENT_HDR_H);
  graphics_context_set_fill_color(ctx, col_alert());
  graphics_fill_rect(ctx, hb, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  // Only the Gothic 18 and 24 faces carry emoji glyphs, so on the screens whose
  // header bar is Gothic 14 the leading emoji is dropped rather than drawn as a
  // hole. The name is what the bar is for.
  const char *hdr = events_header();
#if !EVENT_EMOJI
  const char *sp = ((uint8_t)hdr[0] >= 0x80) ? strchr(hdr, ' ') : NULL;
  if (sp) hdr = sp + 1;
#endif
  graphics_draw_text(ctx, hdr, s_feventb,
                     GRect(hb.origin.x + 3, hb.origin.y, hb.size.w - 6, hb.size.h),
                     GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
#ifndef PBL_ROUND
  // The clock on the bar is the point: it is frozen at this figure until the
  // panel is answered.
  char buf[16];
  snprintf(buf, sizeof(buf), "T+%luyr", (unsigned long)g.cycle);
  graphics_draw_text(ctx, buf, s_f14,
                     GRect(hb.origin.x + 3, hb.origin.y + EVENT_YR_DY,
                           hb.size.w - 6, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif

  int16_t choices_y = choices_top(b.size.h, n);
  const GTextAlignment tal = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  const int16_t ty = EVENT_TOP + EVENT_HDR_H + 2;
  const int16_t tx = PBL_IF_ROUND_ELSE(band_pad(ty), pad);
  const int16_t tw = b.size.w - 2 * tx;
  const int16_t th = choices_y - ty - 2 - EVENT_RESP - EVENT_CONS;
  GRect tr = GRect(tx, ty, tw, th);

  // The bigger face is what the screen is for, but the longest event text does
  // not always fit in what the choices leave -- gabbro's circle in particular
  // gives back a narrow column. Measure it and give up, in order, the type
  // size and then the icon, rather than wrap off the bottom edge.
  GFont tf = s_fevent;
  bool icon = false;
#if EVENT_ART
  GSize isz = GSize(0, 0);
  if (s_ev_art) {
    isz = gbitmap_get_bounds(s_ev_art).size;
    icon = true;
    tr.origin.x += isz.w + 4;
    tr.size.w   -= isz.w + 4;
  }
#endif
  if (graphics_text_layout_get_content_size(events_text(), tf, tr,
                                            GTextOverflowModeWordWrap, tal).h > tr.size.h) {
    tf = s_f14;
    if (icon && graphics_text_layout_get_content_size(events_text(), tf, tr,
                                                      GTextOverflowModeWordWrap, tal).h > tr.size.h) {
      icon = false;
      tr = GRect(tx, ty, tw, th);
      if (graphics_text_layout_get_content_size(events_text(), s_fevent, tr,
                                                GTextOverflowModeWordWrap, tal).h <= tr.size.h)
        tf = s_fevent;
    }
  }
#if EVENT_ART
  if (icon) graphics_draw_bitmap_in_rect(ctx, s_ev_art, GRect(tx, ty + 2, isz.w, isz.h));
#endif
  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, events_text(), tf, tr, GTextOverflowModeWordWrap, tal, NULL);

#if EVENT_RESP
  graphics_context_set_stroke_color(ctx, col_rule());
  graphics_draw_line(ctx, GPoint(pad, choices_y - EVENT_RESP - 2),
                          GPoint(b.size.w - pad, choices_y - EVENT_RESP - 2));
  graphics_context_set_text_color(ctx, col_dim());
  graphics_draw_text(ctx, "RESPONSE", s_f14,
                     GRect(pad, choices_y - EVENT_RESP - 3, b.size.w - 2 * pad, 18),
                     GTextOverflowModeTrailingEllipsis, tal, NULL);
#endif
#if EVENT_CONS
  graphics_context_set_text_color(ctx, col_alert());
  graphics_draw_text(ctx, events_choice_cost(s_event_sel), s_f14,
                     GRect(pad, choices_y - EVENT_CONS - 3, b.size.w - 2 * pad, 18),
                     GTextOverflowModeTrailingEllipsis, tal, NULL);
#endif

  for (uint8_t i = 0; i < n; i++) {
    GRect row = GRect(0, choices_y + i * CHOICE_H, b.size.w, CHOICE_H);
    bool sel = (i == s_event_sel);
    GColor tc = col_fg();
#if CHOICE_SUB
    GColor sc = col_dim();
#endif
    int16_t cx = pad, cw = b.size.w - 2 * pad;

#if defined(PBL_COLOR) && !defined(PBL_ROUND)
    if (sel) {
      graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
      graphics_fill_rect(ctx, row, 0, GCornerNone);
      graphics_context_set_fill_color(ctx, col_fg());
      graphics_fill_rect(ctx, GRect(0, row.origin.y, RULE_W, row.size.h), 0, GCornerNone);
    } else if (i > 0) {
      graphics_context_set_stroke_color(ctx, col_rule());
      graphics_draw_line(ctx, GPoint(pad, row.origin.y), GPoint(b.size.w - pad, row.origin.y));
    }
    cx += RULE_W; cw -= RULE_W;
#else
    if (sel) {
      graphics_context_set_fill_color(ctx, col_fg());
      graphics_fill_rect(ctx, row, 0, GCornerNone);
      tc = GColorBlack;
#if CHOICE_SUB
      sc = GColorBlack;
#endif
    }
#endif

    graphics_context_set_text_color(ctx, tc);
    graphics_draw_text(ctx, events_choice(i), CHOICE_SUB ? s_f14b : s_fevent,
                       GRect(cx, row.origin.y + CHOICE_DY, cw, CHOICE_H + 4),
                       GTextOverflowModeTrailingEllipsis,
                       PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
#if CHOICE_SUB
    graphics_context_set_text_color(ctx, sc);
    graphics_draw_text(ctx, events_choice_cost(i), s_f14,
                       GRect(cx, row.origin.y + CHOICE_DY + 14, cw, 18),
                       GTextOverflowModeTrailingEllipsis,
                       PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
#endif
  }

#if EVENT_FOOT
  graphics_context_set_stroke_color(ctx, col_rule());
  graphics_draw_line(ctx, GPoint(0, b.size.h - EVENT_FOOT),
                          GPoint(b.size.w, b.size.h - EVENT_FOOT));
  graphics_context_set_text_color(ctx, col_dim());
  GRect fr = GRect(pad, b.size.h - EVENT_FOOT - 2, b.size.w - 2 * pad, 18);
  graphics_draw_text(ctx, "UP/DOWN select", s_f14, fr,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, "SELECT confirm", s_f14, fr,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif
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
  clock_kick();
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
#if EVENT_ART
  s_ev_art = gbitmap_create_with_resource(event_art_id());   // NULL is drawn as no icon
#endif
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

static void event_unload(Window *w) {
  layer_destroy(s_event_layer);
#if EVENT_ART
  if (s_ev_art) { gbitmap_destroy(s_ev_art); s_ev_art = NULL; }
#endif
}

static void ui_show_event(void) {
  s_event_sel = 0;
  vibes_short_pulse();
  window_stack_push(s_event_window, false);
}

// ---- ledger and guide ------------------------------------------------------

// The guide text lives in flash as a raw resource and is pulled into the heap
// only while the window is up, rather than sitting in rodata for the whole run.
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

// The ledger opens with what ended the run and what it added up to: cause in
// the header bar rather than buried in the last log line, then built, extracted
// and marks. A round screen has no room for the band and keeps the scroll alone.
#ifndef PBL_ROUND
#define LED_BAR_H     16
#if PBL_DISPLAY_WIDTH >= 200
#define LED_SUM_H     28     // three framed cells, label over figure
#else
#define LED_SUM_H     16     // 144px takes the same three as one line
#endif
#define LEDGER_HDR_H  (LED_BAR_H + LED_SUM_H)

static Layer *s_led_head;

static void ledger_head_update(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  char buf[40], built[16], ext[14], marks[10];
  const char *cause = "MISSION LOG";
  GColor bar = col_head(), ink = col_fg();

  switch (g.end) {
    case END_DEPART:   cause = "DEPARTED";   bar = col_accent(); ink = GColorBlack; break;
    case END_COLLAPSE: cause = "COLLAPSE";   bar = col_crit();
                       ink = PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack); break;
    case END_SUDDEN:   cause = "TERMINATED"; bar = col_crit();
                       ink = PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack); break;
    default: break;
  }

  graphics_context_set_fill_color(ctx, bar);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, LED_BAR_H), 0, GCornerNone);
  graphics_context_set_text_color(ctx, ink);
  graphics_draw_text(ctx, cause, s_f14b, GRect(4, -1, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  snprintf(buf, sizeof(buf), "T+%luyr", (unsigned long)g.cycle);
  graphics_draw_text(ctx, buf, s_f14, GRect(4, -1, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  snprintf(built, sizeof(built), "%dF %dFR%s", g.factories, g.frames,
           g.end == END_DEPART ? " 1R" : "");
  snprintf(ext, sizeof(ext), "%lu", (unsigned long)g.extracted);
  snprintf(marks, sizeof(marks), "%d", g.harvested_bio);
  GColor mc = g.harvested_bio > 0 ? col_alert() : col_dim();

#if LED_SUM_H > 16
  const char *label[3] = { "BUILT", "TAKEN", "MARKS" };
  const char *val[3]   = { built, ext, marks };
  const int16_t cw = (b.size.w - 8) / 3;
  for (uint8_t i = 0; i < 3; i++) {
    GRect cell = GRect(4 + i * cw, LED_BAR_H + 1, cw - 2, LED_SUM_H - 3);
    GColor c = (i == 2) ? mc : col_dim();
    graphics_context_set_stroke_color(ctx, (i == 2) ? mc : col_rule());
    graphics_draw_rect(ctx, cell);
    graphics_context_set_text_color(ctx, c);
    graphics_draw_text(ctx, label[i], s_f14, GRect(cell.origin.x + 3, cell.origin.y - 4, cell.size.w - 6, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, (i == 2) ? mc : col_fg());
    graphics_draw_text(ctx, val[i], s_f14, GRect(cell.origin.x + 3, cell.origin.y + 9, cell.size.w - 6, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
#else
  snprintf(buf, sizeof(buf), "%s - %sM", built, ext);
  graphics_context_set_text_color(ctx, col_dim());
  graphics_draw_text(ctx, buf, s_f14, GRect(4, LED_BAR_H - 3, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  snprintf(buf, sizeof(buf), "%s marks", marks);
  graphics_context_set_text_color(ctx, mc);
  graphics_draw_text(ctx, buf, s_f14, GRect(4, LED_BAR_H - 3, b.size.w - 8, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
#endif

  graphics_context_set_stroke_color(ctx, col_rule());
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w, b.size.h - 1));
}
#endif  // ledger summary band

static void ledger_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  const char *doc;
  int16_t top = 0;
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

#ifndef PBL_ROUND
  // The guide is reference text and has nothing to summarise.
  if (!s_show_guide) {
    top = LEDGER_HDR_H;
    s_led_head = layer_create(GRect(0, 0, b.size.w, LEDGER_HDR_H));
    layer_set_update_proc(s_led_head, ledger_head_update);
    layer_add_child(root, s_led_head);
  }
#endif

  s_scroll = scroll_layer_create(GRect(0, top, b.size.w, b.size.h - top));
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
  clock_kick();               // Back out of the log and the run resumes at once
  text_layer_destroy(s_ledger_text);
  scroll_layer_destroy(s_scroll);
#ifndef PBL_ROUND
  if (s_led_head) { layer_destroy(s_led_head); s_led_head = NULL; }
#endif
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

// Wakeups follow the phase. A running action needs the full cadence for the
// progress bar and the clock; idle only produces a cycle every IDLE_DIVISOR
// ticks, so waking in between burns battery to do nothing; and a frozen screen
// -- the event panel, the log, the guide -- has no work at all until the user
// acts, so it polls at a rate that only exists to notice they have.
static uint32_t tick_delay(void) {
  if (window_stack_contains_window(s_ledger_window)) return TICK_MS * 5;
  if (g.phase == PHASE_ACTION) return TICK_MS;
  if (g.phase == PHASE_EVENT)  return TICK_MS * 5;
  return TICK_MS * IDLE_DIVISOR;
}

// Row subtitles count down only while a rig is depleting a body. With none
// running the list is the same pixels tick after tick, and redrawing it is
// pure drain.
static bool rows_tick(void) {
  for (uint8_t i = 0; i < g.body_count; i++) {
    if (g.bodies[i].rig && g.bodies[i].remaining > 0) return true;
  }
  return false;
}

static void on_timer(void *ctx) {
  s_timer = NULL;   // the handle is spent the moment its callback runs
  if (!s_running) return;

  // Reading stops the clock. The log and the guide are reference, not play, so
  // no cycle passes while either is on screen -- the timer keeps ticking over so
  // the run picks straight back up on Back.
  if (window_stack_contains_window(s_ledger_window)) {
    s_timer = app_timer_register(tick_delay(), on_timer, NULL);
    return;
  }

  bool advanced = false;

  if (g.phase == PHASE_ACTION) {
    game_tick();
    advanced = true;
  } else if (g.phase == PHASE_IDLE) {
    // Idle is not safe time: the world keeps moving, just slower -- one cycle
    // per IDLE_DIVISOR ticks, which is the wait tick_delay() sleeps out.
    game_tick();
    advanced = true;
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
      menu_layer_reload_data(s_menu);
      art_sync();
    } else if (rows_tick()) {
      layer_mark_dirty(menu_layer_get_layer(s_menu));
    }
  }

  s_timer = app_timer_register(tick_delay(), on_timer, NULL);
}

// ---- main window -----------------------------------------------------------

static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);

  s_struct_sig = struct_sig();

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
  // On colour the row draws its own tint and rule, so the layer must not flood
  // the cell first; one bit has nothing but the inversion, so it keeps it.
  menu_layer_set_highlight_colors(s_menu, PBL_IF_COLOR_ELSE(col_bg(), col_accent()),
                                          PBL_IF_COLOR_ELSE(col_fg(), GColorBlack));
#ifdef PBL_ROUND
  menu_layer_set_center_focused(s_menu, true);
#endif
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  art_sync();

  s_running = true;
  s_timer = app_timer_register(tick_delay(), on_timer, NULL);
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
static uint16_t root_guide_row(void) { return s_has_save ? 2 : 1; }
static uint16_t root_rows(MenuLayer *m, uint16_t section, void *ctx) { return root_guide_row() + 1; }
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
  char title[24], sub[40];
  RowSpec rs = { title, sub, NULL, GColorClear, col_fg(), col_dim(), -1 };

  if (row_is_continue(idx->row)) {
    // Continue states what it resumes into, so the destructive row under it is
    // an informed choice rather than a guess.
    uint8_t n = 0;
    for (uint8_t i = 0; i < g.body_count; i++) if (g.bodies[i].scanned) n++;
    snprintf(title, sizeof(title), "Continue");
    snprintf(sub, sizeof(sub), "T+%luyr %d/%d FRM %d/%d", (unsigned long)g.cycle,
             n, g.body_count, g.frames, RING_FRAMES);
  } else if (idx->row == root_guide_row()) {
    snprintf(title, sizeof(title), "Guide");
    snprintf(sub, sizeof(sub), "what the readouts mean");
    rs.title_col = col_dim();
  } else {
    snprintf(title, sizeof(title), "New session");
    if (s_has_save) {
      snprintf(sub, sizeof(sub), "discards the run above");
      rs.sub_col = col_alert();
    } else {
      snprintf(sub, sizeof(sub), "an uncharted system");
    }
  }
  draw_row_cell(ctx, cell, &rs, ROOT_PAD);
}

static void root_select(MenuLayer *m, MenuIndex *idx, void *c) {
  if (idx->row == root_guide_row()) { ui_show_guide(); return; }
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

  // The frame stops at the bottom of the screen. It used to run past it on a
  // rectangle, to spend MenuLayer's ~16px of phantom content on a menu that was
  // never meant to scroll -- but that also told the layer it had a viewport
  // twice the size of the visible one, so once Guide made a third row the menu
  // would not scroll to reach it. The splash bitmaps stay cropped (51px on the
  // 144-wide screens, 98px on emery) so the two-row menu still sits flush.
  s_root_menu = menu_layer_create(GRect(0, top, b.size.w, b.size.h - top));
  menu_layer_set_callbacks(s_root_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = root_sections,
    .get_num_rows = root_rows,
    .get_header_height = root_header_h,
    .get_cell_height = root_cell_h,
    .draw_row = root_draw_row,
    .select_click = root_select,
  });
  menu_layer_set_normal_colors(s_root_menu, col_bg(), col_fg());
  menu_layer_set_highlight_colors(s_root_menu, PBL_IF_COLOR_ELSE(col_bg(), col_accent()),
                                               PBL_IF_COLOR_ELSE(col_fg(), GColorBlack));
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
