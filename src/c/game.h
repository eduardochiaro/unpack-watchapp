#pragma once
#include <pebble.h>

// Pacing. Fast-forward runs one in-game cycle per tick; idle runs one every
// IDLE_DIVISOR ticks, so standing still still costs wall-clock exposure.
#define TICK_MS      200
#define IDLE_DIVISOR 4

#define LAUNCH_WORKERS 5
#define MAX_BODIES 5
#define LOG_MAX 30
#define LOG_LEN 56

typedef enum { BT_ASTEROID, BT_MOON, BT_PLANET, BT_ANOMALY } BodyType;

typedef struct {
  char name[10];
  BodyType type;
  uint8_t yield;     // 0..2 tier
  uint8_t hazard;    // 0..2 tier
  uint8_t distance;  // 0..2 tier
  bool scanned;
  bool bio;
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
  ACT_BUILD_RIG,
  ACT_LAUNCH,
} ActionKind;

typedef enum { PHASE_IDLE, PHASE_ACTION, PHASE_EVENT, PHASE_OVER } Phase;
typedef enum { END_NONE, END_LAUNCH, END_COLLAPSE, END_SUDDEN } EndKind;

typedef struct {
  int16_t power, materials, workers;
  uint8_t arrays;

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
  bool rush;               // launch prep shortened

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

bool game_affordable(ActionKind k, uint8_t target);
int16_t game_cost_mat(ActionKind k);
int16_t game_cost_pow(ActionKind k);
uint16_t game_duration(ActionKind k, uint8_t target);
void game_start_action(ActionKind k, uint8_t target);
const char *game_action_name(ActionKind k);

const char *body_type_name(BodyType t);
const char *tier_name(uint8_t t);
