// MSVC has no pthread.h. The host core and the Pd object only ever take a
// plain, non-recursive mutex — pthread_mutex_init/lock/unlock/destroy and
// the pthread_mutex_t itself, no threads and no condition variables — so
// this provides exactly that much over the native SRW lock rather than
// making every Windows build install pthreads4w.
//
// CMakeLists.txt only puts this directory on the include path when the
// toolchain has no pthread.h of its own, so a real pthreads always wins.
#ifndef WCLAP_VENDOR_PTHREAD_H
#define WCLAP_VENDOR_PTHREAD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK pthread_mutex_t;

// Only the default attributes are ever asked for (init is always called
// with NULL), so the type is opaque.
typedef struct {
  int unused;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

// An SRW lock cannot be re-entered, exactly like a default (non-recursive)
// pthread mutex, so the locking discipline in the callers is unchanged.
int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);

#endif
