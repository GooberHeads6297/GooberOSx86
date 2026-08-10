#include "doom.h"
#include "../gui/vesa_window.h"
#include "../drivers/keyboard/keyboard.h"
#include "../lib/memory.h"
#include "../lib/string.h"
#include <stdint.h>

#define FB_W 320
#define FB_H 200
#define FIXED_SHIFT 8
#define FIXED_ONE (1 << FIXED_SHIFT)
#define FOV_ANGLE 40

typedef struct {
    uint8_t map[DOOM_MAP_H][DOOM_MAP_W];
    uint32_t* fb; /* FB_W * FB_H RGB */
    int player_x, player_y; /* fixed */
    int player_dir;
    uint32_t rng;
    int16_t sin_t[360];
    int16_t cos_t[360];
    int keys_w, keys_a, keys_s, keys_d;
} DoomApp;

static uint32_t doom_rand(DoomApp* app) {
    app->rng = app->rng * 1664525u + 1013904223u;
    return app->rng;
}

static int16_t approx_sin_deg(int degrees) {
    int sign = 1;
    int x = degrees % 360;
    if (x < 0) x += 360;
    if (x >= 180) { sign = -1; x -= 180; }
    if (x > 90) x = 180 - x;
    {
        int numerator = 4 * x * (180 - x) * FIXED_ONE;
        int denominator = 40500 - (x * (180 - x));
        return (int16_t)(sign * (numerator / denominator));
    }
}

static void init_trig(DoomApp* app) {
    int i;
    for (i = 0; i < 360; i++) {
        app->sin_t[i] = approx_sin_deg(i);
        app->cos_t[i] = approx_sin_deg(i + 90);
    }
}

static void generate_map(DoomApp* app) {
    uint8_t tmp[DOOM_MAP_H][DOOM_MAP_W];
    int x, y, iter;
    for (y = 0; y < DOOM_MAP_H; y++) {
        for (x = 0; x < DOOM_MAP_W; x++) {
            if (x == 0 || y == 0 || x == DOOM_MAP_W - 1 || y == DOOM_MAP_H - 1)
                app->map[y][x] = 1;
            else
                app->map[y][x] = (doom_rand(app) % 100) < 42;
        }
    }
    for (iter = 0; iter < 4; iter++) {
        for (y = 0; y < DOOM_MAP_H; y++) {
            for (x = 0; x < DOOM_MAP_W; x++) {
                int walls = 0, dy, dx;
                if (x == 0 || y == 0 || x == DOOM_MAP_W - 1 || y == DOOM_MAP_H - 1) {
                    tmp[y][x] = 1;
                    continue;
                }
                for (dy = -1; dy <= 1; dy++)
                    for (dx = -1; dx <= 1; dx++)
                        if (app->map[y + dy][x + dx]) walls++;
                tmp[y][x] = (walls >= 5 || walls <= 2) ? 1 : 0;
            }
        }
        for (y = 0; y < DOOM_MAP_H; y++)
            for (x = 0; x < DOOM_MAP_W; x++)
                app->map[y][x] = tmp[y][x];
    }
    /* carve spawn */
    app->map[DOOM_MAP_H / 2][DOOM_MAP_W / 2] = 0;
    app->map[DOOM_MAP_H / 2][DOOM_MAP_W / 2 + 1] = 0;
    app->map[DOOM_MAP_H / 2 + 1][DOOM_MAP_W / 2] = 0;
}

static int can_move(DoomApp* app, int x, int y) {
    int mx = x >> FIXED_SHIFT;
    int my = y >> FIXED_SHIFT;
    if (mx < 0 || mx >= DOOM_MAP_W || my < 0 || my >= DOOM_MAP_H) return 0;
    return app->map[my][mx] == 0;
}

static void try_move(DoomApp* app, int sx, int sy) {
    int nx = app->player_x + sx;
    int ny = app->player_y + sy;
    if (can_move(app, nx, app->player_y)) app->player_x = nx;
    if (can_move(app, app->player_x, ny)) app->player_y = ny;
}

static uint32_t shade_wall(int dist, int side) {
    int v = 220 - dist * 6;
    if (v < 40) v = 40;
    if (side) v = (v * 3) / 4;
    return ((uint32_t)v << 16) | ((uint32_t)(v / 2) << 8) | (uint32_t)(v / 3);
}

static uint32_t shade_floor(int y) {
    int v = 20 + (y * 40) / FB_H;
    return ((uint32_t)(v / 2) << 16) | ((uint32_t)v << 8) | (uint32_t)(v / 3);
}

static uint32_t shade_ceil(int y) {
    int v = 50 - (y * 30) / FB_H;
    if (v < 10) v = 10;
    return ((uint32_t)(v / 3) << 16) | ((uint32_t)(v / 3) << 8) | (uint32_t)v;
}

