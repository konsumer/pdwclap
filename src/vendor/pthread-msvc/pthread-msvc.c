// See pthread.h next to this file: the mutex subset the host core and the
// Pd object use, over SRW locks.
#include "pthread.h"

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
  (void)attr;
  InitializeSRWLock(mutex);
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
  (void)mutex;  // an SRW lock has nothing to tear down
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
  AcquireSRWLockExclusive(mutex);
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
  ReleaseSRWLockExclusive(mutex);
  return 0;
}
