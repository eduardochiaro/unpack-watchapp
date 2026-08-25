#include "game.h"
#include "events.h"

GameState g;
char g_logbuf[LOG_LEN];

void game_log_line(const char *s) {
  if (g.log_count >= LOG_MAX) { g.log_full = true; return; }
  strncpy(g.log[g.log_count], s, LOG_LEN - 1);
  g.log[g.log_count][LOG_LEN - 1] = '\0';
  g.log_count++;
}

static const char *s_tier[3] = { "low", "med", "high" };
static const char *s_type[4] = { "asteroid", "moon", "planet", "anomaly" };

static const char *s_life[4]    = { "microbial", "primitive", "pre-industrial", "industrial" };
static const char *s_readout[4] = {
  "Microbial. Nothing there can notice us.",
  "Pre-agricultural. Stone and fire.",
  "Pre-industrial. They are not aware of us.",
  "Industrial. Their telescopes are improving.",
};

// Cycles from first contact with the signal until the inhabitants make orbit.
// The further along a biosphere already is, the less time monitoring buys.
// LIFE_SIMPLE returns 0: microbes do not reach orbit inside a mission, so the
// event is never scheduled at all rather than parked in the pending slot.
static const uint16_t s_orbit_base[4] = { 0, 400, 180, 90 };
static const uint16_t s_orbit_span[4] = { 0, 260, 160, 70 };

const char *tier_name(uint8_t t) { return s_tier[t > 2 ? 2 : t]; }
const char *body_type_name(BodyType t) { return s_type[t > 3 ? 3 : t]; }
const char *life_name(uint8_t stage) { return s_life[stage > 3 ? 3 : stage]; }
const char *life_readout(uint8_t stage) { return s_readout[stage > 3 ? 3 : stage]; }

uint32_t life_orbit_delay(uint8_t stage) {
  if (stage > 3) stage = 3;
  if (s_orbit_base[stage] == 0) return 0;
  return s_orbit_base[stage] + (uint32_t)(rand() % s_orbit_span[stage]);
}

// Cumulative life-stage odds in percent, indexed by BodyType then LifeStage.
// Planet: 30 microbial / 25 primitive / 10 pre-industrial / 5 advanced, 30 dead.
// Moon:   20 / 14 / 10 / 1, 55 dead.
static const uint8_t s_life_odds[4][4] = {
  [BT_ASTEROID] = {  0,  0,  0,  0 },
  [BT_MOON]     = { 20, 34, 44, 45 },
  [BT_PLANET]   = { 30, 55, 65, 70 },
  [BT_ANOMALY]  = {  0,  0,  0,  0 },
};

static void gen_body(Body *b, uint8_t idx) {
  int roll = rand() % 100;
  if (roll < 55)      b->type = BT_ASTEROID;
  else if (roll < 78) b->type = BT_MOON;
  else if (roll < 93) b->type = BT_PLANET;
  else                b->type = BT_ANOMALY;

  b->yield    = rand() % 3;
  b->hazard   = rand() % 3;
  b->distance = rand() % 3;

  switch (b->type) {
    case BT_ASTEROID: snprintf(b->name, sizeof(b->name), "AST-%02d", 10 + rand() % 89); break;
    case BT_MOON:     snprintf(b->name, sizeof(b->name), "M-%03d",   100 + rand() % 899); break;
    case BT_PLANET:   snprintf(b->name, sizeof(b->name), "P-%03d",   100 + rand() % 899); break;
    default:          snprintf(b->name, sizeof(b->name), "ANOM-%d",  1 + idx); break;
  }

  b->remaining = 90 + b->yield * 120 + (rand() % 70);

  // One roll settles both whether the body carries life and how far along it is.
  // A roll that clears the last threshold is a dead body, so the table also
  // enforces that only planets and moons can hold a biosphere at all.
  const uint8_t *cum = s_life_odds[b->type];
  int r = rand() % 100;
  b->bio = false;
  for (uint8_t st = 0; st < 4; st++) {
    if (r < cum[st]) { b->bio = true; b->life = st; break; }
  }
  if (b->type == BT_ANOMALY) b->hazard = 2;
}

void game_new(void) { game_new_seeded((unsigned)time(NULL)); }

void game_new_seeded(unsigned seed) {
  memset(&g, 0, sizeof(g));
  srand(seed);

  g.materials = 70;   // seed payload: one array, one rig, and a little slack
  g.power = 0;
  g.workers = 0;
  g.phase = PHASE_IDLE;
  g.resume_phase = PHASE_IDLE;
  g.pending_event = -1;
  g.active_event = -1;
  g.end = END_NONE;

  g.body_count = 3 + rand() % 3;   // 3..5
  for (uint8_t i = 0; i < g.body_count; i++) gen_body(&g.bodies[i], i);

  game_log("T+0 ARRIVAL. Seed payload intact.");
  game_log("No infrastructure. No power.");
}

