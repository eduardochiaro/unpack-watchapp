/* Headless check for the Unpack simulation. Build and run:
 *   cc -I test -o /tmp/sim_test test/sim_test.c src/c/game.c src/c/events.c && /tmp/sim_test
 * Fails loudly if the sim can produce negative pools, overrun the log,
 * deadlock, become unwinnable, or leave an event with no choices.
 */
#include "../src/c/game.h"
#include "../src/c/events.h"
#include <assert.h>

static void invariants(void) {
  if (g.materials < 0 || g.power < 0 || g.workers < 0) {
    printf("BAD phase=%d cyc=%lu P=%d M=%d W=%d arrays=%d act=%d\n", g.phase, (unsigned long)g.cycle, g.power, g.materials, g.workers, g.arrays, g.action);
    for (uint8_t i=0;i<g.log_count;i++) printf("  | %s\n", g.log[i]);
  }
  assert(g.materials >= 0);
  assert(g.power >= 0);
  assert(g.workers >= 0);
  assert(g.arrays <= 200);
  assert(g.log_count <= LOG_MAX);
  assert(g.body_count >= 3 && g.body_count <= MAX_BODIES);
  for (uint8_t i = 0; i < g.body_count; i++) assert(g.bodies[i].remaining >= 0);
  if (g.phase == PHASE_EVENT) assert(events_choice_count() >= 1);
}

/* A plausible player: survey, get power up, first rig, then workers, then scale.
   Waiting is a legitimate move -- power accrues on its own. */
static void policy(void) {
  if (g.phase != PHASE_IDLE) return;

  if (!g.system_scanned) { game_start_action(ACT_SCAN_SYSTEM, 0); return; }

  /* Nothing runs without power -- rebuild the array before anything else. */
  if (g.arrays == 0 && game_affordable(ACT_BUILD_POWER, 0)) { game_start_action(ACT_BUILD_POWER, 0); return; }

  bool have_rig = false;
  for (uint8_t i = 0; i < g.body_count; i++) if (g.bodies[i].rig) have_rig = true;

  for (uint8_t i = 0; i < g.body_count; i++) {
    if (g.bodies[i].scanned) continue;
    if (game_affordable(ACT_SCAN_BODY, i)) { game_start_action(ACT_SCAN_BODY, i); return; }
    if (g.arrays == 0 && game_affordable(ACT_BUILD_POWER, 0)) { game_start_action(ACT_BUILD_POWER, 0); return; }
    return;                       /* wait for power rather than burn stock */
  }

  for (uint8_t i = 0; i < g.body_count; i++) {
    Body *b = &g.bodies[i];
    if (!b->scanned || b->rig || b->remaining <= 0 || b->forfeited) continue;
    if (game_affordable(ACT_BUILD_RIG, i)) { game_start_action(ACT_BUILD_RIG, i); return; }
    if (!have_rig) return;        /* first rig is worth waiting for */
    break;
  }

  if (g.workers < LAUNCH_WORKERS + 1 && game_affordable(ACT_BUILD_WORKER, 0)) { game_start_action(ACT_BUILD_WORKER, 0); return; }
  if (g.arrays < 3 && game_affordable(ACT_BUILD_POWER, 0))   { game_start_action(ACT_BUILD_POWER, 0); return; }
  if (game_affordable(ACT_LAUNCH, 0)) game_start_action(ACT_LAUNCH, 0);
}

uint32_t g_action_cycles, g_idle_cycles;

static EndKind run(unsigned seed, uint8_t choice_bias) {
  game_new_seeded(seed);
  g_action_cycles = g_idle_cycles = 0;
  for (uint32_t t = 0; t < 60000 && g.phase != PHASE_OVER; t++) {
    if (g.phase == PHASE_EVENT) {
      uint8_t n = events_choice_count();
      assert(n >= 1 && n <= 3);
      events_resolve(choice_bias % n);
      invariants();
      continue;
    }
    policy();
    if (g.phase == PHASE_ACTION) g_action_cycles++; else g_idle_cycles++;
    game_tick();
    invariants();
  }
  if (g.phase != PHASE_OVER) {
    printf("STALL seed=%u cyc=%lu P=%d M=%d W=%d A=%d phase=%d\n",
           seed, (unsigned long)g.cycle, g.power, g.materials, g.workers, g.arrays, g.phase);
    for (uint8_t i = 0; i < g.body_count; i++)
      printf("  body %s scan=%d rig=%d rem=%d forf=%d\n", g.bodies[i].name,
             g.bodies[i].scanned, g.bodies[i].rig, g.bodies[i].remaining, g.bodies[i].forfeited);
  }
  assert(g.phase == PHASE_OVER);   /* no run may stall forever */
  return g.end;
}

