#include "ui.h"
#include "game.h"
#include "events.h"

// The HUD band and its action progress bar. Emery and gabbro have the pixels for
// a chunkier bar, and the band grows to keep it clear of the status line.
#if PBL_DISPLAY_WIDTH >= 200
#define HUD_H         46
#define BAR_H          8
#else
#define HUD_H         38
#define BAR_H          3
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
static Layer *s_hud_layer, *s_event_layer, *s_masthead;
static GBitmap *s_splash;
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

static GFont s_f14, s_f14b, s_title_font;

typedef struct { ActionKind act; } OpRow;
static OpRow s_ops[8];
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
  s_ops[s_ops_n++].act = ACT_LAUNCH;
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

// ---- HUD -------------------------------------------------------------------

static void hud_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  char buf[40];

  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  const int pad = PBL_IF_ROUND_ELSE(20, 3);
  const GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  snprintf(buf, sizeof(buf), "P%d  M%d  W%d", g.power, g.materials, g.workers);
  graphics_context_set_text_color(ctx, col_level(g.materials, 20, 8));
  graphics_draw_text(ctx, buf, s_f14b, GRect(pad, -3, b.size.w - 2 * pad, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);

  if (g.phase == PHASE_ACTION) {
    snprintf(buf, sizeof(buf), "%s  T+%luyr", game_action_name(g.action), (unsigned long)g.cycle);
  } else {
    snprintf(buf, sizeof(buf), "IDLE  T+%luyr", (unsigned long)g.cycle);
  }
  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, buf, s_f14, GRect(pad, 12, b.size.w - 2 * pad, 18),
                     GTextOverflowModeTrailingEllipsis, al, NULL);

  // Progress bar for the running action; a thin rule when idle.
  int16_t bx = pad, bw = b.size.w - 2 * pad, by = b.size.h - BAR_H - 5;
  graphics_context_set_fill_color(ctx, col_fg());
  if (g.phase == PHASE_ACTION && g.action_total > 0) {
    uint16_t done = g.action_total - g.action_left;
    graphics_context_set_stroke_color(ctx, col_fg());
    graphics_draw_rect(ctx, GRect(bx, by, bw, BAR_H));
    graphics_context_set_fill_color(ctx, col_accent());
    graphics_fill_rect(ctx, GRect(bx, by, (int16_t)((bw * done) / g.action_total), BAR_H),
                       0, GCornerNone);
  } else {
    graphics_fill_rect(ctx, GRect(bx, by + BAR_H / 2, bw, 1), 0, GCornerNone);
  }
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
      snprintf(sub, sn, "free - %d cyc", game_duration(k, 0));
      break;
    case ACT_BUILD_POWER:
      snprintf(title, tn, "Power array (%d)", g.arrays);
      snprintf(sub, sn, "%dM - %d cyc", game_cost_mat(k), game_duration(k, 0));
      break;
    case ACT_BUILD_WORKER:
      snprintf(title, tn, "Worker unit (%d)", g.workers);
      snprintf(sub, sn, "%dM %dP - %d cyc", game_cost_mat(k), game_cost_pow(k), game_duration(k, 0));
      break;
    case ACT_LAUNCH:
      snprintf(title, tn, "Probe + launch");
      snprintf(sub, sn, "%dM %dP %dW - %d cyc", game_cost_mat(k), game_cost_pow(k), LAUNCH_WORKERS, game_duration(k, 0));
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
  // The two reading screens cost nothing, so they never carry the "short" tag.
  if (k != ACT_LOG && k != ACT_GUIDE && !game_affordable(k, 0)) {
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
  layer_mark_dirty(s_hud_layer);
  menu_layer_reload_data(s_menu);
}

// ---- event panel -----------------------------------------------------------

static void event_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int pad = PBL_IF_ROUND_ELSE(18, 4);
  const uint8_t n = events_choice_count();
  const int16_t choice_h = 17;

  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar
  graphics_context_set_fill_color(ctx, col_accent());
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, 18), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, events_header(), s_f14b, GRect(pad, -3, b.size.w - 2 * pad, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Choices are bottom-anchored so they are never pushed off screen by long text.
  int16_t choices_y = b.size.h - n * choice_h - 2;
  GRect tr = GRect(pad, 20, b.size.w - 2 * pad, choices_y - 22);
  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, events_text(), s_f14, tr, GTextOverflowModeWordWrap,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);

  for (uint8_t i = 0; i < n; i++) {
    GRect row = GRect(0, choices_y + i * choice_h, b.size.w, choice_h);
    bool sel = (i == s_event_sel);
    if (sel) {
      graphics_context_set_fill_color(ctx, col_fg());
      graphics_fill_rect(ctx, row, 0, GCornerNone);
    }
    graphics_context_set_text_color(ctx, sel ? GColorBlack : col_fg());
    graphics_draw_text(ctx, events_choice(i), s_f14,
                       GRect(pad, row.origin.y - 3, b.size.w - 2 * pad, choice_h + 4),
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
  layer_mark_dirty(s_hud_layer);
  if (g.phase == PHASE_OVER) ui_show_ledger();
}

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
  for (uint8_t i = 0; i < g.log_count && at < sizeof(s_ledger) - 2; i++) {
    int n = snprintf(s_ledger + at, sizeof(s_ledger) - at, "%s\n", g.log[i]);
    if (n < 0) break;
    at += (size_t)n;
  }
  if (g.log_full && at < sizeof(s_ledger) - 20) {
    snprintf(s_ledger + at, sizeof(s_ledger) - at, "(log truncated)\n");
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
  if (!s_running) { s_timer = NULL; return; }
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

  if (advanced) {
    layer_mark_dirty(s_hud_layer);
    uint32_t sig = struct_sig();
    if (sig != s_struct_sig) {
      s_struct_sig = sig;
      build_ops();
      menu_layer_reload_data(s_menu);
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

  s_hud_layer = layer_create(GRect(0, 0, b.size.w, HUD_H));
  layer_set_update_proc(s_hud_layer, hud_update);
  layer_add_child(root, s_hud_layer);

  s_menu = menu_layer_create(GRect(0, HUD_H, b.size.w, b.size.h - HUD_H));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_sections,
    .get_num_rows = menu_rows,
    .get_header_height = menu_header_h,
    .get_cell_height = menu_cell_h,
    .draw_header = menu_draw_header,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
  });
  menu_layer_set_normal_colors(s_menu, col_bg(), col_fg());
  menu_layer_set_highlight_colors(s_menu, col_accent(), GColorBlack);
#ifdef PBL_ROUND
  menu_layer_set_center_focused(s_menu, true);
#endif
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_running = true;
  s_timer = app_timer_register(TICK_MS, on_timer, NULL);
}

static void main_unload(Window *w) {
  s_running = false;
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  menu_layer_destroy(s_menu);
  layer_destroy(s_hud_layer);
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
  s_root_menu = menu_layer_create(GRect(0, top, b.size.w, b.size.h));
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

  s_has_save = session_load();

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