int16_t game_cost_mat(ActionKind k) {
  switch (k) {
    case ACT_BUILD_POWER:  return 15;
    case ACT_BUILD_WORKER: return 12;
    case ACT_BUILD_RIG:    return 25;
    case ACT_LAUNCH:       return 400;
    default:               return 0;
  }
}

int16_t game_cost_pow(ActionKind k) {
  switch (k) {
    case ACT_SCAN_BODY:    return 2;
    case ACT_BUILD_WORKER: return 6;
    case ACT_BUILD_RIG:    return 10;
    case ACT_LAUNCH:       return 140;
    default:               return 0;
  }
}

uint16_t game_duration(ActionKind k, uint8_t target) {
  uint16_t base;
  switch (k) {
    case ACT_SCAN_SYSTEM:  base = 36; break;
    case ACT_SCAN_BODY:    base = 24 + g.bodies[target].distance * 18; break;
    case ACT_BUILD_POWER:  base = 82; break;
    case ACT_BUILD_WORKER: base = 98; break;
    case ACT_BUILD_RIG:    base = 128 + g.bodies[target].distance * 38; break;
    case ACT_LAUNCH:       base = g.rush ? 135 : 255; break;
    default:               return 1;
  }
  // Workers are the compounding multiplier: they only ever shorten the window.
  uint16_t d = (uint16_t)((base * 4) / (4 + (g.workers > 12 ? 12 : g.workers)));
  return d < 3 ? 3 : d;
}

bool game_affordable(ActionKind k, uint8_t target) {
  (void)target;
  if (g.materials < game_cost_mat(k)) return false;
  if (g.power < game_cost_pow(k)) return false;
  if (k == ACT_LAUNCH && g.workers < LAUNCH_WORKERS) return false;
  return true;
}

const char *game_action_name(ActionKind k) {
  switch (k) {
    case ACT_SCAN_SYSTEM:  return "SURVEY";
    case ACT_SCAN_BODY:    return "SCAN";
    case ACT_BUILD_POWER:  return "ARRAY";
    case ACT_BUILD_WORKER: return "WORKER";
    case ACT_BUILD_RIG:    return "RIG";
    case ACT_LAUNCH:       return "LAUNCH";
    default:               return "IDLE";
  }
}

void game_start_action(ActionKind k, uint8_t target) {
  if (g.phase != PHASE_IDLE || !game_affordable(k, target)) return;
  g.materials -= game_cost_mat(k);
  g.power     -= game_cost_pow(k);
  if (k == ACT_LAUNCH) g.workers -= LAUNCH_WORKERS;   // the crew goes with the probes

  g.action = k;
  g.action_target = target;
  g.action_total = game_duration(k, target);
  g.action_left = g.action_total;
  g.phase = PHASE_ACTION;
  g.resume_phase = PHASE_ACTION;
}

static void complete_action(void) {
  ActionKind k = g.action;
  uint8_t t = g.action_target;
  Body *b = &g.bodies[t];

  switch (k) {
    case ACT_SCAN_SYSTEM:
      g.system_scanned = true;
      game_log("T+%lu SURVEY: %d bodies resolved.", (unsigned long)g.cycle, g.body_count);
      break;

    case ACT_SCAN_BODY:
      b->scanned = true;
      game_log("T+%lu %s: yield %s, hazard %s.", (unsigned long)g.cycle,
               b->name, tier_name(b->yield), tier_name(b->hazard));
      if (b->bio) events_schedule(EV_BIO, t, g.cycle);   // fires on the next tick
      break;

    case ACT_BUILD_POWER:
      g.arrays++;
      game_log("T+%lu Power array %d online.", (unsigned long)g.cycle, g.arrays);
      break;

    case ACT_BUILD_WORKER:
      g.workers++;
      game_log("T+%lu Worker units: %d.", (unsigned long)g.cycle, g.workers);
      break;

    case ACT_BUILD_RIG:
      b->rig = true;
      game_log("T+%lu Rig anchored on %s.", (unsigned long)g.cycle, b->name);
      break;

    case ACT_LAUNCH:
      game_log("T+%lu Probes clear the system.", (unsigned long)g.cycle);
      game_end(END_LAUNCH);
      return;

    default: break;
  }

  g.action = ACT_LOG;
  g.phase = PHASE_IDLE;
  g.resume_phase = PHASE_IDLE;
}

