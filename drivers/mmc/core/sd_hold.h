/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DiamaneOS: new cards on removable hosts wait while kernel.hold_new_sd is set
 * (sd_hold.c).
 */
#ifndef _MMC_CORE_SD_HOLD_H
#define _MMC_CORE_SD_HOLD_H

#include <linux/device.h>
#include <linux/types.h>

struct mmc_host;

/* host.c: calls fn for every registered host. */
int mmc_for_each_host(int (*fn)(struct device *dev, void *data), void *data);

#ifdef CONFIG_MMC_HOLD_NEW_CARDS
bool mmc_sd_hold_new_card(struct mmc_host *host);
void mmc_sd_hold_sync(void);
void mmc_sd_hold_cd_event(struct mmc_host *host);
void mmc_sd_hold_card_off(struct mmc_host *host);
bool mmc_sd_hold_card_moved(struct mmc_host *host);
bool mmc_sd_hold_prescan(struct mmc_host *host);
#else
static inline bool mmc_sd_hold_new_card(struct mmc_host *host)
{
	return false;
}

static inline void mmc_sd_hold_sync(void)
{
}

static inline void mmc_sd_hold_cd_event(struct mmc_host *host)
{
}

static inline void mmc_sd_hold_card_off(struct mmc_host *host)
{
}

static inline bool mmc_sd_hold_card_moved(struct mmc_host *host)
{
	return false;
}

static inline bool mmc_sd_hold_prescan(struct mmc_host *host)
{
	return false;
}
#endif

#endif
