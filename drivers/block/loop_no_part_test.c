// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 The DiamaneOS Project
 *
 * KUnit test, built into loop.c: with CONFIG_BLK_NO_EXTERNAL_PART_SCAN a loop
 * device gets no partition, not even one a privileged caller adds with BLKPG.
 */

#include <kunit/test.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/moduleparam.h>

static long loop_test_blkpg(struct kunit *test, struct file *file, unsigned long uaddr,
			    int op)
{
	struct blkpg_partition part = { .pno = 1, .start = 0, .length = SZ_512K };
	struct blkpg_ioctl_arg arg = {
		.op = op,
		.datalen = sizeof(part),
		.data = (void __user *)(uaddr + sizeof(arg)),
	};

	KUNIT_ASSERT_EQ(test, 0, copy_to_user((void __user *)uaddr, &arg, sizeof(arg)));
	KUNIT_ASSERT_EQ(test, 0, copy_to_user(arg.data, &part, sizeof(part)));
	return file->f_op->unlocked_ioctl(file, BLKPG, uaddr);
}

static void loop_test_blkpg_add_refused(struct kunit *test)
{
	struct loop_device *lo;
	unsigned long uaddr;
	struct file *file;
	int idx;
	long ret;

	idx = loop_add(-1);
	KUNIT_ASSERT_GE(test, idx, 0);
	mutex_lock(&loop_ctl_mutex);
	lo = idr_find(&loop_index_idr, idx);
	mutex_unlock(&loop_ctl_mutex);
	KUNIT_ASSERT_NOT_NULL(test, lo);
	KUNIT_EXPECT_TRUE(test, lo->lo_disk->flags & GENHD_FL_NO_PART);

	/* The size a 1 MiB backing file gives the device. */
	set_capacity(lo->lo_disk, SZ_1M >> SECTOR_SHIFT);
	file = bdev_file_open_by_dev(disk_devt(lo->lo_disk), BLK_OPEN_READ, NULL, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	uaddr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_FALSE(test, !uaddr || IS_ERR_VALUE(uaddr));

	ret = loop_test_blkpg(test, file, uaddr, BLKPG_ADD_PARTITION);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_NULL(test, xa_load(&lo->lo_disk->part_tbl, 1));
	if (!ret)
		loop_test_blkpg(test, file, uaddr, BLKPG_DEL_PARTITION);

	fput(file);
	flush_delayed_fput();
	set_capacity(lo->lo_disk, 0);
	KUNIT_EXPECT_EQ(test, 0, loop_control_remove(idx));
}

/* The real built-in parameter table must have no way to restore the scan. */
static int external_scan_unknown(char *param, char *val, const char *doing, void *arg)
{
	unsigned int *unknown = arg;

	if (parameq(param, "external_part_scan"))
		(*unknown)++;
	return 0;
}

static void external_scan_override_is_unknown(struct kunit *test)
{
	static const char * const forms[] = {
		"external_part_scan=1", "external-part-scan=1", "external_part_scan",
	};
	struct gendisk disk = { .flags = GENHD_FL_USER_PART };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(forms); i++) {
		unsigned int unknown = 0;
		char *args = kunit_kstrdup(test, forms[i], GFP_KERNEL);
		char reset[] = "external_part_scan=0";

		KUNIT_ASSERT_NOT_NULL(test, args);
		KUNIT_EXPECT_PTR_EQ(test, NULL,
			parse_args("external scan test", args, __start___param,
				   __stop___param - __start___param, -32768, 32767,
				   &unknown, external_scan_unknown));
		KUNIT_EXPECT_EQ_MSG(test, unknown, 1U, "%s was a registered parameter", forms[i]);
		KUNIT_EXPECT_FALSE(test, disk_has_partscan(&disk));
		/* Leave an unfixed baseline's global parameter at its original value. */
		parse_args("external scan reset", reset, __start___param,
			   __stop___param - __start___param, -32768, 32767,
			   &unknown, external_scan_unknown);
	}
}

static struct kunit_case loop_no_part_test_cases[] = {
	KUNIT_CASE(loop_test_blkpg_add_refused),
	KUNIT_CASE(external_scan_override_is_unknown),
	{}
};

static struct kunit_suite loop_no_part_test_suite = {
	.name = "loop_no_partitions",
	.test_cases = loop_no_part_test_cases,
};
kunit_test_suite(loop_no_part_test_suite);
