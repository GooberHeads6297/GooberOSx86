#ifndef GOOBER_DOSEMU_H
#define GOOBER_DOSEMU_H

#include <stddef.h>
#include <stdint.h>

struct VWindow;

/* GooberDOS — BSD soft MS-DOS guest for GooberOS (not DOSBox). */

/* Open a DOSBox-style interactive session at C:\ (/Dos).
 * If path is non-NULL, also auto-run that .COM/.EXE then return to the prompt.
 * Pass NULL or "" for shell-only. Returns 0 on launch, <0 on error. */
int dos_exec(const char* path);

/* True if a GooberDOS session window is open. */
int dos_session_active(void);

/* Window close hook (from vdesk_close_window). */
void dos_on_window_closed(struct VWindow* win);

/* Embedded HELLO.COM bytes for seeding / fixtures. */
const uint8_t* dos_hello_com_bytes(size_t* out_len);

/* Ensure /Dos and /Dos/Apps exist; seed HELLO.COM when missing. */
void dos_seed_share(void);

#endif
