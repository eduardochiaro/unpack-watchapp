/* Host-side stand-in for the Pebble SDK header, so the pure game logic in
   game.c / events.c can be compiled and exercised natively by sim_test.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
