/****************************************************************************
 * include/nuttx/net/netdev_pollguard.h
 *
 * Debug-only poll-overlap detector (CONFIG_DEBUG_NET_POLL_OVERLAP).
 *
 * Wrap every section that uses the shared per-device buffer state
 * (d_buf, d_appdata, d_len, d_sndlen) with netdev_poll_guard_enter() /
 * netdev_poll_guard_exit(). If a different task enters a guarded section
 * of the same device while another one is still inside, the overlap is
 * counted and the first NETDEV_POLLGUARD_NEVENTS are recorded. Read them
 * from a normal task with netdev_poll_guard_get().
 *
 * Nesting by the same task is allowed (only counted).
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_NET_NETDEV_POLLGUARD_H
#define __INCLUDE_NUTTX_NET_NETDEV_POLLGUARD_H

#include <nuttx/config.h>

#include <sys/types.h>
#include <assert.h>
#include <semaphore.h>
#include <stdint.h>

/* Network-stack assertions, independent of CONFIG_DEBUG_ASSERTIONS */

#ifdef CONFIG_DEBUG_NET_ASSERTIONS
#  define NET_DEBUGASSERT(f) ASSERT(f)
#else
#  define NET_DEBUGASSERT(f) ((void)(1 || (f)))
#endif

#ifdef CONFIG_DEBUG_NET_POLL_OVERLAP

#define NETDEV_POLLGUARD_NEVENTS  8  /* First N overlaps are kept */
#define NETDEV_POLLGUARD_NAMELEN  16
#define NETDEV_POLLGUARD_NHOLDERS 4  /* Holders kept per suspect lock */
#define NETDEV_POLLGUARD_NHELD    4  /* Held semaphores kept per task */

struct net_driver_s;

/* One semaphore holder record (from the priority-inheritance bookkeeping) */

struct netdev_pollguard_holder_s
{
  int16_t pid;
  int16_t counts;                        /* Counts this task holds */
  uint8_t prio;                          /* Current priority */
  uint8_t baseprio;                      /* Priority without inheritance */
};

/* Snapshot of one suspect lock at overlap time */

struct netdev_pollguard_lock_s
{
  const void *sem;                       /* Semaphore address */
  int16_t semcount;                      /* < 0: number of waiters */
  uint8_t nholders;
  struct netdev_pollguard_holder_s holders[NETDEV_POLLGUARD_NHOLDERS];
};

/* One task inside a guarded section */

struct netdev_pollguard_party_s
{
  const char *tag;                       /* Section, e.g. "devif_poll" */
  char name[NETDEV_POLLGUARD_NAMELEN];   /* Task name */
  int16_t pid;
  uint8_t prio;                          /* Priority when it entered */
  uint8_t basepri;                       /* BASEPRI when it entered */

  /* Taken at overlap time */

  uint8_t prio_now;                      /* Current priority */
  uint8_t baseprio;                      /* Priority without inheritance */
  uint8_t nheld;
  const void *held[NETDEV_POLLGUARD_NHELD];       /* Semaphores it holds */
  int16_t heldcounts[NETDEV_POLLGUARD_NHELD];
};

/* One detected overlap */

struct netdev_pollguard_event_s
{
  uint32_t ticks;                        /* clock_systime_ticks() */
  const char *ifname;
  uint8_t nest;                          /* Nesting depth before intruder */
  uint8_t irq;                           /* Intruder in interrupt context */
  struct netdev_pollguard_party_s owner;
  struct netdev_pollguard_party_s intruder;

  /* Suspect locks for the priority boost */

  int16_t netlock_owner;                 /* net_lock()'s own owner pid */
  uint16_t netlock_count;                /* ...and its recursion count */
  struct netdev_pollguard_lock_s netlock;
  struct netdev_pollguard_lock_s heap;   /* Kernel/user heap semaphore */
};

#ifdef __cplusplus
extern "C"
{
#endif

void netdev_poll_guard_enter(struct net_driver_s *dev, const char *tag);
void netdev_poll_guard_exit(struct net_driver_s *dev);

/* Copy up to max recorded events into events (may be NULL), store how
 * many were copied in *nevents (may be NULL) and return the total number
 * of overlaps seen since boot.
 */

uint32_t netdev_poll_guard_get(struct netdev_pollguard_event_s *events,
                               int max, int *nevents);

/* Debug accessor in net/utils/net_lock.c */

sem_t *net_lock_debug(pid_t *holder, unsigned int *count);

#ifdef __cplusplus
}
#endif

#else /* CONFIG_DEBUG_NET_POLL_OVERLAP */

#  define netdev_poll_guard_enter(dev, tag)
#  define netdev_poll_guard_exit(dev)

#endif /* CONFIG_DEBUG_NET_POLL_OVERLAP */
#endif /* __INCLUDE_NUTTX_NET_NETDEV_POLLGUARD_H */
