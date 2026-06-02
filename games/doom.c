#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include "../lib/string.h"
#include "../shell/shell.h"
#include "doom.h"
#include <stdint.h>

#define SCREEN_W VGA_WIDTH
#define SCREEN_H VGA_HEIGHT
#define FIXED_SHIFT 8
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FOV_ANGLE 30

static uint8_t world_map[DOOM_MAP_H][DOOM_MAP_W];

static int player_x; // fixed-point
static int player_y; // fixed-point
static int player_dir; // degrees 0-359
static int exit_game;
static uint32_t rng_state = 1;
static uint32_t move_tick = 0;
static int trig_ready = 0;

static uint32_t rand_next(void) {
    rng_state = rng_state * 1664525 + 1013904223;
    return rng_state;
}

/* Cellular automata step for dungeon-like caves */
static void generate_map(void) {
    uint8_t tmp[DOOM_MAP_H][DOOM_MAP_W];
    int w = DOOM_MAP_W, h = DOOM_MAP_H;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                world_map[y][x] = 1;
            } else {
                world_map[y][x] = (rand_next() % 100) < 42;
            }
        }
    }
    for (int iter = 0; iter < 4; iter++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                    tmp[y][x] = 1;
                    continue;
                }
                int walls = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (world_map[y + dy][x + dx]) walls++;
                tmp[y][x] = (walls >= 5 || walls <= 2) ? 1 : 0;
            }
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                world_map[y][x] = tmp[y][x];
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                world_map[y][x] = 1;
        }
    }
}

// Signed fixed-point trig tables scaled to 256 units per tile.
static int16_t sin_table[360];
static int16_t cos_table[360];

static int16_t approx_sin_deg(int degrees) {
    int sign = 1;
    int x = degrees % 360;

    if (x < 0) x += 360;
    if (x >= 180) {
        sign = -1;
        x -= 180;
    }
    if (x > 90) x = 180 - x;

    // Bhaskara I approximation on [0, 180], scaled to FIXED_ONE.
    {
        int numerator = 4 * x * (180 - x) * FIXED_ONE;
        int denominator = 40500 - (x * (180 - x));
        int value = numerator / denominator;
        return (int16_t)(sign * value);
    }
}

static void init_trig_tables() {
    if (trig_ready) return;
    for (int i = 0; i < 360; i++) {
        sin_table[i] = approx_sin_deg(i);
        cos_table[i] = approx_sin_deg(i + 90);
    }
    trig_ready = 1;
}