static void cast_column(DoomApp* app, int col) {
    int angle_off = (col - FB_W / 2) * FOV_ANGLE / FB_W;
    int ray_ang = (app->player_dir + angle_off + 360) % 360;
    int dir_x = app->cos_t[ray_ang];
    int dir_y = -app->sin_t[ray_ang];
    int ray_x = app->player_x;
    int ray_y = app->player_y;
    int dist = 0;
    int hit = 0;
    int side = 0;
    int prev_x = ray_x;
    int y, start, end, wall_h;

    if (dir_x == 0 && dir_y == 0) dir_x = 1;
    while (dist < 80) {
        prev_x = ray_x;
        ray_x += dir_x;
        ray_y += dir_y;
        {
            int mx = ray_x >> FIXED_SHIFT;
            int my = ray_y >> FIXED_SHIFT;
            if (mx < 0 || mx >= DOOM_MAP_W || my < 0 || my >= DOOM_MAP_H) {
                hit = 1; break;
            }
            if (app->map[my][mx]) {
                hit = 1;
                side = ((prev_x >> FIXED_SHIFT) != mx) ? 1 : 0;
                break;
            }
        }
        dist++;
    }
    if (!hit) dist = 79;
    if (dist < 1) dist = 1;

    wall_h = (FB_H * FIXED_ONE) / dist;
    start = ((FB_H * FIXED_ONE - wall_h) / 2) >> FIXED_SHIFT;
    end = start + (wall_h >> FIXED_SHIFT);
    if (start < 0) start = 0;
    if (end >= FB_H) end = FB_H - 1;

    for (y = 0; y < FB_H; y++) {
        uint32_t c;
        if (y < start) c = shade_ceil(y);
        else if (y <= end) c = shade_wall(dist, side);
        else c = shade_floor(y);
        app->fb[y * FB_W + col] = c;
    }
}

static void doom_render_scene(DoomApp* app) {
    int x;
    for (x = 0; x < FB_W; x++) cast_column(app, x);
}

static void doom_dirty_client(VWindow* win) {
    int cx = win->x + BORDER_SIZE;
    int cy = win->y + TITLEBAR_HEIGHT + BORDER_SIZE;
    int cw = win->width - BORDER_SIZE * 2;
    int ch = win->height - TITLEBAR_HEIGHT - BORDER_SIZE * 2;
    if (cw < 1 || ch < 1) return;
    vdesk_mark_dirty(cx, cy, cw, ch);
}

static void doom_win_render(VWindow* win, int cx, int cy, int cw, int ch) {
    DoomApp* app = (DoomApp*)win->user_data;
    int x, y, bw, bh, cols, rows;
    if (!app || !app->fb) return;
    /* Fixed low present grid; block size grows with client area (high-res). */
    cols = 80;
    rows = 50;
    if (cw < cols) cols = cw > 0 ? cw : 1;
    if (ch < rows + 18) rows = ch > 20 ? ch - 20 : 1;
    bw = cw / cols;
    bh = (ch - 18) / rows;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    vdesk_draw_rect(cx, cy, cw, ch, 0x000000);
    for (y = 0; y < rows; y++) {
        int sy = (y * FB_H) / rows;
        for (x = 0; x < cols; x++) {
            int sx = (x * FB_W) / cols;
            vdesk_draw_rect(cx + x * bw, cy + y * bh, bw, bh,
                            app->fb[sy * FB_W + sx]);
        }
    }
    vdesk_draw_rect(cx, cy + ch - 18, cw, 18, 0x101010);
    vdesk_draw_text(cx + 6, cy + ch - 15, "WASD move/turn  ESC close  GooberDoom",
                    0xCCCCCC, 0x101010);
}

static void doom_win_key(VWindow* win, char key) {
    DoomApp* app = (DoomApp*)win->user_data;
    unsigned char k;
    if (!app) return;
    k = (unsigned char)key;
    if (k == 0x1B) {
        if (app->fb) kfree(app->fb);
        kfree(app);
        win->user_data = NULL;
        vdesk_close_window(win);
        return;
    }
    /* Movement is handled in tick via keyboard_char_held (hold-to-move). */
    (void)k;
}

static void doom_win_tick(VWindow* win) {
    DoomApp* app = (DoomApp*)win->user_data;
    int dx, dy;
    int moved = 0;
    if (!app) return;
    keyboard_poll();
    dx = app->cos_t[app->player_dir] >> 2;
    dy = -app->sin_t[app->player_dir] >> 2;
    if (keyboard_char_held('w')) { try_move(app, dx, dy); moved = 1; }
    if (keyboard_char_held('s')) { try_move(app, -dx, -dy); moved = 1; }
    if (keyboard_char_held('a')) {
        app->player_dir = (app->player_dir + 350) % 360;
        moved = 1;
    }
    if (keyboard_char_held('d')) {
        app->player_dir = (app->player_dir + 10) % 360;
        moved = 1;
    }
    if (moved) {
        doom_render_scene(app);
        doom_dirty_client(win);
    }
}

static void doom_win_close_cleanup(VWindow* win) {
    DoomApp* app;
    if (!win) return;
    app = (DoomApp*)win->user_data;
    if (app) {
        if (app->fb) kfree(app->fb);
        kfree(app);
        win->user_data = NULL;
    }
}

void open_doom_window(void) {
    DoomApp* app;
    VWindow* win;
    app = (DoomApp*)kmalloc(sizeof(DoomApp));
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->fb = (uint32_t*)kmalloc(FB_W * FB_H * sizeof(uint32_t));
    if (!app->fb) { kfree(app); return; }
    app->rng = 0xC0FFEE;
    init_trig(app);
    generate_map(app);
    app->player_x = (DOOM_MAP_W / 2) << FIXED_SHIFT;
    app->player_y = (DOOM_MAP_H / 2) << FIXED_SHIFT;
    app->player_dir = 0;
    doom_render_scene(app);

    win = vdesk_create_window("GooberDoom", 100, 60, 640, 420);
    if (!win) {
        kfree(app->fb);
        kfree(app);
        return;
    }
    win->user_data = app;
    win->render = doom_win_render;
    win->key_handler = doom_win_key;
    win->tick_handler = doom_win_tick;
    /* no close callback on VWindow — free via nulling when closed from key ESC
     * leak on titlebar close is acceptable; try attach if process_pid unused */
    (void)doom_win_close_cleanup;
    vdesk_notify("GooberDoom", "WASD to move — ESC closes");
    vdesk_mark_full_dirty();
}

void run_doom_game(void) {
    open_doom_window();
}
