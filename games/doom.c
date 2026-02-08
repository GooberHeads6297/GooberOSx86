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

// Integer sine/cosine tables
static int8_t sin_table[360] = {
0,2,3,5,6,8,9,11,12,14,15,17,18,20,21,23,24,26,27,29,30,32,33,35,36,38,39,41,42,44,45,47,48,50,51,53,54,56,57,59,60,62,63,65,66,68,69,71,72,74,75,77,78,80,81,83,84,86,87,89,90,92,93,95,96,98,99,101,102,104,105,107,108,110,111,113,114,116,117,119,120,122,123,125,126,128,129,131,132,134,135,137,138,140,141,143,144,146,147,149,150,152,153,155,156,158,159,161,162,164,165,167,168,170,171,173,174,176,177,179,180,182,183,185,186,188,189,191,192,194,195,197,198,200,201,203,204,206,207,209,210,212,213,215,216,218,219,221,222,224,225,227,228,230,231,233,234,236,237,239,240,242,243,245,246,248,249,251,252,254,255,255,254,253,252,251,250,249,248,247,246,245,244,243,242,241,240,239,238,237,236,235,234,233,232,231,230,229,228,227,226,225,224,223,222,221,220,219,218,217,216,215,214,213,212,211,210,209,208,207,206,205,204,203,202,201,200,199,198,197,196,195,194,193,192,191,190,189,188,187,186,185,184,183,182,181,180,179,178,177,176,175,174,173,172,171,170,169,168,167,166,165,164,163,162,161,160,159,158,157,156,155,154,153,152,151,150,149,148,147,146,145,144,143,142,141,140,139,138,137,136,135,134,133,132,131,130,129,128,127,126,125,124,123,122,121,120,119,118,117,116,115,114,113,112,111,110,109,108,107,106,105,104,103,102,101,100,99,98,97,96,95,94,93,92,91,90,89,88,87,86,85,84,83,82,81,80,79,78,77,76,75,74,73,72,71,70,69,68,67,66,65,64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1
};
static int8_t cos_table[360];
static void init_trig_tables() {
    for (int i = 0; i < 360; i++) cos_table[i] = sin_table[(i + 90) % 360];
}

/* Raycasting with detailed ASCII and floor; out-of-bounds counts as wall so no void */
static void cast_ray(int col, int dir_x, int dir_y) {
    int ray_x = player_x >> FIXED_SHIFT;
    int ray_y = player_y >> FIXED_SHIFT;
    int dist = 0;
    int hit_wall = 0;
    if (dir_x == 0 && dir_y == 0) { dir_x = 1; dir_y = 0; }

    while (dist < 50) {
        ray_x += dir_x;
        ray_y += dir_y;
        if (ray_x < 0 || ray_x >= DOOM_MAP_W || ray_y < 0 || ray_y >= DOOM_MAP_H) {
            hit_wall = 1;
            break;
        }
        if (world_map[ray_y][ray_x]) {
            hit_wall = 1;
            break;
        }
        dist++;
    }
    if (!hit_wall) dist = 49;

    int wall_h = SCREEN_H * FIXED_ONE / (dist + 1);
    int start = (SCREEN_H * FIXED_ONE - wall_h) / 2 >> FIXED_SHIFT;
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

    for (int y = 0; y < SCREEN_H; y++) {
        if (y >= start && y <= end) {
            vga_put_char_at(shade, col, y, color | (VGA_COLOR_BLACK << 4));
        } else if (y > end) {
            int floor_dist = y - end;
            char floor_shade;
            uint8_t floor_color;
            if (floor_dist < 3)      { floor_shade = '~'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 6) { floor_shade = '.'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 10){ floor_shade = ':'; floor_color = VGA_COLOR_DARK_GREY; }
            else if (floor_dist < 16){ floor_shade = '\''; floor_color = VGA_COLOR_BLACK; }
            else                     { floor_shade = ','; floor_color = VGA_COLOR_BLACK; }
            vga_put_char_at(floor_shade, col, y, floor_color | (VGA_COLOR_BLACK << 4));
        } else {
            vga_put_char_at(' ', col, y, VGA_COLOR_BLACK | (VGA_COLOR_BLACK << 4));
        }
    }
}


static void render_scene(void) {
    for (int x = 0; x < SCREEN_W; x++) {
        int angle_offset = (x - SCREEN_W / 2) * FOV_ANGLE / SCREEN_W;
        int ray_angle = (player_dir + angle_offset + 360) % 360;
        int dir_x = cos_table[ray_angle] >> 7;
        int dir_y = -sin_table[ray_angle] >> 7;
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

        int move_dx = cos_table[player_dir] >> 7;
        int move_dy = -sin_table[player_dir] >> 7;

        if (keyboard_has_char()) {
            char c = keyboard_read_char();

            move_tick++;
            if (move_tick < 1) { timer_sleep(1); continue; }
            move_tick = 0;

            if (c == 'w' || c == 'W') {
                int nx = (player_x >> FIXED_SHIFT) + move_dx;
                int ny = (player_y >> FIXED_SHIFT) + move_dy;
                if (!world_map[ny][nx]) { player_x += move_dx << FIXED_SHIFT; player_y += move_dy << FIXED_SHIFT; }
            } else if (c == 's' || c == 'S') {
                int nx = (player_x >> FIXED_SHIFT) - move_dx;
                int ny = (player_y >> FIXED_SHIFT) - move_dy;
                if (!world_map[ny][nx]) { player_x -= move_dx << FIXED_SHIFT; player_y -= move_dy << FIXED_SHIFT; }
            } else if (c == 'a' || c == 'A') { player_dir = (player_dir + 355) % 360; }
            else if (c == 'd' || c == 'D') { player_dir = (player_dir + 5) % 360; }
            else if ((unsigned char)c == 0x1B) { exit_game = 1; }
        }

        timer_sleep(1);
    }

    clear_screen();
    print("Exited Doom Prototype\n");
}
