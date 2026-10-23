/****************************************************************************
 * net/devif/devif_pollguard.c
 *
 * Debug-only poll-overlap detector, see nuttx/net/netdev_pollguard.h
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include <semaphore.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/mm/mm.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netdev_pollguard.h>

/* Holder bookkeeping only exists with priority inheritance and preallocated
 * holder records (CONFIG_SEM_PREALLOCHOLDERS > 0, as on fmu-v6x).
 */

#if defined(CONFIG_PRIORITY_INHERITANCE) && CONFIG_SEM_PREALLOCHOLDERS > 0
#  define POLLGUARD_HAVE_HOLDERS 1
#endif

#ifdef CONFIG_DEBUG_NET_POLL_OVERLAP

static struct netdev_pollguard_event_s g_events[NETDEV_POLLGUARD_NEVENTS];
static int g_nevents;
static uint32_t g_noverlaps;

static uint8_t pollguard_basepri(void)
{
#if defined(CONFIG_ARCH_ARMV7M)
  return getbasepri();
#else
  return 0;
#endif
}

static void pollguard_name(char *dst, int16_t pid)
{
#if CONFIG_TASK_NAME_SIZE > 0
  FAR struct tcb_s *tcb = nxsched_get_tcb(pid);

  strncpy(dst, tcb != NULL ? tcb->name : "?", NETDEV_POLLGUARD_NAMELEN - 1);
#else
  strncpy(dst, "?", NETDEV_POLLGUARD_NAMELEN - 1);
#endif
  dst[NETDEV_POLLGUARD_NAMELEN - 1] = '\0';
}

/* Current/base priority and the semaphores the task is recorded as holding.
 * Must be called inside a critical section.
 */

static void pollguard_task(FAR struct netdev_pollguard_party_s *p, pid_t pid)
{
  FAR struct tcb_s *tcb = nxsched_get_tcb(pid);

  p->nheld = 0;
  if (tcb == NULL)
    {
      return;
    }

  p->prio_now = tcb->sched_priority;
#ifdef CONFIG_PRIORITY_INHERITANCE
  p->baseprio = tcb->base_priority;
#else
  p->baseprio = tcb->sched_priority;
#endif

#ifdef POLLGUARD_HAVE_HOLDERS
  for (FAR struct semholder_s *h = tcb->holdsem;
       h != NULL && p->nheld < NETDEV_POLLGUARD_NHELD; h = h->tlink)
    {
      p->held[p->nheld]       = h->sem;
      p->heldcounts[p->nheld] = h->counts;
      p->nheld++;
    }
#endif
}

/* Count and holder list of one semaphore. Must be called inside a critical
 * section.
 */

static void pollguard_lock(FAR struct netdev_pollguard_lock_s *l,
                           FAR sem_t *sem)
{
  l->sem      = sem;
  l->semcount = sem->semcount;
  l->nholders = 0;

#ifdef POLLGUARD_HAVE_HOLDERS
  for (FAR struct semholder_s *h = sem->hhead;
       h != NULL && l->nholders < NETDEV_POLLGUARD_NHOLDERS; h = h->flink)
    {
      FAR struct netdev_pollguard_holder_s *o = &l->holders[l->nholders++];

      o->pid      = h->htcb != NULL ? h->htcb->pid : -1;
      o->counts   = h->counts;
      o->prio     = h->htcb != NULL ? h->htcb->sched_priority : 0;
      o->baseprio = h->htcb != NULL ? h->htcb->base_priority : 0;
    }
#endif
}

void netdev_poll_guard_enter(FAR struct net_driver_s *dev,
                             FAR const char *tag)
{
  /* Sample BASEPRI before enter_critical_section() raises it */

  uint8_t basepri = pollguard_basepri();
  FAR struct tcb_s *tcb = nxsched_self();
  irqstate_t flags = enter_critical_section();

  if (dev->d_pollnest != 0 && dev->d_pollowner != tcb->pid)
    {
      g_noverlaps++;

      if (g_nevents < NETDEV_POLLGUARD_NEVENTS)
        {
          FAR struct netdev_pollguard_event_s *ev = &g_events[g_nevents++];

          ev->ticks            = clock_systime_ticks();
          ev->ifname           = dev->d_ifname;
          ev->nest             = dev->d_pollnest;
          ev->irq              = up_interrupt_context();

          ev->owner.tag        = dev->d_pollownertag;
          ev->owner.pid        = dev->d_pollowner;
          ev->owner.prio       = dev->d_pollownerprio;
          ev->owner.basepri    = dev->d_pollownerbasepri;
          pollguard_name(ev->owner.name, dev->d_pollowner);

          ev->intruder.tag     = tag;
          ev->intruder.pid     = tcb->pid;
          ev->intruder.prio    = tcb->sched_priority;
          ev->intruder.basepri = basepri;
          pollguard_name(ev->intruder.name, tcb->pid);

          /* Who could have boosted whom: priorities, held semaphores and
           * the suspect locks' holders, all at the moment of the overlap.
           */

          pollguard_task(&ev->owner, dev->d_pollowner);
          pollguard_task(&ev->intruder, tcb->pid);

          pid_t netlock_owner;
          unsigned int netlock_count;
          FAR sem_t *netlock = net_lock_debug(&netlock_owner,
                                              &netlock_count);

          ev->netlock_owner = netlock_owner;
          ev->netlock_count = netlock_count;
          pollguard_lock(&ev->netlock, netlock);

          /* ponytail: mm_semaphore is the first member of the private
           * struct mm_heap_s (mm/mm_heap/mm.h); re-check if that changes.
           */

          pollguard_lock(&ev->heap, (FAR sem_t *)g_mmheap);
        }
    }

  /* ponytail: ownership stays with the first task until the nest count
   * drops to 0, so if the owner leaves first, a re-entry by the intruder
   * is reported again against the old owner. Fine for a race detector.
   */

  if (dev->d_pollnest++ == 0)
    {
      dev->d_pollowner        = tcb->pid;
      dev->d_pollownerprio    = tcb->sched_priority;
      dev->d_pollownerbasepri = basepri;
      dev->d_pollownertag     = tag;
    }

  leave_critical_section(flags);
}

void netdev_poll_guard_exit(FAR struct net_driver_s *dev)
{
  irqstate_t flags = enter_critical_section();

  ASSERT(dev->d_pollnest > 0);
  dev->d_pollnest--;

  leave_critical_section(flags);
}

uint32_t netdev_poll_guard_get(FAR struct netdev_pollguard_event_s *events,
                               int max, FAR int *nevents)
{
  irqstate_t flags = enter_critical_section();
  int n = g_nevents < max ? g_nevents : max;
  uint32_t total = g_noverlaps;

  if (events != NULL && n > 0)
    {
      memcpy(events, g_events, n * sizeof(*events));
    }

  leave_critical_section(flags);

  if (nevents != NULL)
    {
      *nevents = events != NULL ? n : 0;
    }

  return total;
}

#endif /* CONFIG_DEBUG_NET_POLL_OVERLAP */
