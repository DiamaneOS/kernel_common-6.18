// SPDX-License-Identifier: GPL-2.0-only
/* Regression tests for CPU progress during proxy-execution mutex waits. */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <trace/hooks/dtask.h>

#include "mutex.h"

struct proxy_mutex_test {
	struct mutex lock;
	struct completion entered;
	struct completion done;
	atomic_t pulses;
	atomic_t self_spins;
	bool recursive;
	bool killable;
	bool suppress_spin;
	bool owner_confirmed;
	int result;
};

static void proxy_mutex_can_spin(void *data, struct mutex *lock, int *retval)
{
	struct proxy_mutex_test *ctx = data;

	if (lock == &ctx->lock && ctx->suppress_spin)
		*retval = 0;
}

static void proxy_mutex_spin_start(void *data, struct mutex *lock,
				   bool *time_out, int *cnt)
{
	struct proxy_mutex_test *ctx = data;

	if (lock != &ctx->lock)
		return;
	if (__mutex_owner(lock) == current)
		atomic_inc(&ctx->self_spins);
	/* Waiter spinning bypasses mutex_can_spin_on_owner(). */
	if (ctx->suppress_spin)
		*time_out = true;
}

static int proxy_mutex_heartbeat(void *data)
{
	struct proxy_mutex_test *ctx = data;

	while (!kthread_should_stop()) {
		atomic_inc(&ctx->pulses);
		msleep(20);
	}
	return 0;
}

static int proxy_mutex_waiter(void *data)
{
	struct proxy_mutex_test *ctx = data;

	allow_signal(ctx->killable ? SIGKILL : SIGUSR1);
	if (ctx->recursive) {
		mutex_lock(&ctx->lock);
		ctx->owner_confirmed = __mutex_owner(&ctx->lock) == current;
	}
	complete(&ctx->entered);
	if (ctx->killable)
		ctx->result = mutex_lock_killable(&ctx->lock);
	else
		ctx->result = mutex_lock_interruptible(&ctx->lock);
	/*
	 * Recursive locking is invalid usage. A wakeup can make the existing
	 * handoff path return success; release the original ownership only
	 * once in either case so the regression can recover on old kernels.
	 */
	if (!ctx->result || ctx->recursive)
		mutex_unlock(&ctx->lock);
	flush_signals(current);
	complete(&ctx->done);
	return 0;
}

static struct task_struct *proxy_mutex_start(int (*fn)(void *),
					    struct proxy_mutex_test *ctx,
					    int cpu, bool realtime,
					    const char *name)
{
	struct task_struct *task = kthread_create(fn, ctx, "%s", name);

	if (IS_ERR(task))
		return task;
	/* Keep the task valid even if it exits before kthread_stop(). */
	get_task_struct(task);
	kthread_bind(task, cpu);
	if (realtime)
		sched_set_fifo(task);
	wake_up_process(task);
	return task;
}

static void proxy_mutex_stop(struct task_struct *task)
{
	kthread_stop(task);
	put_task_struct(task);
}

static bool proxy_mutex_wake_handoff(struct proxy_mutex_test *ctx,
				     struct task_struct *waiter)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	unsigned int state = ctx->killable ? TASK_KILLABLE : TASK_INTERRUPTIBLE;
	unsigned long flags;
	bool ready;

	do {
		raw_spin_lock_irqsave(&ctx->lock.wait_lock, flags);
		ready = __mutex_owner(&ctx->lock) == waiter &&
			!list_empty(&ctx->lock.wait_list) &&
			READ_ONCE(waiter->__state) == state &&
			!READ_ONCE(waiter->on_cpu);
		if (ready) {
			/*
			 * Model a preceding waiter requesting handoff and then
			 * cancelling. Removal leaves HANDOFF set while this
			 * recursive waiter remains on the nonempty wait list.
			 */
			atomic_long_or(MUTEX_FLAG_HANDOFF, &ctx->lock.owner);
		}
		raw_spin_unlock_irqrestore(&ctx->lock.wait_lock, flags);
		if (ready)
			return wake_up_process(waiter);
		msleep(20);
	} while (time_before(jiffies, timeout));

	return false;
}

