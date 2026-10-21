/****************************************************************************
 * net/devif/devif_pollguard.c
 *
 * Debug-only poll-overlap detector, see nuttx/net/netdev_pollguard.h
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netdev_pollguard.h>

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
