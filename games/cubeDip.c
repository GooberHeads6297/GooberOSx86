#include "../../drivers/video/vga.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/timer.h"
#include <stdint.h>

#define WIDTH 10
#define HEIGHT 20

typedef struct {
    int x, y;
} Point;

typedef struct {
    Point blocks[4];
    uint8_t color;
} Piece;

static int exit_game = 0;
static int board[WIDTH][HEIGHT];
static Piece current_piece;
static Point piece_position;
static int game_over;
static int score;

static unsigned int rand_seed = 987654321;

unsigned int cubeDip_rand() {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % 32768;
}

static Piece pieces[7] = {
    {{ {0,1},{1,1},{2,1},{3,1} }, 0x0C},
    {{ {0,0},{1,0},{0,1},{1,1} }, 0x0E},
    {{ {0,1},{1,1},{1,0},{2,0} }, 0x09},
    {{ {0,0},{1,0},{1,1},{2,1} }, 0x0A},
    {{ {0,0},{1,0},{2,0},{1,1} }, 0x0B},
    {{ {0,0},{0,1},{1,1},{2,1} }, 0x0D},
    {{ {2,0},{0,1},{1,1},{2,1} }, 0x0F}
};

static int x_offset;
static int y_offset;
static char block_glyph = '#';

static void calc_offsets() {
    x_offset = (VGA_WIDTH - (WIDTH + 2)) / 2;
    y_offset = 2;
}

static void reset_game() {
    for (int x = 0; x < WIDTH; x++)
        for (int y = 0; y < HEIGHT; y++)
            board[x][y] = 0;

    current_piece = pieces[cubeDip_rand() % 7];
    piece_position.x = WIDTH / 2 - 2;
    piece_position.y = 0;
    score = 0;
    game_over = 0;
    exit_game = 0;
    vga_set_cursor(VGA_HEIGHT - 1, 0);
}

static void draw_char(int x, int y, char c, uint8_t color) {
    vga_put_char_at(c, x + x_offset, y + y_offset, color);
}

static void clear_playfield_area() {
    int total_w = WIDTH + 2;
    int total_h = HEIGHT + 2;
    for (int yy = 0; yy < total_h; yy++)
        for (int xx = 0; xx < total_w; xx++)
            vga_put_char_at(' ', x_offset + xx, y_offset + yy, 0x0F);
}

static void draw_border() {
    for (int x = 0; x < WIDTH + 2; x++) {
        draw_char(x, 0, block_glyph, 0x06);
        draw_char(x, HEIGHT + 1, block_glyph, 0x06);
    }
    for (int y = 1; y <= HEIGHT; y++) {
        draw_char(0, y, block_glyph, 0x06);
        draw_char(WIDTH + 1, y, block_glyph, 0x06);
    }
}

static void draw_board() {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            char ch = ' ';
            uint8_t color = 0x0F;
            if (board[x][y]) {
                ch = block_glyph;
                color = board[x][y];
            }
            draw_char(x + 1, y + 1, ch, color);
        }
    }
    for (int i = 0; i < 4; i++) {
        int bx = piece_position.x + current_piece.blocks[i].x;
        int by = piece_position.y + current_piece.blocks[i].y;
        if (by >= 0 && by < HEIGHT && bx >= 0 && bx < WIDTH)
            draw_char(bx + 1, by + 1, block_glyph, current_piece.color);
    }
}

static int check_collision(int nx, int ny, Piece p) {
    for (int i = 0; i < 4; i++) {
        int bx = nx + p.blocks[i].x;
        int by = ny + p.blocks[i].y;
        if (bx < 0 || bx >= WIDTH || by >= HEIGHT)
            return 1;
        if (by >= 0 && board[bx][by])
            return 1;
    }
    return 0;
}

static void place_piece() {
    for (int i = 0; i < 4; i++) {
        int bx = piece_position.x + current_piece.blocks[i].x;
        int by = piece_position.y + current_piece.blocks[i].y;
        if (by >= 0 && by < HEIGHT && bx >= 0 && bx < WIDTH)
            board[bx][by] = current_piece.color;
    }
}

