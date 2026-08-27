# unpack-watchapp

**Unpack - Pebble Lite.** A standalone von Neumann probe game for Pebble, built
to the design in `reference-files/game-pebble.md`. You arrive in a procedurally
generated system with a seed payload and no infrastructure, unpack yourself into
extraction and power, endure narrative events with no clean answers, and build
the infrastructure the human colonists will arrive to -- factories, colony
frames, and finally the orbital ring -- then break orbit for the next system. A
session runs 2-4 minutes, start to departure or failure. No phone dependency.

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

The mission screen opens with a readout band, one row of three columns: the
P/M/W pools stacked on the left, a pixel icon for whatever is highlighted in the
middle (the body's type in SYSTEM, the op in OPS), and the running action, the
clock and the progress bar on the right. It replaces the separate HUD and art
strip, so the rows get the height back -- 50px on emery and gabbro, 48px on the
rectangular 144-wide screens.

Round watches run the same three columns, just inside the circle rather than
across the full width. The cap of the circle is too narrow to hold a row of
text, so `BAND_TOP` starts the columns below it, and `band_pad()` insets each
row by the circle's chord at that row's height -- so the columns step outwards
as they descend and follow the bezel, rather than every row squaring off inside
the narrowest one. On a rectangle `band_pad()` answers the same 3px for every
row and the layout is the same code. `BAND_BIAS` nudges the icon off-centre so
the right-hand column, which has to hold an action name and `T+9999yr`, gets
more room than three-character pool figures need.

The icons live in `resources/art/` at 48x48 for the 200-wide-and-up screens and
24x24 for the rest, both bound to the same `RESOURCE_ID_ART_*` the way the
splash is, so the layout code never branches on platform for them. Only the
highlighted row's bitmap is resident: the band costs one bitmap of heap rather
than twelve.

- **Up / Down** - move through the SYSTEM and OPS lists
- **Select** - start the highlighted action, or confirm an event choice
- **Back** - leave the mission log or guide, or return from the mission to the menu

The last OPS row is **Guide**: a scrolling page explaining the readouts, the
body states, each op and how a run ends. Its text is `resources/guide.txt`,
loaded as a raw resource only while the window is open -- aplite has 24K for
code, data and heap together, and a permanent copy in rodata left too little
heap to scroll the menu. For the same reason aplite caps the mission log at 12
entries where the other platforms hold 30.

Construction is a chain: **Factory** shortens every later build (each is worth
three workers, and does nothing for scans), **Colony frame** is what the ring
anchors to and eats 2 workers apiece, and **Orbital ring** unlocks only once 3
frames and 2 factories stand. Closing the ring ends the run. Until the
prerequisites are met the ring row reports what is still missing instead of a
price.

One action runs at a time. While an action runs, in-game time fast-forwards;
while idle, it still advances, just slower. Opening the mission log or the guide
stops the clock -- they are reference, not play -- and Back resumes the run
where it left off. Narrative events interrupt either
one and freeze time until answered. The run ends when the orbital ring closes
and the probe departs, when the system collapses, or when an event resolves
catastrophically -- then the mission log becomes the ledger.

One system per run. The multi-system runs and the carried bonus/malus legacy in
`reference-files/game-progression.md` are not in this version -- a Pebble
session ends at the first departure.

### Saving

The session is written to persistent storage when the app closes, and offered
as **Continue** next time. A run that has ended is not resumable -- showing its
ledger clears the save. Adding the factory and frame counters changed
`sizeof(GameState)`, and the meta key stores that size -- so saves written
before the construction chain invalidate themselves rather than loading as
nonsense. State is stored as a run of 256-byte chunks (the cap on
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
every choice. It asserts the invariants and the
measured pacing: no negative pools, no stalled runs, the mission stays
completable, the orbital ring stays gated on its frames and factories while
factories shorten builds and not scans, sessions stay in their measured length band, stellar events fire
at half the rate of the rest of the pool and spare the arrays half the time,
the ledger of a sudden ending names its cause even when the log has overflowed,
and a session round-tripped through a byte buffer rebuilds the same event panel
it was saved on.

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
