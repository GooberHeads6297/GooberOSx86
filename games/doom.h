#ifndef DOOM_H
#define DOOM_H

#include <stdint.h>
#pragma once

#define DOOM_MAP_W 24
#define DOOM_MAP_H 24

/* Text-shell entry (opens VESA Doom window). */
void run_doom_game(void);

/* Desktop / Start-menu entry. */
void open_doom_window(void);

#endif
