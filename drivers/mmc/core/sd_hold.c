// SPDX-License-Identifier: GPL-2.0-only
/*
 * DiamaneOS: hold of new cards on removable hosts.
 *
 * The MMC core reads and parses a new card's registers (CID, CSD, SCR, SD
 * status, switch data) and its replies in the kernel. While kernel.hold_new_sd
 * is 1, mmc_rescan() leaves a new card on a removable host unpowered and sends
 * it no command: only the card detect state is read. A card that already works
 * keeps working. Clearing the hold rescans the removable hosts that have no
 * card, so a held card starts as if just inserted. Hosts with
 * MMC_CAP_NONREMOVABLE (eMMC, soldered SDIO chips) are never held.
 *
 * A card that was already working when the hold was set keeps working, also
 * across sleep: its power-off and resume keep it. But a card detect event while
 * it was powered off means it may have been swapped: while held, it is not
 * powered again and gets no command, it is dropped as removed, and whatever card
 * is in the slot now waits like any new card. The prescan power-up of a
 * removable host at its start is skipped while held.
 *
 * The hold is set at boot and stays set until userspace clears it. On Android,
 * the system server sets security.hold_new_sd from the lock screen state and
 * init writes it here.
 */

#define pr_fmt(fmt) "mmc: " fmt

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mmc/host.h>
#include <linux/mutex.h>
#include <linux/ratelimit.h>
#include <linux/sysctl.h>

#include "core.h"
#include "sd_hold.h"

static int hold_new_sd = 1;

/*
 * Every resume rescans a host, so a held card would log at each suspend cycle:
 * one line a minute at most.
 */
static struct ratelimit_state hold_log =
	RATELIMIT_STATE_INIT_FLAGS(hold_log, 60 * HZ, 1, RATELIMIT_MSG_ON_RELEASE);

/* Serialises changes of the hold with __mmc_stop_host() (mmc_sd_hold_sync). */
static DEFINE_MUTEX(hold_lock);

/*
 * Per host (by its index): the card detect events seen, and their count when
 * the host's card was powered off. Hosts above the table are never held across
 * a power-off: their card is treated as moved.
 */
#define HOLD_HOSTS 16
static atomic_t cd_events[HOLD_HOSTS];
static int cd_events_at_off[HOLD_HOSTS];

static bool held_removable(struct mmc_host *host)
{
	return mmc_card_is_removable(host) && READ_ONCE(hold_new_sd);
}

/* _mmc_detect_change() for a card detect event (cd_irq). */
void mmc_sd_hold_cd_event(struct mmc_host *host)
{
	if (host->index >= 0 && host->index < HOLD_HOSTS)
		atomic_inc(&cd_events[host->index]);
}

/* _mmc_sd_suspend() after it powered the card off. Under the host's claim. */
void mmc_sd_hold_card_off(struct mmc_host *host)
{
	if (host->index >= 0 && host->index < HOLD_HOSTS)
		cd_events_at_off[host->index] = atomic_read(&cd_events[host->index]);
}

/*
 * _mmc_sd_resume() before it powers the card on, under the host's claim: true
 * when the card must not be resumed, because the hold is set and a card detect
 * event came while it was off. A card detect interrupt that came in sleep runs
 * first, so its event is counted.
 */
bool mmc_sd_hold_card_moved(struct mmc_host *host)
{
	if (!held_removable(host))
		return false;
	if (host->slot.cd_irq >= 0)
		synchronize_irq(host->slot.cd_irq);
	if (host->index < 0 || host->index >= HOLD_HOSTS)
		return true;
	if (atomic_read(&cd_events[host->index]) == cd_events_at_off[host->index])
		return false;
	if (__ratelimit(&hold_log))
		pr_info("%s: card may have been swapped while off, dropped (kernel.hold_new_sd)\n",
			mmc_hostname(host));
	return true;
}

/* mmc_start_host(): no prescan power-up of a removable host while held. */
bool mmc_sd_hold_prescan(struct mmc_host *host)
{
	return held_removable(host);
}

/*
 * Called by mmc_rescan() for a host without a card, once the card detect state
 * says a card may be present and before any command.
 */
bool mmc_sd_hold_new_card(struct mmc_host *host)
{
	if (!mmc_card_is_removable(host) || !READ_ONCE(hold_new_sd))
		return false;

	if (__ratelimit(&hold_log))
		pr_info("%s: new card held (kernel.hold_new_sd)\n",
			mmc_hostname(host));
	return true;
}

/*
 * Called by __mmc_stop_host() after it set rescan_disable: a release running
 * now has either seen rescan_disable and skipped the host, or queued its
 * detect work before the cancel that follows.
 */
void mmc_sd_hold_sync(void)
{
	mutex_lock(&hold_lock);
	mutex_unlock(&hold_lock);
}

static int start_held_card(struct device *dev, void *data)
{
	struct mmc_host *host = container_of(dev, struct mmc_host, class_dev);

	if (mmc_card_is_removable(host) && !READ_ONCE(host->rescan_disable) &&
	    !READ_ONCE(host->bus_ops))
		_mmc_detect_change(host, 0, false);
	return 0;
}

static void hold_set(int hold)
{
	mutex_lock(&hold_lock);
	if (hold_new_sd && !hold) {
		WRITE_ONCE(hold_new_sd, 0);
		mmc_for_each_host(start_held_card, NULL);
	} else {
		WRITE_ONCE(hold_new_sd, hold);
	}
	mutex_unlock(&hold_lock);
}

static int hold_new_sd_handler(const struct ctl_table *table, int write,
			       void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t = *table;
	int val = READ_ONCE(hold_new_sd);
	int ret;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t.data = &val;
	ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (!ret && write)
		hold_set(val);
	return ret;
}

static const struct ctl_table hold_table[] = {
	{
		.procname	= "hold_new_sd",
		.data		= &hold_new_sd,
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= hold_new_sd_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static int __init mmc_sd_hold_init(void)
{
	register_sysctl_init("kernel", hold_table);
	return 0;
}
late_initcall(mmc_sd_hold_init);

#if IS_ENABLED(CONFIG_MMC_HOLD_NEW_CARDS_KUNIT_TEST)
#include "sd_hold_test.c"
#endif
