#ifndef PX68K_MUTEX_LOCK_H
#define PX68K_MUTEX_LOCK_H

/* Opaque wrapper around a Win32 CRITICAL_SECTION.
 *
 * frontend_core.c needs a lock but also needs px68k-libretro/libretro/prop.h
 * (for Config), which pulls in libretro/compiler.h - a "win32 emulation"
 * shim meant for *non*-Windows builds. That header unconditionally #defines
 * GENERIC_READ/OPEN_EXISTING/etc. and redeclares GetPrivateProfileString
 * with a different signature than the real one, with no guard against
 * <windows.h> having already been seen. Including real <windows.h> in the
 * same translation unit as prop.h is therefore a hard compile error (not
 * just a redefinition warning) - confirmed by trying it. Keeping the real
 * CRITICAL_SECTION type behind this opaque handle lets frontend_core.c use
 * a lock without ever including <windows.h> itself. */
typedef struct px68k_lock px68k_lock_t;

px68k_lock_t *px68k_lock_create(void);
void px68k_lock_destroy(px68k_lock_t *lock);
void px68k_lock_enter(px68k_lock_t *lock);
void px68k_lock_leave(px68k_lock_t *lock);

#endif /* PX68K_MUTEX_LOCK_H */
