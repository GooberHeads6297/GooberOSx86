#ifndef GOOBEROS_ARCH_H
#define GOOBEROS_ARCH_H

/* User-visible arch labels for menus, boot banners, and desktop chrome. */

#if defined(__i386__)
#define GOOBEROS_ARCH_SHORT "x86"
#define GOOBEROS_ARCH_LONG  "32-bit x86"
#elif defined(__x86_64__)
#define GOOBEROS_ARCH_SHORT "x64"
#define GOOBEROS_ARCH_LONG  "64-bit x86_64"
#else
#define GOOBEROS_ARCH_SHORT "unknown"
#define GOOBEROS_ARCH_LONG  "unknown architecture"
#endif

#define GOOBEROS_MENU_LABEL "GooberOS " GOOBEROS_ARCH_SHORT

#endif /* GOOBEROS_ARCH_H */
