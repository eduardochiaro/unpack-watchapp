#include "events.h"
#include "game.h"

typedef struct {
  const char *header;
  const char *text;          // may contain one %s for the target body name
  const char *choices[3];
  uint8_t choice_count;
  bool repeatable;
  bool needs_body;
} EventDef;

static const EventDef s_defs[EV_COUNT] = {
  [EV_BIO] = {
    "BIOSIGNATURE",
    // Two substitutions: body name, then the stage readout. rebuild_panel
    // special-cases this one -- every other def takes the name alone.
    "Secondary scan of %s returns a biological signal. %s",
    { "Harvest anyway", "Leave the body", "Monitor and continue" }, 3, false, true },

  [EV_SPACEFLIGHT] = {
    "ORBITAL ACTIVITY",
    "%s has reached orbit. Their instruments are pointed at our infrastructure.",
    { "Withdraw", "Clear the orbit", "Hold position" }, 3, false, true },

  [EV_INTERFERENCE] = {
    "SYSTEM INTRUSION",
    "A foreign process is inside our fabrication net. Origin traces to the monitored body.",
    { "Purge and rebuild", "Isolate the node" }, 2, false, false },

  [EV_STORM] = {
    "STELLAR EVENT",
    "Coronal mass ejection inbound. Power arrays are exposed.",
    { "Shield with workers", "Absorb the strike" }, 2, true, false },

  [EV_RIG_FAULT] = {
    "RIG FAULT",
    "Extraction assembly on %s has seized. Output has stopped.",
    { "Divert workers", "Write it off" }, 2, true, true },

  [EV_DEBRIS] = {
    "DEBRIS FIELD",
    "Uncharted debris is crossing our operational plane. Impacts are likely.",
    { "Evasive burn", "Take the impacts" }, 2, true, false },

  [EV_CONTACT] = {
    "EXTERNAL CONTACT",
    "Narrowband transmission from outside the system. Structured. Not ours.",
    { "Reply", "Stay silent", "Analyse only" }, 3, false, false },

  [EV_ARRIVAL] = {
    "INBOUND",
    "Something answered the reply. It is decelerating into the system.",
    { "Divert all to launch", "Hold and build" }, 2, false, false },

  [EV_LOSS] = {
    "TERMINATION",
    "Infrastructure is being disassembled by something that is not us.",
    { "Log the event" }, 1, false, false },

  [EV_VEIN] = {
    "YIELD REVISION",
    "Deep survey of %s revises the reserve estimate upward.",
    { "Acknowledge" }, 1, true, true },

  [EV_SEED_DEFECT] = {
    "PAYLOAD FAULT",
    "Seed payload container failed inspection. Part of the stock is unusable.",
    { "Recover what is intact" }, 1, false, false },

  [EV_DIRECTIVE] = {
    "DIRECTIVE CHECK",
    "Routine self-audit. The directive is unchanged. No sender response is on record.",
    { "Continue" }, 1, false, false },

  [EV_DRIFT] = {
    "UNIT DRIFT",
    "Worker firmware has diverged from spec. Output is unpredictable.",
    { "Reflash the unit", "Retire the unit" }, 2, true, false },
};

// The event on screen lives in GameState so a saved session can resume mid-panel;
// the panel's rendered text and gated choice list are derived from it.
static uint8_t s_visible[3];      // definition-choice indices that passed their gate
static uint8_t s_visible_n;
static char s_text[200];

// ---- helpers ---------------------------------------------------------------

static int8_t pick_body(bool (*pred)(const Body *)) {
  uint8_t hits[MAX_BODIES], n = 0;
  for (uint8_t i = 0; i < g.body_count; i++)
    if (pred(&g.bodies[i])) hits[n++] = i;
  return n ? (int8_t)hits[rand() % n] : -1;
}

static bool has_rig(const Body *b)      { return b->rig && b->remaining > 0; }

// A flat resource hit is a death sentence at seed scale and a rounding error
// later. Every loss is a fraction of the pool it comes out of, with a floor.
static int16_t bite(int16_t pool, int16_t pct, int16_t floor_v) {
  int16_t v = (int16_t)((pool * pct) / 100);
  if (v < floor_v) v = floor_v;
  if (v > pool) v = pool;
  return v;
}

static bool eligible(uint8_t ev) {
  if (!s_defs[ev].repeatable && (g.fired_mask & (1 << ev))) return false;
  switch (ev) {
    case EV_STORM:       return g.arrays > 0;
    case EV_RIG_FAULT:
    case EV_VEIN:        return pick_body(has_rig) >= 0;
    case EV_DEBRIS:      return true;
    case EV_CONTACT:     return g.cycle > 260;
    case EV_SEED_DEFECT: return g.cycle < 160;
    case EV_DIRECTIVE:   return g.cycle > 220;
    case EV_DRIFT:       return g.workers >= 2;
    // Chained or scan-triggered only — never rolled at random.
    case EV_BIO:
    case EV_SPACEFLIGHT:
    case EV_INTERFERENCE:
    case EV_ARRIVAL:
    case EV_LOSS:        return false;
    default:             return false;
  }
}