static bool extracting(void) {
  // A rig with no power draws nothing. Without an array to refill the pool that
  // is not a pause, it is the end of material income.
  if (g.arrays == 0 && g.power < 1) return false;
  for (uint8_t i = 0; i < g.body_count; i++)
    if (g.bodies[i].rig && g.bodies[i].remaining > 0) return true;
  return false;
}

static bool riggable(void) {
  for (uint8_t i = 0; i < g.body_count; i++) {
    const Body *b = &g.bodies[i];
    if (!b->rig && !b->forfeited && b->remaining > 0) return true;
  }
  return false;
}

// Arrays buffer power, they do not bank it forever. Without a ceiling an idle
// probe could sit and accumulate its way to a launch, and the counter overflows.
static int16_t power_cap(void) { return (int16_t)(20 + g.arrays * 50); }

static void check_collapse(void) {
  // Materials only ever come from a running rig, so with none running the
  // question is whether the probe can still pay its way back to one -- or out.
  if (extracting()) return;

  int16_t need_workers = LAUNCH_WORKERS - g.workers;
  if (need_workers < 0) need_workers = 0;
  // Power is only obtainable through an array, so a probe with none has to
  // fund one out of the same stock before anything else it wants to do.
  int16_t array_tax = (g.arrays > 0) ? 0 : game_cost_mat(ACT_BUILD_POWER);

  int16_t mat_to_launch = (int16_t)(game_cost_mat(ACT_LAUNCH) +
                                    need_workers * game_cost_mat(ACT_BUILD_WORKER) + array_tax);
  bool can_launch = g.materials >= mat_to_launch;
  bool can_rig = riggable() && g.materials >= (int16_t)(game_cost_mat(ACT_BUILD_RIG) + array_tax);

  if (!can_launch && !can_rig) {
    game_log("T+%lu Reserves gone. Nothing left to build with.", (unsigned long)g.cycle);
    game_end(END_COLLAPSE);
  }
}

void game_tick(void) {
  if (g.phase == PHASE_OVER || g.phase == PHASE_EVENT) return;

  g.cycle++;
  g.power += g.arrays * 2;
  if (g.power > power_cap()) g.power = power_cap();

  // Rigs draw power to run; with no power they stall rather than fail.
  for (uint8_t i = 0; i < g.body_count; i++) {
    Body *b = &g.bodies[i];
    if (!b->rig || b->remaining <= 0) continue;
    if (g.power < 1) continue;
    g.power -= 1;
    int16_t rate = (int16_t)(b->yield + 1);
    if (rate > b->remaining) rate = b->remaining;
    b->remaining -= rate;
    g.materials += rate;
    if (g.materials > 9000) g.materials = 9000;
    if (b->remaining == 0) {
      b->rig = false;
      game_log("T+%lu %s depleted. Rig recovered.", (unsigned long)g.cycle, b->name);
      g.materials += 6;
    }
  }

  if (g.phase == PHASE_ACTION && g.action_left > 0) {
    g.action_left--;
    if (g.action_left == 0) complete_action();
  }

  if (g.phase == PHASE_OVER) return;

  events_maybe_fire();
  if (g.phase == PHASE_IDLE) check_collapse();
}

void game_end(EndKind kind) {
  if (g.end != END_NONE) return;
  g.end = kind;
  g.phase = PHASE_OVER;

  game_log("---");
  switch (kind) {
    case END_LAUNCH:   game_log("MISSION: probes launched."); break;
    case END_COLLAPSE: game_log("MISSION: collapse. No launch."); break;
    default:           game_log("MISSION: terminated."); break;
  }
  game_log("Elapsed: %lu yr", (unsigned long)g.cycle);
  game_log("Arrays %d  Workers %d  Decisions %d", g.arrays, g.workers, g.decisions);
  game_log("Materials left: %d", g.materials);

  for (uint8_t i = 0; i < g.body_count; i++) {
    Body *b = &g.bodies[i];
    if (!b->scanned) { game_log("%s: never surveyed.", b->name); continue; }
    if (b->forfeited) { game_log("%s: left intact.", b->name); continue; }
    if (b->monitored) { game_log("%s: still under watch.", b->name); continue; }
    game_log("%s: %d remaining.", b->name, b->remaining);
  }
  if (g.harvested_bio > 0) game_log("Biosignatures taken: %d", g.harvested_bio);
  game_log("---");
  game_log("Record ends. No acknowledgement sent.");
}
