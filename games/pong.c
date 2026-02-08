#include "../drivers/video/vga.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include "../lib/string.h"
#include "../shell/shell.h"
#include "pong.h"
#include <stdint.h>
#include <stddef.h>

#define PADDLE_HEIGHT 4
#define BALL_CHAR 'O'
#define PADDLE_CHAR '|'
#define FPS_DELAY 30000

static int ball_x, ball_y;
static int ball_dx = 1, ball_dy = 1;
static int paddle_y;
static int ai_paddle_y;
static int player_score = 0, ai_score = 0;

static int exit_game = 0;

static void draw_paddle(int x, int y) {
    for (int i = 0; i < PADDLE_HEIGHT; i++) {
        vga_put_char_at(PADDLE_CHAR, x, y + i, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
    }
}

static void erase_paddle(int x, int y) {
    for (int i = 0; i < PADDLE_HEIGHT; i++) {
        vga_put_char_at(' ', x, y + i, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
    }
}

static void draw_ball(int x, int y) {
    vga_put_char_at(BALL_CHAR, x, y, VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4));
}

static void erase_ball(int x, int y) {
    vga_put_char_at(' ', x, y, VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4));
}

static void reset_ball(void) {
    ball_x = VGA_WIDTH / 2;
    ball_y = VGA_HEIGHT / 2;
    ball_dx = 1;
    ball_dy = 1;
}

static void reset_game(void) {
    clear_screen();
    paddle_y = VGA_HEIGHT / 2 - PADDLE_HEIGHT / 2;
    ai_paddle_y = VGA_HEIGHT / 2 - PADDLE_HEIGHT / 2;
    reset_ball();
    player_score = 0;
    ai_score = 0;
    exit_game = 0;
}

void run_pong_game(void) {
    reset_game();

    while (!exit_game) {
        // Erase previous positions
        erase_ball(ball_x, ball_y);
        erase_paddle(0, paddle_y);
        erase_paddle(VGA_WIDTH - 1, ai_paddle_y);

        // Update ball position
        ball_x += ball_dx;
        ball_y += ball_dy;

        // Bounce ball on top/bottom
        if (ball_y <= 0) { ball_y = 0; ball_dy = 1; }
        if (ball_y >= VGA_HEIGHT - 1) { ball_y = VGA_HEIGHT - 1; ball_dy = -1; }

        // Bounce ball on paddles
        if (ball_x == 1 && ball_y >= paddle_y && ball_y < paddle_y + PADDLE_HEIGHT) ball_dx = 1;
        if (ball_x == VGA_WIDTH - 2 && ball_y >= ai_paddle_y && ball_y < ai_paddle_y + PADDLE_HEIGHT) ball_dx = -1;

        // Score handling
        if (ball_x <= 0) { ai_score++; reset_ball(); }
        if (ball_x >= VGA_WIDTH - 1) { player_score++; reset_ball(); }

        // AI movement with clamping
        if (ball_y > ai_paddle_y + PADDLE_HEIGHT / 2) ai_paddle_y++;
        if (ball_y < ai_paddle_y + PADDLE_HEIGHT / 2) ai_paddle_y--;
        if (ai_paddle_y < 0) ai_paddle_y = 0;
        if (ai_paddle_y > VGA_HEIGHT - PADDLE_HEIGHT) ai_paddle_y = VGA_HEIGHT - PADDLE_HEIGHT;

        // Player input with clamping
        if (keyboard_has_char()) {
            char c = keyboard_read_char();
            if (c == 'w' || c == 'W') paddle_y--;
            if (c == 's' || c == 'S') paddle_y++;
            if ((unsigned char)c == 0x1B) exit_game = 1;  // ESC key

            if (paddle_y < 0) paddle_y = 0;
            if (paddle_y > VGA_HEIGHT - PADDLE_HEIGHT) paddle_y = VGA_HEIGHT - PADDLE_HEIGHT;
        }

        // Draw ball and paddles
        draw_ball(ball_x, ball_y);
        draw_paddle(0, paddle_y);
        draw_paddle(VGA_WIDTH - 1, ai_paddle_y);

        // Draw score on top row
        char score_str[16];
        itoa(player_score, score_str, 10);
        for (int i = 0; score_str[i] != '\0'; i++) {
            vga_put_char_at(score_str[i], 8 + i, 0, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
        }
        const char* player_label = "Player: ";
        for (int i = 0; player_label[i] != '\0'; i++) {
            vga_put_char_at(player_label[i], i, 0, VGA_COLOR_LIGHT_GREEN | (VGA_COLOR_BLACK << 4));
        }
        
        itoa(ai_score, score_str, 10);
        for (int i = 0; score_str[i] != '\0'; i++) {
            vga_put_char_at(score_str[i], 20 + i, 0, VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4));
        }
        const char* ai_label = "AI: ";
        for (int i = 0; ai_label[i] != '\0'; i++) {
            vga_put_char_at(ai_label[i], 16 + i, 0, VGA_COLOR_LIGHT_RED | (VGA_COLOR_BLACK << 4));
        }
        timer_sleep(25);
    }

    clear_screen();
    print("Exited Pong\n");
}
