// SPDX-License-Identifier: GPL-2.0-only
/*
 * DiamaneOS: KUnit test of the hold of new cards, built into sd_hold.c.
 *
 * Each test registers a host whose controller fails every command and counts
 * them. A held removable host must get no command and no prescan power-up, a
 * non-removable one is never held, and clearing the hold starts a card that
 * waits. A card powered off while held is not resumed after a card detect
 * event.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/mmc/core.h>
#include <linux/string.h>

struct hold_test_slot {
	atomic_t commands;
	atomic_t power_ups;
	int card_present;
};

static void hold_test_request(struct mmc_host *host, struct mmc_request *mrq)
{
	struct hold_test_slot *slot = mmc_priv(host);

	atomic_inc(&slot->commands);
	mrq->cmd->error = -ETIMEDOUT;
	mmc_request_done(host, mrq);
}

static void hold_test_set_ios(struct mmc_host *host, struct mmc_ios *ios)
{
	struct hold_test_slot *slot = mmc_priv(host);

	if (ios->power_mode == MMC_POWER_UP)
		atomic_inc(&slot->power_ups);
}

static int hold_test_get_cd(struct mmc_host *host)
{
	struct hold_test_slot *slot = mmc_priv(host);

	return READ_ONCE(slot->card_present);
}

static const struct mmc_host_ops hold_test_ops = {
	.request	= hold_test_request,
	.set_ios	= hold_test_set_ios,
	.get_cd		= hold_test_get_cd,
};

KUNIT_DEFINE_ACTION_WRAPPER(hold_test_free_host, mmc_free_host,
			    struct mmc_host *);
KUNIT_DEFINE_ACTION_WRAPPER(hold_test_remove_host, mmc_remove_host,
			    struct mmc_host *);

static void hold_test_reset(void *unused)
{
	hold_set(1);
}

static int hold_test_commands(struct mmc_host *host)
{
	struct hold_test_slot *slot = mmc_priv(host);

	flush_delayed_work(&host->detect);
	return atomic_read(&slot->commands);
}

/* A registered host with a card in its slot, after its first scan. */
static struct mmc_host *hold_test_host(struct kunit *test, bool removable)
{
	struct hold_test_slot *slot;
	struct mmc_host *host;
	struct device *dev;

	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, hold_test_reset, NULL));
	dev = kunit_device_register(test, "mmc_hold_test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	host = mmc_alloc_host(sizeof(*slot), dev);
	KUNIT_ASSERT_NOT_NULL(test, host);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test,
			hold_test_free_host, host));
	slot = mmc_priv(host);
	atomic_set(&slot->commands, 0);
	atomic_set(&slot->power_ups, 0);
	slot->card_present = 1;
	host->ops = &hold_test_ops;
	host->f_min = 400000;
	host->f_max = 400000;
	host->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	if (!removable)
		host->caps |= MMC_CAP_NONREMOVABLE;

	KUNIT_ASSERT_EQ(test, 0, mmc_add_host(host));
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test,
			hold_test_remove_host, host));
	hold_test_commands(host);
	return host;
}

static void hold_test_held_card_gets_no_command(struct kunit *test)
{
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, true);
	KUNIT_EXPECT_EQ(test, 0, hold_test_commands(host));
	KUNIT_EXPECT_NULL(test, host->card);

	/* A card event while held, as a card detect interrupt makes. */
	mmc_detect_change(host, 0);
	KUNIT_EXPECT_EQ(test, 0, hold_test_commands(host));

	hold_set(0);
	KUNIT_EXPECT_GT(test, hold_test_commands(host), 0);
}

static void hold_test_non_removable_never_held(struct kunit *test)
{
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, false);
	KUNIT_EXPECT_GT(test, hold_test_commands(host), 0);
	/* Its prescan power-up as on stock. */
	KUNIT_EXPECT_GT(test, atomic_read(&((struct hold_test_slot *)mmc_priv(host))->power_ups), 0);
}

