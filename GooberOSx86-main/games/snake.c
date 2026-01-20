#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include <stdint.h>

#define WIDTH 30
#define HEIGHT 20
#define SNAKE_MAX_LEN (WIDTH * HEIGHT)

typedef struct {
    int x, y;
} Point;


static int exit_game = 0;

static Point snake[SNAKE_MAX_LEN];
static int snake_length;
static Point food;
static int direction; // 0=up,1=right,2=down,3=left
static int game_over;
static int score;

static unsigned int rand_seed = 123456789;

unsigned int rand() {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % 32768;
}

static void place_food() {
    int valid = 0;
    while (!valid) {
        food.x = rand() % WIDTH;
        food.y = rand() % HEIGHT;
        valid = 1;
        for (int i = 0; i < snake_length; i++) {
            if (snake[i].x == food.x && snake[i].y == food.y) {
                valid = 0;
                break;
            }
        }
    }
}

static int point_equal(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

static void draw_char(int x, int y, char c, uint8_t color) {
    vga_put_char_at(c, x + 25, y + 2, color);
}

static void draw_border() {
    for (int x = 0; x < WIDTH + 2; x++) {
        draw_char(x, 0, '#', 0x06);
        draw_char(x, HEIGHT + 1, '#', 0x06);
    }
    for (int y = 1; y <= HEIGHT; y++) {
        draw_char(0, y, '#', 0x06);
        draw_char(WIDTH + 1, y, '#', 0x06);
    }
}

static void draw_score() {
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put_char_at(' ', x, 0, 0x0F);
    }
    const char *score_str = "Score: ";
    int i = 0;
    while (score_str[i]) {
        vga_put_char_at(score_str[i], i, 0, 0x0F);
        i++;
    }
    int s = score;
    int digits[10];
    int dcount = 0;
    if (s == 0) {
        digits[dcount++] = 0;
    } else {
        while (s > 0 && dcount < 10) {
            digits[dcount++] = s % 10;
            s /= 10;
        }
    }
    for (int j = dcount - 1; j >= 0; j--) {
        vga_put_char_at('0' + digits[j], i++, 0, 0x0F);
    }
}

static void draw_game() {
    for (int y = 1; y <= HEIGHT; y++) {
        for (int x = 1; x <= WIDTH; x++) {
            char ch = ' ';
            uint8_t color = 0x0F;
            Point p = { x - 1, y - 1 };
            for (int i = 0; i < snake_length; i++) {
                if (point_equal(p, snake[i])) {
                    if (i == 0) {
                        ch = 'O';
                        color = VGA_COLOR_LIGHT_GREEN;
                    } else if (i == 1) {
                        ch = 'o';
                        color = VGA_COLOR_LIGHT_GREEN;
                    } else {
                        ch = 'o';
                        color = VGA_COLOR_LIGHT_GREEN;
                    }
                    break;
                }
            }
            if (point_equal(p, food)) {
                ch = '*';
                color = VGA_COLOR_LIGHT_RED;
            }
            draw_char(x, y, ch, color);
        }
    }
}

static void game_over_screen() {
    const char *msg = "GAME OVER! Press R to restart";
    int len = 26;
    int start_x = (VGA_WIDTH - len) / 2;
    int y = HEIGHT + 3;
    for (int i = 0; i < len; i++) {
        vga_put_char_at(msg[i], start_x + i, y, 0x0C);
    }
}

static void reset_game() {
    snake_length = 5;
    for (int i = 0; i < snake_length; i++) {
        snake[i].x = 10 - i;
        snake[i].y = 10;
    }
    direction = 1;
    score = 0;
    game_over = 0;
    place_food();
    vga_set_cursor(HEIGHT + 4, 0);
}

static void update_snake() {
    if (game_over) return;

    Point new_head = snake[0];

    switch (direction) {
        case 0: new_head.y--; break;
        case 1: new_head.x++; break;
        case 2: new_head.y++; break;
        case 3: new_head.x--; break;
    }

    if (new_head.x < 0 || new_head.x >= WIDTH || new_head.y < 0 || new_head.y >= HEIGHT) {
        game_over = 1;
        return;
    }

    for (int i = 0; i < snake_length; i++) {
        if (point_equal(new_head, snake[i])) {
            game_over = 1;
            return;
        }
    }

    for (int i = snake_length; i > 0; i--) {
        snake[i] = snake[i - 1];
    }
    snake[0] = new_head;

    if (point_equal(new_head, food)) {
        snake_length++;
        if (snake_length > SNAKE_MAX_LEN) snake_length = SNAKE_MAX_LEN;
        score += 10;
        place_food();
    }
}

static void handle_input() {
    while (keyboard_has_char()) {
        unsigned char c = (unsigned char)keyboard_read_char();
        if (c == 3) {
            game_over = 1;
            exit_game = 1;
            return;
        }
        if (game_over) {
            if (c == 'r' || c == 'R') {
                reset_game();
            }
            continue;
        }
        switch (c) {
            case 'w': case 'W': if (direction != 2) direction = 0; break;
            case 'd': case 'D': if (direction != 3) direction = 1; break;
            case 's': case 'S': if (direction != 0) direction = 2; break;
            case 'a': case 'A': if (direction != 1) direction = 3; break;
            case 0x1B:  // F1 key
                game_over = 1;
                exit_game = 1;
                break;
        }
    }
}




void snake_game_loop() {
    handle_input();
    update_snake();
    draw_border();
    draw_game();
    draw_score();

    if (game_over) {
        game_over_screen();
    }
}

void run_snake_game() {
    reset_game();
    exit_game = 0;

    while (!exit_game) {
        snake_game_loop();

        if (game_over) {
            while (!exit_game) {
                if (keyboard_has_char()) {
                    char c = keyboard_read_char();
                    if (c == 'r' || c == 'R') {
                        reset_game();
                        break;
                    } else if ((unsigned char)c == 0x1B) {  // ESC key
                        exit_game = 1;
                        break;
                    }
                }
                timer_sleep(50);
            }
        }

        timer_sleep(25);
    }
}