static void proxy_mutex_run(struct kunit *test, bool recursive, bool killable,
			    bool cancel, bool wake_handoff)
{
	struct task_struct *heartbeat = NULL, *waiter = NULL;
	struct proxy_mutex_test *ctx;
	cpumask_var_t saved;
	unsigned long timeout;
	int worker_cpu, control_cpu, before = 0, after = 0;
	bool owner_locked = false, waiter_done = false, measured = false;
	bool spin_probe = false, can_spin_probe = false;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, test->param_value);
	KUNIT_ASSERT_TRUE_MSG(test, IS_ENABLED(CONFIG_MUTEX_SPIN_ON_OWNER),
			      "Normal-spin coverage requires MUTEX_SPIN_ON_OWNER");
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	if (!alloc_cpumask_var(&saved, GFP_KERNEL)) {
		kfree(ctx);
		KUNIT_FAIL(test, "Cannot save CPU affinity");
		return;
	}
	mutex_init(&ctx->lock);
	init_completion(&ctx->entered);
	init_completion(&ctx->done);
	atomic_set(&ctx->pulses, 0);
	atomic_set(&ctx->self_spins, 0);
	ctx->recursive = recursive;
	ctx->killable = killable;
	ctx->suppress_spin = *(const bool *)test->param_value;
	ctx->result = -EINPROGRESS;

	cpus_read_lock();
	cpumask_copy(saved, current->cpus_ptr);
	worker_cpu = cpumask_first_and(saved, cpu_online_mask);
	control_cpu = cpumask_next_and(worker_cpu, saved, cpu_online_mask);
	if (control_cpu >= nr_cpu_ids) {
		KUNIT_FAIL(test, "Two allowed online CPUs are required; use QEMU -smp 2");
		goto out_affinity;
	}
	ret = set_cpus_allowed_ptr(current, cpumask_of(control_cpu));
	if (ret) {
		KUNIT_FAIL(test, "Cannot bind test controller: %d", ret);
		goto out_affinity;
	}
	ret = register_trace_android_vh_mutex_opt_spin_start(proxy_mutex_spin_start, ctx);
	if (ret) {
		KUNIT_FAIL(test, "Cannot observe mutex spinning: %d", ret);
		goto out_affinity;
	}
	spin_probe = true;
	if (ctx->suppress_spin) {
		ret = register_trace_android_vh_mutex_can_spin_on_owner(proxy_mutex_can_spin, ctx);
		if (ret) {
			KUNIT_FAIL(test, "Cannot suppress initial mutex spinning: %d", ret);
			goto out_affinity;
		}
		can_spin_probe = true;
	}
	heartbeat = proxy_mutex_start(proxy_mutex_heartbeat, ctx, worker_cpu,
				      false, "mutex-heartbeat");
	if (IS_ERR(heartbeat)) {
		heartbeat = NULL;
		KUNIT_FAIL(test, "Cannot start heartbeat");
		goto out_affinity;
	}
	timeout = jiffies + msecs_to_jiffies(1000);
	while (!atomic_read(&ctx->pulses) && time_before(jiffies, timeout))
		msleep(20);
	if (!atomic_read(&ctx->pulses)) {
		KUNIT_FAIL(test, "Heartbeat did not start before contention");
		goto out_threads;
	}
	if (!recursive) {
		mutex_lock(&ctx->lock);
		owner_locked = true;
	}
	waiter = proxy_mutex_start(proxy_mutex_waiter, ctx, worker_cpu,
				   true, "mutex-waiter");
	if (IS_ERR(waiter)) {
		waiter = NULL;
		KUNIT_FAIL(test, "Cannot start waiter");
		goto out_threads;
	}
	if (!wait_for_completion_timeout(&ctx->entered, msecs_to_jiffies(1000))) {
		KUNIT_FAIL(test, "Waiter did not start");
		goto out_threads;
	}
	if (recursive)
		KUNIT_EXPECT_TRUE_MSG(test, ctx->owner_confirmed,
				      "Waiter must own the mutex before recursive acquisition");
	if (wake_handoff && !proxy_mutex_wake_handoff(ctx, waiter)) {
		KUNIT_FAIL(test, "Could not wake a blocked recursive waiter with HANDOFF");
		goto out_threads;
	}
	/* Let the FIFO waiter block before checking unrelated fair-class work. */
	msleep(50);
	before = atomic_read(&ctx->pulses);
	msleep(200);
	after = atomic_read(&ctx->pulses);
	measured = true;
	if (recursive) {
		KUNIT_EXPECT_FALSE_MSG(test, completion_done(&ctx->done),
				       "Recursive waiter returned before cancellation");
		kunit_info(test, "self_spin_iterations=%d\n", atomic_read(&ctx->self_spins));
		KUNIT_EXPECT_EQ_MSG(test, atomic_read(&ctx->self_spins), 0,
				    "Recursive waiter spun on its own mutex after wakeup");
	}