/* Raycasting with detailed ASCII and floor; out-of-bounds counts as wall so no void */
static void cast_ray(int col, int dir_x, int dir_y) {
    int ray_x = player_x;
    int ray_y = player_y;
    int dist = 0;
    int hit_wall = 0;
    if (dir_x == 0 && dir_y == 0) { dir_x = 1; dir_y = 0; }

    while (dist < 64) {
        ray_x += dir_x;
        ray_y += dir_y;
        {
            int map_x = ray_x >> FIXED_SHIFT;
            int map_y = ray_y >> FIXED_SHIFT;
            if (map_x < 0 || map_x >= DOOM_MAP_W || map_y < 0 || map_y >= DOOM_MAP_H) {
                hit_wall = 1;
                break;
            }
            if (world_map[map_y][map_x]) {
                hit_wall = 1;
                break;
            }
        }
        dist++;
    }
    if (!hit_wall) dist = 63;

    if (dist < 1) dist = 1;

    int wall_h = SCREEN_H * FIXED_ONE / dist;
    int start = ((SCREEN_H * FIXED_ONE - wall_h) / 2) >> FIXED_SHIFT;
    int end = start + (wall_h >> FIXED_SHIFT);

    char shade;
    uint8_t color;
    if (dist <= 1) { shade = '#'; color = VGA_COLOR_WHITE; }
    else if (dist <= 2) { shade = '#'; color = VGA_COLOR_LIGHT_GREY; }
    else if (dist <= 3) { shade = 'X'; color = VGA_COLOR_LIGHT_GREY; }
    else if (dist <= 5) { shade = 'X'; color = VGA_COLOR_WHITE; }
    else if (dist <= 7) { shade = '='; color = VGA_COLOR_LIGHT_BROWN; }
    else if (dist <= 10) { shade = '|'; color = VGA_COLOR_BROWN; }
    else if (dist <= 14) { shade = ':'; color = VGA_COLOR_BROWN; }
    else if (dist <= 20) { shade = '-'; color = VGA_COLOR_DARK_GREY; }
    else { shade = '.'; color = VGA_COLOR_DARK_GREY; }

    /*
     * Phase 4 (item 5): writes go through vga_text_putc so the passthrough
     * shim can intercept on x64. x86 BIOS path: vga_text_putc forwards
     * directly to vga_put_char_at, no behavioural change.
     */
    for (int y = 0; y < SCREEN_H; y++) {
        if (y >= start && y <= end) {
            vga_text_putc(shade, col, y, color | (VGA_COLOR_BLACK << 4));
        } else if (y > end) {
            int floor_dist = y - end;
            char floor_shade;
            uint8_t floor_color;
            if (floor_dist < 3)      { floor_shade = '~'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 6) { floor_shade = '.'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 10){ floor_shade = ':'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 16){ floor_shade = '\''; floor_color = VGA_COLOR_BLACK; }
            else                     { floor_shade = ','; floor_color = VGA_COLOR_BLACK; }
            vga_text_putc(floor_shade, col, y, floor_color | (VGA_COLOR_BLACK << 4));
        } else {
            vga_text_putc(' ', col, y, VGA_COLOR_BLACK | (VGA_COLOR_BLACK << 4));
        }
    }
}

static int can_move_to(int x, int y) {
    int map_x = x >> FIXED_SHIFT;
    int map_y = y >> FIXED_SHIFT;

    if (map_x < 0 || map_x >= DOOM_MAP_W || map_y < 0 || map_y >= DOOM_MAP_H) {
        return 0;
    }
    return world_map[map_y][map_x] == 0;
}

static void try_move(int step_x, int step_y) {
    int next_x = player_x + step_x;
    int next_y = player_y + step_y;

    if (can_move_to(next_x, player_y)) player_x = next_x;
    if (can_move_to(player_x, next_y)) player_y = next_y;
}

static void render_scene(void) {
    for (int x = 0; x < SCREEN_W; x++) {
        int angle_offset = (x - SCREEN_W / 2) * FOV_ANGLE / SCREEN_W;
        int ray_angle = (player_dir + angle_offset + 360) % 360;
        int dir_x = cos_table[ray_angle];
        int dir_y = -sin_table[ray_angle];
        cast_ray(x, dir_x, dir_y);
    }
}

static void reset_game(void) {
    clear_screen();
    generate_map();
    init_trig_tables();
    player_x = (DOOM_MAP_W / 2) << FIXED_SHIFT;
    player_y = (DOOM_MAP_H / 2) << FIXED_SHIFT;
    player_dir = 0;
    exit_game = 0;
    move_tick = 0;
}

void run_doom_game(void) {
    reset_game();

    while (!exit_game) {
        render_scene();

        {
            int move_dx = cos_table[player_dir] >> 2;
            int move_dy = -sin_table[player_dir] >> 2;

            if (keyboard_has_char()) {
                char c = keyboard_read_char();

                move_tick++;
                if (move_tick < 1) { timer_sleep(1); continue; }
                move_tick = 0;

                if (c == 'w' || c == 'W') {
                    try_move(move_dx, move_dy);
                } else if (c == 's' || c == 'S') {
                    try_move(-move_dx, -move_dy);
                } else if (c == 'a' || c == 'A') { player_dir = (player_dir + 355) % 360; }
                else if (c == 'd' || c == 'D') { player_dir = (player_dir + 5) % 360; }
                else if ((unsigned char)c == 0x1B) { exit_game = 1; }
            }
        }

        timer_sleep(1);
    }

    clear_screen();
    print("Exited Doom Prototype\n");
}
