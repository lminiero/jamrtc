/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

#ifndef JAMRTC_MUTEX_H
#define JAMRTC_MUTEX_H

#include <pthread.h>
#include <errno.h>

#include "debug.h"

extern int lock_debug;

#ifdef USE_PTHREAD_MUTEX

/*! Jamus mutex implementation */
typedef pthread_mutex_t jamrtc_mutex;
/*! Jamus mutex initialization */
#define jamrtc_mutex_init(a) pthread_mutex_init(a,NULL)
/*! Jamus static mutex initializer */
#define JAMRTC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
/*! Jamus mutex destruction */
#define jamrtc_mutex_destroy(a) pthread_mutex_destroy(a)
/*! Jamus mutex lock without debug */
#define jamrtc_mutex_lock_nodebug(a) pthread_mutex_lock(a);
/*! Jamus mutex lock with debug (prints the line that locked a mutex) */
#define jamrtc_mutex_lock_debug(a) { JAMRTC_PRINT("[%s:%s:%d:lock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); pthread_mutex_lock(a); };
/*! Jamus mutex lock wrapper (selective locking debug) */
#define jamrtc_mutex_lock(a) { if(!lock_debug) { jamrtc_mutex_lock_nodebug(a); } else { jamrtc_mutex_lock_debug(a); } };
/*! Jamus mutex unlock without debug */
#define jamrtc_mutex_unlock_nodebug(a) pthread_mutex_unlock(a);
/*! Jamus mutex unlock with debug (prints the line that unlocked a mutex) */
#define jamrtc_mutex_unlock_debug(a) { JAMRTC_PRINT("[%s:%s:%d:unlock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); pthread_mutex_unlock(a); };
/*! Jamus mutex unlock wrapper (selective locking debug) */
#define jamrtc_mutex_unlock(a) { if(!lock_debug) { jamrtc_mutex_unlock_nodebug(a); } else { jamrtc_mutex_unlock_debug(a); } };

/*! Jamus condition implementation */
typedef pthread_cond_t jamrtc_condition;
/*! Jamus condition initialization */
#define jamrtc_condition_init(a) pthread_cond_init(a,NULL)
/*! Jamus condition destruction */
#define jamrtc_condition_destroy(a) pthread_cond_destroy(a)
/*! Jamus condition wait */
#define jamrtc_condition_wait(a, b) pthread_cond_wait(a, b);
/*! Jamus condition timed wait */
#define jamrtc_condition_timedwait(a, b, c) pthread_cond_timedwait(a, b, c);
/*! Jamus condition signal */
#define jamrtc_condition_signal(a) pthread_cond_signal(a);
/*! Jamus condition broadcast */
#define jamrtc_condition_broadcast(a) pthread_cond_broadcast(a);

#else

/*! Jamus mutex implementation */
typedef GMutex jamrtc_mutex;
/*! Jamus mutex initialization */
#define jamrtc_mutex_init(a) g_mutex_init(a)
/*! Jamus static mutex initializer */
#define JAMRTC_MUTEX_INITIALIZER {0}
/*! Jamus mutex destruction */
#define jamrtc_mutex_destroy(a) g_mutex_clear(a)
/*! Jamus mutex lock without debug */
#define jamrtc_mutex_lock_nodebug(a) g_mutex_lock(a);
/*! Jamus mutex lock with debug (prints the line that locked a mutex) */
#define jamrtc_mutex_lock_debug(a) { JAMRTC_PRINT("[%s:%s:%d:lock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); g_mutex_lock(a); };
/*! Jamus mutex lock wrapper (selective locking debug) */
#define jamrtc_mutex_lock(a) { if(!lock_debug) { jamrtc_mutex_lock_nodebug(a); } else { jamrtc_mutex_lock_debug(a); } };
/*! Jamus mutex unlock without debug */
#define jamrtc_mutex_unlock_nodebug(a) g_mutex_unlock(a);
/*! Jamus mutex unlock with debug (prints the line that unlocked a mutex) */
#define jamrtc_mutex_unlock_debug(a) { JAMRTC_PRINT("[%s:%s:%d:unlock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); g_mutex_unlock(a); };
/*! Jamus mutex unlock wrapper (selective locking debug) */
#define jamrtc_mutex_unlock(a) { if(!lock_debug) { jamrtc_mutex_unlock_nodebug(a); } else { jamrtc_mutex_unlock_debug(a); } };

/*! Jamus condition implementation */
typedef GCond jamrtc_condition;
/*! Jamus condition initialization */
#define jamrtc_condition_init(a) g_cond_init(a)
/*! Jamus condition destruction */
#define jamrtc_condition_destroy(a) g_cond_clear(a)
/*! Jamus condition wait */
#define jamrtc_condition_wait(a, b) g_cond_wait(a, b);
/*! Jamus condition wait until */
#define jamrtc_condition_wait_until(a, b, c) g_cond_wait_until(a, b, c);
/*! Jamus condition signal */
#define jamrtc_condition_signal(a) g_cond_signal(a);
/*! Jamus condition broadcast */
#define jamrtc_condition_broadcast(a) g_cond_broadcast(a);

#endif

#endif
