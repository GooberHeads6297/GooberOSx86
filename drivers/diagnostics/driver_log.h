#ifndef DRIVER_LOG_H
#define DRIVER_LOG_H

#include <stdint.h>

void driver_log_clear(void);
void driver_log(const char* msg);
void driver_log_u32(uint32_t value);
void driver_log_hex32(uint32_t value);
void driver_log_line(const char* msg);
void driver_log_dump(void (*write)(const char*));
const char* driver_log_buffer(void);
uint32_t driver_log_size(void);
uint32_t driver_log_version(void);

/* Write the current diagnostics buffer to /Desktop/log.txt when the in-memory
 * filesystem is available. Safe to call repeatedly. */
int driver_log_sync_desktop_file(void);

#endif