/* Force every event through every one of its choices. */
static void event_matrix(void) {
  for (uint8_t ev = 0; ev < EV_COUNT; ev++) {
    for (uint8_t c = 0; c < 3; c++) {
      game_new_seeded(1000 + ev * 7 + c);
      g.system_scanned = true;
      g.materials = 100; g.power = 100; g.workers = 3; g.arrays = 2;
      for (uint8_t i = 0; i < g.body_count; i++) {
        g.bodies[i].scanned = true;
        g.bodies[i].rig = true;
        if (g.bodies[i].remaining < 50) g.bodies[i].remaining = 50;
      }
      g.pending_event = -1;
      events_schedule(ev, 0, g.cycle);
      assert(events_maybe_fire());
      assert(g.phase == PHASE_EVENT);
      uint8_t n = events_choice_count();
      assert(n >= 1 && n <= 3);
      assert(events_header()[0] != '\0');
      assert(events_text()[0] != '\0');
      for (uint8_t i = 0; i < n; i++) assert(events_choice(i)[0] != '\0');
      events_resolve(c % n);
      invariants();
      assert(g.phase != PHASE_EVENT);
    }
  }
}

/* A session saved mid-event must come back to the same panel. The panel text and
   choice list are derived, not stored, so this round-trips the state exactly the
   way the save does -- and scribbles over the derived statics first, so a panel
   that only "works" because it was left in memory fails here. */
static void resume_mid_event(void) {
  static uint8_t blob[sizeof(GameState)];
  char hdr[64], txt[220], ch[3][48];

  for (uint8_t ev = 0; ev < EV_COUNT; ev++) {
    game_new_seeded(4000 + ev);
    g.system_scanned = true;
    g.materials = 100; g.power = 100; g.workers = 3; g.arrays = 2;
    for (uint8_t i = 0; i < g.body_count; i++) { g.bodies[i].scanned = true; g.bodies[i].rig = true; }
    g.pending_event = -1;
    events_schedule(ev, 0, g.cycle);
    assert(events_maybe_fire());

    uint8_t n = events_choice_count();
    snprintf(hdr, sizeof(hdr), "%s", events_header());
    snprintf(txt, sizeof(txt), "%s", events_text());
    for (uint8_t i = 0; i < n; i++) snprintf(ch[i], sizeof(ch[i]), "%s", events_choice(i));

    memcpy(blob, &g, sizeof(GameState));

    /* Stale the derived panel by genuinely firing a different event, so the
       comparison below cannot pass on leftovers. Never clobber using the
       function under test. */
    uint8_t other = (uint8_t)((ev + 1) % EV_COUNT);
    game_new_seeded(7000 + ev);
    g.system_scanned = true;
    g.materials = 100; g.power = 100; g.workers = 3; g.arrays = 2;
    for (uint8_t i = 0; i < g.body_count; i++) { g.bodies[i].scanned = true; g.bodies[i].rig = true; }
    g.pending_event = -1;
    events_schedule(other, 0, g.cycle);
    assert(events_maybe_fire());

    memset(&g, 0, sizeof(GameState));
    memcpy(&g, blob, sizeof(GameState));
    events_reactivate();

    assert(g.phase == PHASE_EVENT);
    assert(strcmp(hdr, events_header()) == 0);
    assert(strcmp(txt, events_text()) == 0);
    assert(events_choice_count() == n);
    for (uint8_t i = 0; i < n; i++) assert(strcmp(ch[i], events_choice(i)) == 0);

    events_resolve(0);
    invariants();
    assert(g.phase != PHASE_EVENT);
  }
}

int main(void) {
  int launched = 0, collapsed = 0, sudden = 0;

  for (unsigned seed = 1; seed <= 300; seed++) {
    switch (run(seed, (uint8_t)(seed % 3))) {
      case END_LAUNCH:   launched++;  break;
      case END_COLLAPSE: collapsed++; break;
      default:           sudden++;    break;
    }
  }

  {
    uint32_t tot = 0, n = 0, mn = 999999, mx = 0, dec = 0, ev_min = 99, ev_max = 0;
    for (unsigned s = 1; s <= 300; s++) {
      run(s, (uint8_t)(s % 3));
      /* real seconds: action ticks at 120ms, idle advances a cycle every 4 ticks */
      uint32_t secs = (g_action_cycles * TICK_MS + g_idle_cycles * TICK_MS * IDLE_DIVISOR) / 1000;
      tot += secs; n++;
      if (secs < mn) mn = secs;
      if (secs > mx) mx = secs;
      dec += g.decisions;
      if (g.decisions < ev_min) ev_min = g.decisions;
      if (g.decisions > ev_max) ev_max = g.decisions;
    }
    printf("session seconds: mean %lu  min %lu  max %lu\n",
           (unsigned long)(tot / n), (unsigned long)mn, (unsigned long)mx);
    printf("decisions per run: mean %lu  min %lu  max %lu\n",
           (unsigned long)(dec / n), (unsigned long)ev_min, (unsigned long)ev_max);
    /* The design target is a 5-15 minute session played in one sitting. */
    assert(tot / n >= 240 && tot / n <= 900);
    assert(dec / n >= 4);        /* a session without decisions is not this game */
  }

  event_matrix();
  resume_mid_event();

  printf("300 runs: %d launched, %d collapsed, %d sudden death\n", launched, collapsed, sudden);
  assert(launched > 0);      /* the mission must be completable */
  assert(collapsed + sudden > 0 || launched == 300);
  printf("ok\n");
  return 0;
}
