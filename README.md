# unpack-watchapp

**Unpack - Pebble Lite.** A standalone von Neumann probe game for Pebble, built
to the design in `reference-files/game-pebble.md`. You arrive in a procedurally
generated system with a seed payload and no infrastructure, unpack yourself into
extraction and power, endure narrative events with no clean answers, and build
the probes that continue the chain. A session runs 5-15 minutes, start to launch
or failure. No phone dependency.

## Building & running

```sh
pebble build                          # build for all targetPlatforms
pebble install --emulator emery       # install on the emery emulator
pebble install --phone <ip>           # install to a paired phone
```

## Target platforms

`targetPlatforms` in `package.json` controls which watches you build for. The
modern Pebble hardware is **emery** (Pebble Time 2), **gabbro** (Pebble Round
2), and **flint** (Pebble 2 Duo); the original Pebble platforms (aplite,
basalt, chalk, diorite) are included by default for backwards compatibility.

## Playing

The app opens on a main menu: the title, splash art between two rules, then the
rows. **Continue** appears first whenever a session is saved; **New session**
generates a fresh system and discards it.

The splash ships at two sizes -- `resources/splash-basalt.png` (144x51, used on
the 144-wide platforms and cropped by chalk's round bezel) and
`resources/splash-emery.png` (200x98, also used on gabbro). Both bind to the
same `RESOURCE_ID_SPLASH`, so the layout code never branches on platform.

- **Up / Down** - move through the SYSTEM and OPS lists
- **Select** - start the highlighted action, or confirm an event choice
- **Back** - leave the mission log or guide, or return from the mission to the menu

The last OPS row is **Guide**: a scrolling page explaining the readouts, the
body states, each op and how a run ends. Its text is `resources/guide.txt`,
loaded as a raw resource only while the window is open -- aplite has 24K for
code, data and heap together, and a permanent copy in rodata left too little
heap to scroll the menu. For the same reason aplite caps the mission log at 12
entries where the other platforms hold 30.

One action runs at a time. While an action runs, in-game time fast-forwards;
while idle, it still advances, just slower. Narrative events interrupt either
one and freeze time until answered. The run ends when probes launch, when the
system collapses, or when an event resolves catastrophically -- then the mission
log becomes the ledger.

### Saving

The session is written to persistent storage when the app closes, and offered
as **Continue** next time. A run that has ended is not resumable -- showing its
ledger clears the save. State is stored as a run of 256-byte chunks (the cap on
a single persist key), with the meta key written last and validated first, so a
half-finished write can never be read back as a save.

Note this departs from `game-pebble.md`, which specifies no save-and-resume.
One consequence: backing out to the menu stops the clock, so idle time spent at
the menu is free, where in-mission idle time is not.

## Source layout

| File | Contents |
|---|---|
| `src/c/game.c` | State, system generation, resource sim, actions, ledger |
| `src/c/events.c` | The narrative event pool, eligibility, and effects |
| `src/c/ui.c` | Menu, main screen, event panel, ledger; save/load; the sim clock |
| `src/c/main.c` | Entry point |

## Tests

`test/sim_test.c` compiles the pure game logic natively (against a small
`test/pebble.h` stand-in) and plays 300 headless runs plus every event through
every choice. It asserts the invariants and the design targets: no negative
pools, no stalled runs, the mission stays completable, sessions stay in the
5-15 minute band, and a session round-tripped through a byte buffer rebuilds
the same event panel it was saved on.

```sh
cc -std=c11 -Wall -I test -o /tmp/sim_test test/sim_test.c src/c/game.c src/c/events.c
/tmp/sim_test
```

## Project layout

```
src/c/           C source for the watchapp
src/pkjs/        PebbleKit JS (phone-side) source, if any
worker_src/c/    Background worker source, if any
resources/       Images, fonts, and other bundled resources
package.json     Project metadata (UUID, platforms, resources, message keys)
wscript          Build rules — usually no need to edit
```

By default this project is configured as a watchapp. To make it a watchface,
set `pebble.watchapp.watchface` to `true` in `package.json`.

## Documentation

Full SDK docs, tutorials, and API reference: <https://developer.repebble.com>
