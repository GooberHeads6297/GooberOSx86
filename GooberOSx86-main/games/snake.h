#ifndef SNAKE_H
#define SNAKE_H

#include <stdint.h>

#define WIDTH 30
#define HEIGHT 20
#define SNAKE_MAX_LEN (WIDTH * HEIGHT)

typedef struct {
    int x, y;
} Point;

void run_snake_game();

void snake_game_loop();

#endif
