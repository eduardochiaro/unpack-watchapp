#pragma once
#include <pebble.h>

enum {
  EV_BIO = 0,
  EV_SPACEFLIGHT,
  EV_INTERFERENCE,
  EV_STORM,
  EV_RIG_FAULT,
  EV_DEBRIS,
  EV_CONTACT,
  EV_ARRIVAL,
  EV_LOSS,
  EV_VEIN,
  EV_SEED_DEFECT,
  EV_DIRECTIVE,
  EV_DRIFT,
  EV_COUNT
};

void events_schedule(uint8_t ev, uint8_t target, uint32_t at);
bool events_maybe_fire(void);          // sets g.phase = PHASE_EVENT when one fires
int8_t events_active(void);
void events_reactivate(void);   // rebuild the panel after loading a saved session
const char *events_header(void);
const char *events_text(void);
uint8_t events_choice_count(void);
const char *events_choice(uint8_t i);
const char *events_choice_cost(uint8_t i);  // the consequence line under choice i
void events_resolve(uint8_t i);        // applies effects and restores the prior phase
