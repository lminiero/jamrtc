/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

#ifndef JAMRTC_REFCOUNT_H
#define JAMRTC_REFCOUNT_H

#include <glib.h>
#include "mutex.h"

//~ #define REFCOUNT_DEBUG

extern int refcount_debug;

#define jamrtc_refcount_containerof(refptr, type, member) \
	((type *)((char *)(refptr) - offsetof(type, member)))


/*! Jamus reference counter structure */
typedef struct jamrtc_refcount jamrtc_refcount;
struct jamrtc_refcount {
	/*! The reference counter itself */
	gint count;
	/*! Pointer to the function that will be used to free the object */
	void (*free)(const jamrtc_refcount *);
};


#ifdef REFCOUNT_DEBUG
/* Reference counters debugging */
extern GHashTable *counters;
extern jamrtc_mutex counters_mutex;
#define jamrtc_refcount_track(refp) { \
	jamrtc_mutex_lock(&counters_mutex); \
	if(counters == NULL) \
		counters = g_hash_table_new(NULL, NULL); \
	g_hash_table_insert(counters, refp, refp); \
	jamrtc_mutex_unlock(&counters_mutex); \
}
#define jamrtc_refcount_untrack(refp) { \
	jamrtc_mutex_lock(&counters_mutex); \
	g_hash_table_remove(counters, refp); \
	jamrtc_mutex_unlock(&counters_mutex); \
}
#endif


/*! Jamus reference counter initialization (debug according to settings) */
#define jamrtc_refcount_init(refp, free_fn) { \
	if(!refcount_debug) { \
		jamrtc_refcount_init_nodebug(refp, free_fn); \
	} else { \
		jamrtc_refcount_init_debug(refp, free_fn); \
	} \
}
/*! Jamus reference counter initialization (no debug) */
#ifdef REFCOUNT_DEBUG
#define jamrtc_refcount_init_nodebug(refp, free_fn) { \
	(refp)->count = 1; \
	(refp)->free = free_fn; \
	jamrtc_refcount_track((refp)); \
}
#else
#define jamrtc_refcount_init_nodebug(refp, free_fn) { \
	(refp)->count = 1; \
	(refp)->free = free_fn; \
}
#endif
/*! Jamus reference counter initialization (debug) */
#ifdef REFCOUNT_DEBUG
#define jamrtc_refcount_init_debug(refp, free_fn) { \
	(refp)->count = 1; \
	JAMRTC_PRINT("[%s:%s:%d:init] %p (%d)\n", __FILE__, __FUNCTION__, __LINE__, refp, (refp)->count); \
	(refp)->free = free_fn; \
	jamrtc_refcount_track((refp)); \
}
#else
#define jamrtc_refcount_init_debug(refp, free_fn) { \
	(refp)->count = 1; \
	JAMRTC_PRINT("[%s:%s:%d:init] %p (%d)\n", __FILE__, __FUNCTION__, __LINE__, refp, (refp)->count); \
	(refp)->free = free_fn; \
}
#endif

/*! Increase the Jamus reference counter (debug according to settings) */
#define jamrtc_refcount_increase(refp) { \
	if(!refcount_debug) { \
		jamrtc_refcount_increase_nodebug(refp); \
	} else { \
		jamrtc_refcount_increase_debug(refp); \
	} \
}
/*! Increase the Jamus reference counter (no debug) */
#define jamrtc_refcount_increase_nodebug(refp)  { \
	g_atomic_int_inc((gint *)&(refp)->count); \
}
/*! Increase the Jamus reference counter (debug) */
#define jamrtc_refcount_increase_debug(refp)  { \
	JAMRTC_PRINT("[%s:%s:%d:increase] %p (%d)\n", __FILE__, __FUNCTION__, __LINE__, refp, (refp)->count+1); \
	g_atomic_int_inc((gint *)&(refp)->count); \
}

/*! Decrease the Jamus reference counter (debug according to settings) */
#define jamrtc_refcount_decrease(refp) { \
	if(!refcount_debug) { \
		jamrtc_refcount_decrease_nodebug(refp); \
	} else { \
		jamrtc_refcount_decrease_debug(refp); \
	} \
}
/*! Decrease the Jamus reference counter (debug) */
#ifdef REFCOUNT_DEBUG
#define jamrtc_refcount_decrease_debug(refp)  { \
	JAMRTC_PRINT("[%s:%s:%d:decrease] %p (%d)\n", __FILE__, __FUNCTION__, __LINE__, refp, (refp)->count-1); \
	if(g_atomic_int_dec_and_test((gint *)&(refp)->count)) { \
		(refp)->free(refp); \
		jamrtc_refcount_untrack((refp)); \
	} \
}
#else
#define jamrtc_refcount_decrease_debug(refp)  { \
	JAMRTC_PRINT("[%s:%s:%d:decrease] %p (%d)\n", __FILE__, __FUNCTION__, __LINE__, refp, (refp)->count-1); \
	if(g_atomic_int_dec_and_test((gint *)&(refp)->count)) { \
		(refp)->free(refp); \
	} \
}
#endif
/*! Decrease the Jamus reference counter (no debug) */
#ifdef REFCOUNT_DEBUG
#define jamrtc_refcount_decrease_nodebug(refp)  { \
	if(g_atomic_int_dec_and_test((gint *)&(refp)->count)) { \
		(refp)->free(refp); \
		jamrtc_refcount_untrack((refp)); \
	} \
}
#else
#define jamrtc_refcount_decrease_nodebug(refp)  { \
	if(g_atomic_int_dec_and_test((gint *)&(refp)->count)) { \
		(refp)->free(refp); \
	} \
}
#endif

#endif
