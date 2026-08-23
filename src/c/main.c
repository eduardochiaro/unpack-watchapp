#include <pebble.h>
#include "game.h"
#include "ui.h"

int main(void) {
  ui_init();   // the main menu decides whether to resume or generate a system
  app_event_loop();
  ui_deinit();
  return 0;
}