static void clear_lines() {
    for (int y = HEIGHT - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < WIDTH; x++) {
            if (!board[x][y]) {
                full = 0;
                break;
            }
        }
        if (full) {
            for (int ty = y; ty > 0; ty--)
                for (int x = 0; x < WIDTH; x++)
                    board[x][ty] = board[x][ty - 1];
            for (int x = 0; x < WIDTH; x++)
                board[x][0] = 0;
            score += 100;
            y++;
        }
    }
}

static void rotate_piece() {
    Piece rotated = current_piece;
    for (int i = 0; i < 4; i++) {
        int x = current_piece.blocks[i].x;
        int y = current_piece.blocks[i].y;
        rotated.blocks[i].x = y;
        rotated.blocks[i].y = 3 - x;
    }
    if (!check_collision(piece_position.x, piece_position.y, rotated))
        current_piece = rotated;
    else if (!check_collision(piece_position.x - 1, piece_position.y, rotated)) {
        piece_position.x--;
        current_piece = rotated;
    } else if (!check_collision(piece_position.x + 1, piece_position.y, rotated)) {
        piece_position.x++;
        current_piece = rotated;
    }
}

static void handle_input() {
    while (keyboard_has_char()) {
        unsigned char c = (unsigned char)keyboard_read_char();
        if (c == 3 || c == 0x1B) {
            exit_game = 1;
            return;
        }
        if (c == 'a' || c == 'A') {
            if (!check_collision(piece_position.x - 1, piece_position.y, current_piece))
                piece_position.x--;
        } else if (c == 'd' || c == 'D') {
            if (!check_collision(piece_position.x + 1, piece_position.y, current_piece))
                piece_position.x++;
        } else if (c == 's' || c == 'S') {
            static unsigned int last_fast_drop = 0;
            unsigned int now = timer_ticks();
            if (now - last_fast_drop > 10) { // faster drop while holding S
                if (!check_collision(piece_position.x, piece_position.y + 1, current_piece))
                    piece_position.y++;
                last_fast_drop = now;
            }
        } else if (c == 'w' || c == 'W') {
            rotate_piece();
        }
    }
}



static void update_piece() {
    if (!check_collision(piece_position.x, piece_position.y + 1, current_piece)) {
        piece_position.y++;
    } else {
        place_piece();
        clear_lines();
        current_piece = pieces[cubeDip_rand() % 7];
        piece_position.x = WIDTH / 2 - 2;
        piece_position.y = 0;
        if (check_collision(piece_position.x, piece_position.y, current_piece))
            game_over = 1;
    }
}

static void draw_score() {
    for (int x = 0; x < VGA_WIDTH; x++) vga_put_char_at(' ', x, 0, 0x0F);
    const char *str = "Score: ";
    int i = 0;
    while (str[i]) vga_put_char_at(str[i], i++, 0, 0x0F);
    int s = score, digits[10], dcount = 0;
    if (s == 0) digits[dcount++] = 0;
    while (s > 0 && dcount < 10) {
        digits[dcount++] = s % 10;
        s /= 10;
    }
    for (int j = dcount - 1; j >= 0; j--) vga_put_char_at('0' + digits[j], i++, 0, 0x0F);
}

static void game_over_screen() {
    const char *msg = "GAME OVER! Press R to restart";
    int len = 29;
    int start_x = (VGA_WIDTH - len) / 2;
    int y = y_offset + HEIGHT + 2;
    for (int i = 0; i < len; i++)
        vga_put_char_at(msg[i], start_x + i, y, 0x0C);
}

static unsigned int last_drop_time = 0;

void cubeDip_game_loop() {
    unsigned int now = timer_ticks();
    handle_input();

    if (!game_over) {
        if (now - last_drop_time > 50) {
            update_piece();
            last_drop_time = now;
        }
    }

    draw_board(); 
    draw_score();
    if (game_over)
        game_over_screen();
}

void run_cubeDip_game() {
    calc_offsets();
    clear_playfield_area();
    draw_border();
    reset_game();
    while (!exit_game) {
        cubeDip_game_loop();
        if (game_over) {
            while (!exit_game) {
                if (keyboard_has_char()) {
                    char c = keyboard_read_char();
                    if (c == 'r' || c == 'R') {
                        reset_game();
                        break;
                    } else if ((unsigned char)c == 0x1B) {
                        exit_game = 1;
                        break;
                    }
                }
                timer_sleep(30);
            }
        }
        timer_sleep(15);
    }
}