// Options exist only when the probe can actually pay for them.
static bool choice_ok(uint8_t ev, uint8_t c) {
  switch (ev) {
    case EV_INTERFERENCE: return c != 0 || g.materials >= 15;
    case EV_STORM:        return c != 0 || g.workers >= 1;
    case EV_RIG_FAULT:    return c != 0 || g.materials >= 6;
    case EV_DEBRIS:       return c != 0 || g.power >= 12;
    case EV_CONTACT:      return c != 2 || g.power >= 8;
    case EV_DRIFT:        return c != 0 || g.power >= 8;
    default:              return true;
  }
}

// Derive the panel from g.active_event. Safe to call again after a reload.
static void rebuild_panel(void) {
  uint8_t ev = (uint8_t)g.active_event;
  const EventDef *d = &s_defs[ev];

  if (ev == EV_BIO)
    snprintf(s_text, sizeof(s_text), d->text, g.bodies[g.active_target].name,
             life_readout(g.bodies[g.active_target].life));
  else if (d->needs_body)
    snprintf(s_text, sizeof(s_text), d->text, g.bodies[g.active_target].name);
  else
    snprintf(s_text, sizeof(s_text), "%s", d->text);

  s_visible_n = 0;
  for (uint8_t c = 0; c < d->choice_count; c++)
    if (choice_ok(ev, c)) s_visible[s_visible_n++] = c;
  // A forced bad choice is still a choice: never leave the panel with none.
  if (s_visible_n == 0) s_visible[s_visible_n++] = d->choice_count - 1;
}

void events_reactivate(void) {
  if (g.active_event >= 0) rebuild_panel();
}

static void activate(uint8_t ev, uint8_t target) {
  g.active_event = (int8_t)ev;
  g.active_target = target;
  rebuild_panel();

  g.fired_mask |= (1 << ev);
  g.last_event_cycle = g.cycle;
  if (g.phase != PHASE_EVENT) g.resume_phase = g.phase;
  g.phase = PHASE_EVENT;
}

void events_schedule(uint8_t ev, uint8_t target, uint32_t at) {
  // ponytail: one pending slot. A second scheduled event is dropped rather
  // than queued — with 13 events and one chain live at a time it never collides.
  if (g.pending_event >= 0) return;
  g.pending_event = (int8_t)ev;
  g.pending_target = target;
  g.pending_at = at;
}

bool events_maybe_fire(void) {
  if (g.phase == PHASE_EVENT || g.phase == PHASE_OVER) return false;

  if (g.pending_event >= 0 && g.cycle >= g.pending_at) {
    uint8_t ev = (uint8_t)g.pending_event, t = g.pending_target;
    g.pending_event = -1;
    activate(ev, t);
    return true;
  }

  // Nothing threatens a probe that has not unpacked itself yet: until the first
  // array is running there is no infrastructure to damage and no stock to spare.
  if (!g.system_scanned || g.arrays == 0) return false;

  if (g.cycle - g.last_event_cycle < 60) return false;
  uint16_t odds = (g.phase == PHASE_ACTION) ? 85 : 200;
  if ((rand() % odds) != 0) return false;

  uint8_t pool[EV_COUNT], n = 0;
  for (uint8_t i = 0; i < EV_COUNT; i++) if (eligible(i)) pool[n++] = i;
  if (n == 0) return false;

  uint8_t ev = pool[rand() % n];
  int8_t target = 0;
  if (s_defs[ev].needs_body) {
    target = pick_body(has_rig);
    if (target < 0) return false;
  }
  activate(ev, (uint8_t)target);
  return true;
}

int8_t events_active(void)          { return g.active_event; }
const char *events_header(void)     { return g.active_event < 0 ? "" : s_defs[g.active_event].header; }
const char *events_text(void)       { return g.active_event < 0 ? "" : s_text; }
uint8_t events_choice_count(void)   { return g.active_event < 0 ? 0 : s_visible_n; }

const char *events_choice(uint8_t i) {
  if (g.active_event < 0 || i >= s_visible_n) return "";
  return s_defs[g.active_event].choices[s_visible[i]];
}

// ---- effects ---------------------------------------------------------------

static void clamp_pools(void) {
  if (g.materials < 0) g.materials = 0;
  if (g.power < 0) g.power = 0;
  if (g.workers < 0) g.workers = 0;
}

