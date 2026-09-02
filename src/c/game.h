#pragma once
#include <pebble.h>

// Pacing. Fast-forward runs one in-game cycle per tick; idle runs one every
// IDLE_DIVISOR ticks, so standing still still costs wall-clock exposure.
#define TICK_MS      200
#define IDLE_DIVISOR 4

// The construction chain. Factories speed every later build, frames are what the
// ring anchors to, and the ring is the last thing built before the probe leaves.
#define FRAME_WORKERS  2   // crew committed to each frame; they stay with it
#define RING_WORKERS   3   // crew committed to the ring
#define RING_FRAMES    3
#define RING_FACTORIES 2
#define MAX_BODIES 5
#define LOG_MAX 30
#define LOG_LEN 56

typedef enum { BT_ASTEROID, BT_MOON, BT_PLANET, BT_ANOMALY } BodyType;

// How far a biosphere has got. Only meaningful on a body with `bio` set. The
// stage sets how long the inhabitants take to reach orbit and notice us --
// LIFE_SIMPLE never gets there inside a mission.
typedef enum { LIFE_SIMPLE, LIFE_PRIMITIVE, LIFE_PREINDUSTRIAL, LIFE_ADVANCED } LifeStage;

typedef struct {
  char name[10];
  BodyType type;
  uint8_t yield;     // 0..2 tier
  uint8_t hazard;    // 0..2 tier
  uint8_t distance;  // 0..2 tier
  bool scanned;
  bool bio;
  uint8_t life;      // LifeStage, only read when bio
  bool monitored;
  bool rig;
  bool forfeited;    // deliberately left alone
  int16_t remaining; // materials still in the body
} Body;

typedef enum {
  ACT_LOG = 0,        // not an action: opens the mission log
  ACT_SCAN_SYSTEM,
  ACT_SCAN_BODY,
  ACT_BUILD_POWER,
  ACT_BUILD_WORKER,
  ACT_BUILD_FACTORY,
  ACT_BUILD_RIG,
  ACT_FRAME,
  ACT_RING,
  ACT_GUIDE,          // not an action either: opens the terminology guide
} ActionKind;

typedef enum { PHASE_IDLE, PHASE_ACTION, PHASE_EVENT, PHASE_OVER } Phase;
typedef enum { END_NONE, END_DEPART, END_COLLAPSE, END_SUDDEN } EndKind;

typedef struct {
  int16_t power, materials, workers;
  uint8_t arrays, factories, frames;
  uint32_t extracted;      // running total pulled out of the system, for the ledger

  Body bodies[MAX_BODIES];
  uint8_t body_count;
  bool system_scanned;

  uint32_t cycle;          // in-game years elapsed
  Phase phase;
  Phase resume_phase;      // phase to restore after an event

  ActionKind action;
  uint8_t action_target;
  uint16_t action_left, action_total;

  EndKind end;
  bool rush;               // frame and ring assembly shortened

  int8_t active_event;     // event on screen right now, -1 = none
  uint8_t active_target;
  uint16_t fired_mask;     // one-shot events already used
  uint32_t last_event_cycle;
  int8_t pending_event;    // -1 = none
  uint32_t pending_at;
  uint8_t pending_target;

  uint8_t harvested_bio;   // biosignature bodies taken
  uint8_t decisions;

  char log[LOG_MAX][LOG_LEN];
  uint8_t log_count;
  bool log_full;
} GameState;

extern GameState g;

void game_new(void);
void game_new_seeded(unsigned seed);

// ponytail: macro + staging buffer, not a varargs function -- the Pebble SDK has
// no vsnprintf, and formatting straight into g.log would alias g's own fields.
// Fixed-size log; once full, later entries drop (a run cannot realistically fill it).
extern char g_logbuf[LOG_LEN];
void game_log_line(const char *s);
#define game_log(...) do { \
    snprintf(g_logbuf, LOG_LEN, __VA_ARGS__); \
    game_log_line(g_logbuf); \
  } while (0)

void game_tick(void);
void game_end(EndKind kind);

bool game_affordable(ActionKind k);
int16_t game_cost_mat(ActionKind k);
int16_t game_cost_pow(ActionKind k);
uint16_t game_duration(ActionKind k, uint8_t target);
void game_start_action(ActionKind k, uint8_t target);
const char *game_action_name(ActionKind k);

const char *body_type_name(BodyType t);
const char *tier_name(uint8_t t);
const char *life_name(uint8_t stage);        // "microbial", "industrial", ...
const char *life_readout(uint8_t stage);     // the scan line shown in the event panel
uint32_t life_orbit_delay(uint8_t stage);    // cycles until orbit, 0 = never