out_threads:
	if (waiter) {
		/* Let a regressed self-spinner reschedule and observe cancellation. */
		sched_set_normal(waiter, 0);
		if (cancel || recursive)
			send_sig(killable ? SIGKILL : SIGUSR1, waiter, 1);
		if (owner_locked && !cancel) {
			mutex_unlock(&ctx->lock);
			owner_locked = false;
		}
		waiter_done = wait_for_completion_timeout(&ctx->done,
							 msecs_to_jiffies(5000));
		if (owner_locked) {
			mutex_unlock(&ctx->lock);
			owner_locked = false;
		}
		KUNIT_EXPECT_TRUE_MSG(test, waiter_done, "Waiter failed to exit");
		if (waiter_done) {
			KUNIT_EXPECT_EQ(test, ctx->result,
					cancel || recursive ? -EINTR : 0);
			proxy_mutex_stop(waiter);
		} else {
			/* Preserve task data if a broken kernel cannot recover. */
			put_task_struct(waiter);
			goto out_affinity;
		}
	}
	if (owner_locked)
		mutex_unlock(&ctx->lock);
	if (heartbeat)
		proxy_mutex_stop(heartbeat);
	if (measured) {
		kunit_info(test, "recursive=%d killable=%d cancel=%d wake_handoff=%d no_spin=%d heartbeat_delta=%d\n",
			   recursive, killable, cancel, wake_handoff,
			   ctx->suppress_spin, after - before);
		KUNIT_EXPECT_GT_MSG(test, after, before,
				    "Mutex waiter starved unrelated work on CPU %d", worker_cpu);
	}
out_affinity:
	/* Keep the probe's data alive too if a broken waiter cannot recover. */
	if (!waiter || waiter_done) {
		if (can_spin_probe)
			unregister_trace_android_vh_mutex_can_spin_on_owner(
				proxy_mutex_can_spin, ctx);
		if (spin_probe)
			unregister_trace_android_vh_mutex_opt_spin_start(
				proxy_mutex_spin_start, ctx);
		if (can_spin_probe || spin_probe)
			tracepoint_synchronize_unregister();
	}
	KUNIT_EXPECT_EQ(test, set_cpus_allowed_ptr(current, saved), 0);
	cpus_read_unlock();
	free_cpumask_var(saved);
	if (!waiter || waiter_done)
		kfree(ctx);
}

static void proxy_mutex_recursive_interruptible(struct kunit *test)
{
	proxy_mutex_run(test, true, false, true, false);
}

static void proxy_mutex_recursive_killable(struct kunit *test)
{
	proxy_mutex_run(test, true, true, true, false);
}

static void proxy_mutex_recursive_handoff_interruptible(struct kunit *test)
{
	proxy_mutex_run(test, true, false, true, true);
}

static void proxy_mutex_recursive_handoff_killable(struct kunit *test)
{
	proxy_mutex_run(test, true, true, true, true);
}

static void proxy_mutex_handoff(struct kunit *test)
{
	proxy_mutex_run(test, false, false, false, false);
}

static void proxy_mutex_cancel(struct kunit *test)
{
	proxy_mutex_run(test, false, false, true, false);
}

static const bool proxy_mutex_spin_modes[] = { false, true };

static void proxy_mutex_spin_mode_desc(const bool *suppress_spin, char *desc)
{
	strscpy(desc, *suppress_spin ? "hook_no_spin" : "normal_spin",
		KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(proxy_mutex, proxy_mutex_spin_modes, proxy_mutex_spin_mode_desc);

static struct kunit_case proxy_mutex_cases[] = {
	KUNIT_CASE_PARAM(proxy_mutex_recursive_interruptible, proxy_mutex_gen_params),
	KUNIT_CASE_PARAM(proxy_mutex_recursive_killable, proxy_mutex_gen_params),
	KUNIT_CASE_PARAM(proxy_mutex_recursive_handoff_interruptible, proxy_mutex_gen_params),
	KUNIT_CASE_PARAM(proxy_mutex_recursive_handoff_killable, proxy_mutex_gen_params),
	KUNIT_CASE_PARAM(proxy_mutex_handoff, proxy_mutex_gen_params),
	KUNIT_CASE_PARAM(proxy_mutex_cancel, proxy_mutex_gen_params),
	{}
};

static struct kunit_suite proxy_mutex_suite = {
	.name = "proxy-mutex",
	.test_cases = proxy_mutex_cases,
};

kunit_test_suite(proxy_mutex_suite);