static void hold_test_card_taken_out_while_held(struct kunit *test)
{
	struct hold_test_slot *slot;
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, true);
	slot = mmc_priv(host);
	KUNIT_EXPECT_EQ(test, 0, hold_test_commands(host));

	WRITE_ONCE(slot->card_present, 0);
	mmc_detect_change(host, 0);
	hold_set(0);
	KUNIT_EXPECT_EQ(test, 0, hold_test_commands(host));
}

static void hold_test_set_again_holds_next_card(struct kunit *test)
{
	struct mmc_host *host;
	int seen;

	hold_set(0);
	host = hold_test_host(test, true);
	seen = hold_test_commands(host);
	KUNIT_EXPECT_GT(test, seen, 0);

	hold_set(1);
	mmc_detect_change(host, 0);
	KUNIT_EXPECT_EQ(test, seen, hold_test_commands(host));
}

static void hold_test_held_host_not_powered_at_start(struct kunit *test)
{
	struct hold_test_slot *slot;
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, true);
	slot = mmc_priv(host);
	KUNIT_EXPECT_EQ(test, 0, atomic_read(&slot->power_ups));
}

static void hold_test_card_event_while_off(struct kunit *test)
{
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, true);

	/* A card that stayed: no card detect event while it was off. */
	mmc_sd_hold_card_off(host);
	KUNIT_EXPECT_FALSE(test, mmc_sd_hold_card_moved(host));

	/* A card detect event while it was off: it may have been swapped. */
	mmc_detect_change(host, 0);
	KUNIT_EXPECT_TRUE(test, mmc_sd_hold_card_moved(host));
	/* Rescans of the host itself are no card detect event. */
	mmc_sd_hold_card_off(host);
	_mmc_detect_change(host, 0, false);
	KUNIT_EXPECT_FALSE(test, mmc_sd_hold_card_moved(host));

	/* Unlocked, a card resumes as on stock (its CID is compared). */
	mmc_detect_change(host, 0);
	hold_set(0);
	KUNIT_EXPECT_FALSE(test, mmc_sd_hold_card_moved(host));
	hold_test_commands(host);
}

static int hold_test_write(const char *text)
{
	char buf[8];
	size_t len = strlen(text);
	loff_t pos = 0;

	strscpy(buf, text, sizeof(buf));
	return hold_new_sd_handler(&hold_table[0], 1, buf, &len, &pos);
}

static void hold_test_sysctl(struct kunit *test)
{
	struct mmc_host *host;

	hold_set(1);
	host = hold_test_host(test, true);

	KUNIT_EXPECT_EQ(test, -EINVAL, hold_test_write("2\n"));
	KUNIT_EXPECT_EQ(test, -EINVAL, hold_test_write("-1\n"));
	KUNIT_EXPECT_EQ(test, 1, READ_ONCE(hold_new_sd));
	KUNIT_EXPECT_EQ(test, 0, hold_test_commands(host));

	KUNIT_EXPECT_EQ(test, 0, hold_test_write("0\n"));
	KUNIT_EXPECT_EQ(test, 0, READ_ONCE(hold_new_sd));
	KUNIT_EXPECT_GT(test, hold_test_commands(host), 0);
}

static struct kunit_case hold_test_cases[] = {
	KUNIT_CASE(hold_test_held_card_gets_no_command),
	KUNIT_CASE(hold_test_non_removable_never_held),
	KUNIT_CASE(hold_test_card_taken_out_while_held),
	KUNIT_CASE(hold_test_set_again_holds_next_card),
	KUNIT_CASE(hold_test_sysctl),
	KUNIT_CASE(hold_test_held_host_not_powered_at_start),
	KUNIT_CASE(hold_test_card_event_while_off),
	{}
};

static struct kunit_suite hold_test_suite = {
	.name = "mmc_hold_new_cards",
	.test_cases = hold_test_cases,
};
kunit_test_suite(hold_test_suite);