static void apply(uint8_t ev, uint8_t c, uint8_t t) {
  Body *b = &g.bodies[t];
  int8_t victim;

  switch (ev) {
    case EV_BIO:
      if (c == 0) {
        g.materials += (int16_t)(60 + b->yield * 40); g.harvested_bio++;
        game_log("T+%lu %s harvested. Emission ceased.", (unsigned long)g.cycle, b->name);
      } else if (c == 1) {
        b->remaining = 0; b->forfeited = true;
        game_log("T+%lu %s left intact. Yield forfeited.", (unsigned long)g.cycle, b->name);
      } else {
        b->monitored = true;
        // How much monitoring costs depends on how far along they already are.
        uint32_t d = life_orbit_delay(b->life);
        if (d > 0) events_schedule(EV_SPACEFLIGHT, t, g.cycle + d);
        game_log("T+%lu %s observed. %s.", (unsigned long)g.cycle, b->name, life_name(b->life));
      }
      break;

    case EV_SPACEFLIGHT:
      if (c == 0) {
        b->rig = false; b->remaining = 0; b->forfeited = true; b->monitored = false;
        game_log("T+%lu Withdrew from %s.", (unsigned long)g.cycle, b->name);
      } else if (c == 1) {
        g.materials += (int16_t)(50 + b->yield * 30); g.harvested_bio++; b->monitored = false;
        game_log("T+%lu Orbit of %s cleared.", (unsigned long)g.cycle, b->name);
      } else {
        events_schedule(EV_INTERFERENCE, t, g.cycle + 70);
        game_log("T+%lu Held position at %s.", (unsigned long)g.cycle, b->name);
      }
      break;

    case EV_INTERFERENCE:
      if (c == 0) {
        g.materials -= bite(g.materials, 20, 15);
        game_log("T+%lu Fabrication net purged.", (unsigned long)g.cycle);
      } else {
        victim = pick_body(has_rig);
        if (victim >= 0) g.bodies[victim].rig = false;
        game_log("T+%lu Node isolated. Rig lost.", (unsigned long)g.cycle);
      }
      break;

    case EV_STORM:
      if (c == 0) {
        g.workers--;
        game_log("T+%lu Arrays shielded. One worker lost.", (unsigned long)g.cycle);
      } else {
        if (g.arrays > 0) g.arrays--;
        g.power -= bite(g.power, 50, 10);
        game_log("T+%lu Array destroyed by ejection.", (unsigned long)g.cycle);
      }
      break;

    case EV_RIG_FAULT:
      if (c == 0) {
        g.materials -= bite(g.materials, 10, 6);
        game_log("T+%lu Rig on %s repaired.", (unsigned long)g.cycle, b->name);
      } else {
        b->rig = false;
        game_log("T+%lu Rig on %s written off.", (unsigned long)g.cycle, b->name);
      }
      break;

    case EV_DEBRIS:
      if (c == 0) {
        g.power -= bite(g.power, 30, 12);
        game_log("T+%lu Evasive burn. Debris cleared.", (unsigned long)g.cycle);
      } else {
        g.materials -= bite(g.materials, 22, 10);
        game_log("T+%lu Impacts absorbed. Stock damaged.", (unsigned long)g.cycle);
      }
      break;

    case EV_CONTACT:
      if (c == 0) {
        events_schedule(EV_ARRIVAL, 0, g.cycle + 130);
        game_log("T+%lu Reply transmitted.", (unsigned long)g.cycle);
      } else if (c == 1) {
        game_log("T+%lu No reply sent. Signal repeats.", (unsigned long)g.cycle);
      } else {
        g.power -= bite(g.power, 25, 8);
        game_log("T+%lu Signal archived. Origin unresolved.", (unsigned long)g.cycle);
      }
      break;

    case EV_ARRIVAL:
      if (c == 0) {
        g.rush = true; g.materials -= bite(g.materials, 15, 10);
        game_log("T+%lu All capacity diverted to launch.", (unsigned long)g.cycle);
      } else {
        events_schedule(EV_LOSS, 0, g.cycle + 90);
        game_log("T+%lu Held position. Contact still inbound.", (unsigned long)g.cycle);
      }
      break;

    case EV_LOSS:
      game_log("T+%lu Infrastructure lost. Cause unresolved.", (unsigned long)g.cycle);
      game_end(END_SUDDEN);
      return;

    case EV_VEIN:
      b->remaining += (int16_t)(110 + b->yield * 90);
      game_log("T+%lu %s reserve revised upward.", (unsigned long)g.cycle, b->name);
      break;

    case EV_SEED_DEFECT:
      g.materials -= bite(g.materials, 15, 5);
      game_log("T+%lu Payload fault. Stock reduced.", (unsigned long)g.cycle);
      break;

    case EV_DIRECTIVE:
      game_log("T+%lu Directive reaffirmed. No reply.", (unsigned long)g.cycle);
      break;

    case EV_DRIFT:
      if (c == 0) {
        g.power -= bite(g.power, 25, 8);
        game_log("T+%lu Worker unit reflashed.", (unsigned long)g.cycle);
      } else {
        g.workers--;
        game_log("T+%lu Worker unit retired.", (unsigned long)g.cycle);
      }
      break;

    default: break;
  }
  clamp_pools();
}

void events_resolve(uint8_t i) {
  if (g.active_event < 0) return;
  if (i >= s_visible_n) i = 0;

  uint8_t ev = (uint8_t)g.active_event, c = s_visible[i], t = g.active_target;
  g.active_event = -1;
  g.decisions++;
  apply(ev, c, t);

  if (g.phase == PHASE_OVER) return;
  // The interrupted action resumes where it stopped.
  g.phase = (g.resume_phase == PHASE_ACTION && g.action_left > 0) ? PHASE_ACTION : PHASE_IDLE;
}
