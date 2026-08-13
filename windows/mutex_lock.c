#include "mutex_lock.h"

#include <stdlib.h>
#include <windows.h>

struct px68k_lock {
    CRITICAL_SECTION cs;
};

px68k_lock_t *px68k_lock_create(void)
{
    px68k_lock_t *lock = (px68k_lock_t *)malloc(sizeof(*lock));
    if (!lock)
        return NULL;
    InitializeCriticalSection(&lock->cs);
    return lock;
}

void px68k_lock_destroy(px68k_lock_t *lock)
{
    if (!lock)
        return;
    DeleteCriticalSection(&lock->cs);
    free(lock);
}

void px68k_lock_enter(px68k_lock_t *lock)
{
    EnterCriticalSection(&lock->cs);
}

void px68k_lock_leave(px68k_lock_t *lock)
{
    LeaveCriticalSection(&lock->cs);
}
