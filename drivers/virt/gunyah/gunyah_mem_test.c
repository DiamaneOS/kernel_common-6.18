// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 The DiamaneOS Project
 *
 * KUnit test: who holds VM memory after a lend, a rollback or a reclaim fails.
 *
 * The Gunyah driver needs arm64. This test builds its memory code into one
 * file for UML: the Qualcomm platform hooks, the resource manager calls,
 * ordinary (pinned) memory and the VM memory pool. A fake resource manager
 * answers, refuses or loses the reply of each call as a test says, and a fake
 * TrustZone keeps the owners of every run qcom_scm_assign_mem() moves. Memory
 * the host may not own must stay pinned or out of the pool, unzeroed, with its
 * pieces still counted.
 */

#define CONFIG_GUNYAH_LEND_UPFRONT 1
#define CONFIG_GUNYAH_PLATFORM_HOOKS 1

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/arm-smccc.h>
#include <linux/file.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

/* The fake TrustZone: the hooks' qcom_scm_assign_mem() comes here. */
#define qcom_scm_assign_mem gunyah_test_scm_assign_mem
#include <linux/firmware/qcom/qcom_scm.h>

/* No hypervisor on UML: the hooks find no Qualcomm extensions at boot. */
static void gunyah_test_smccc(struct arm_smccc_res *res)
{
	*res = (struct arm_smccc_res){};
}
#undef arm_smccc_1_1_invoke
#define arm_smccc_1_1_invoke(id, res) gunyah_test_smccc(res)

#include "gunyah_qcom.c"
#include "gunyah_platform_hooks.c"
/* DiamaneOS: exercise the real RPC transport and reply decoder on UML. */
#include <linux/module.h>
#undef module_init
#undef module_exit
#define module_init(fn) static typeof(fn) *const __maybe_unused test_init_##fn = fn
#define module_exit(fn) static typeof(fn) *const __maybe_unused test_exit_##fn = fn
#undef pr_fmt
static bool test_fail_reply_allocation;
static void *test_rm_zalloc(size_t size, gfp_t flags)
{
	return test_fail_reply_allocation ? NULL : kzalloc(size, flags);
}
#pragma push_macro("kzalloc")
#undef kzalloc
#define kzalloc test_rm_zalloc
struct kobject *hypervisor_kobj; /* No hypervisor sysfs tree on UML. */
#include "rsc_mgr.c"
#pragma pop_macro("kzalloc")
#undef pr_fmt
#define pr_fmt(fmt) fmt
#include "rsc_mgr_rpc.c"
#undef pr_fmt
#include "vm_mgr_mem.c"
#undef pr_fmt
#include "vm_mgr_pool_mem.c"
#include "vm_mgr_start_state.h"

#define TEST_VMID	45
#define TEST_GPA	0x80000000ULL
#define TEST_BLOCK	GUNYAH_VM_POOL_BLOCK
#define TEST_MARK	0xa5
#define TEST_HOST	BIT_ULL(QCOM_SCM_VMID_HLOS)
/* VMIDs up to QCOM_SCM_MAX_MANAGED_VMID are TrustZone owners themselves. */
#define TEST_VM		BIT_ULL(TEST_VMID)

enum gunyah_error gunyah_hypercall_addrspace_map(u64 capid, u64 extent_capid, u64 vbase,
						 u32 extent_attrs, u32 flags, u64 offset, u64 size)
{
	return GUNYAH_ERROR_UNIMPLEMENTED;
}

enum gunyah_error gunyah_hypercall_addrspace_unmap(u64 capid, u64 extent_capid, u64 vbase,
						   u32 flags, u64 offset, u64 size)
{
	return GUNYAH_ERROR_UNIMPLEMENTED;
}

enum gunyah_error gunyah_hypercall_memextent_donate(u32 options, u64 from_capid, u64 to_capid,
						    u64 offset, u64 size)
{
	return GUNYAH_ERROR_UNIMPLEMENTED;
}

/* One scripted answer of the fake resource manager. */
struct test_rm_step {
	u32 msg_id;
	int ret;
	/* The resource manager acted, but its reply never came. */
	bool lost;
};

static struct {
	struct test_rm_step steps[8];
	unsigned int n_steps, next_step;
	struct gunyah_rm rm;
	u8 request[16384];
	size_t request_size;
	bool invalid_handle;
	u32 raw_status;
	bool use_raw_status;
	/* Parcel handles the resource manager holds. */
	DECLARE_BITMAP(live, 64);
	u32 next_handle;
	unsigned int lends, appends, reclaims, stale_reclaims;
	/* The fake TrustZone: run address >> PAGE_SHIFT to its owners. */
	struct xarray owners;
	unsigned int scm_calls;
	/* Bit n set: the n-th assignment fails. */
	u64 scm_fail, scm_unknown, scm_move_on_failure, scm_not_issued;
} t;

static void test_rm_step(u32 msg_id, int ret, bool lost)
{
	t.steps[t.n_steps++] = (struct test_rm_step){ msg_id, ret, lost };
}

static int test_rm_call(u32 msg_id, const void *req, void **resp, size_t *resp_size,
			bool *lost)
{
	struct test_rm_step step = { .msg_id = msg_id };
	const struct gunyah_rm_mem_release_req *release = req;
	__le32 *handle;
	u32 h;

	*lost = false;
	if (t.next_step < t.n_steps && t.steps[t.next_step].msg_id == msg_id)
		step = t.steps[t.next_step++];

	switch (msg_id) {
	case GUNYAH_RM_RPC_MEM_LEND:
	case GUNYAH_RM_RPC_MEM_SHARE:
		t.lends++;
		if (step.ret && !step.lost)
			return step.ret;
		h = t.next_handle++;
		set_bit(h, t.live);
		if (step.lost) {
			*lost = true;
			return step.ret;
		}
		handle = kmalloc(sizeof(*handle), GFP_KERNEL);
		if (!handle) {
			*lost = true;
			return -ENOMEM;
		}
		*handle = cpu_to_le32(t.invalid_handle ? GUNYAH_MEM_HANDLE_INVAL : h);
		*resp = handle;
		*resp_size = sizeof(*handle);
		return 0;
	case GUNYAH_RM_RPC_MEM_APPEND:
		t.appends++;
		*lost = step.lost;
		return step.ret;
	case GUNYAH_RM_RPC_MEM_RECLAIM:
		t.reclaims++;
		h = le32_to_cpu(release->mem_handle);
		if (h >= 64 || !test_bit(h, t.live)) {
			t.stale_reclaims++;
			return -EINVAL;
		}
		if (step.ret && !step.lost)
			return step.ret;
		clear_bit(h, t.live);
		*lost = step.lost;
		return step.ret;
	default:
		return -EOPNOTSUPP;
	}
}

/* Only the hypercall is scripted. rsc_mgr.c sends and decodes every reply. */
enum gunyah_error gunyah_hypercall_msgq_send(u64 capid, size_t size, void *data,
					  u64 flags, bool *ready)
{
	struct gunyah_rm_rpc_hdr *hdr = data;
	struct {
		struct gunyah_rm_rpc_reply_hdr reply;
		__le32 handle;
	} reply = {};
	void *payload = NULL;
	size_t payload_size = 0;
	u32 id = le32_to_cpu(hdr->msg_id), status = 0;
	bool lost;
	int ret;

	*ready = true;
	if (FIELD_GET(RM_RPC_TYPE_MASK, hdr->type) == RM_RPC_TYPE_REQUEST)
		t.request_size = 0;
	if (size - sizeof(*hdr) > sizeof(t.request) - t.request_size)
		return GUNYAH_ERROR_ARG_SIZE;
	memcpy(t.request + t.request_size, hdr + 1, size - sizeof(*hdr));
	t.request_size += size - sizeof(*hdr);
	if (!(flags & GUNYAH_HYPERCALL_MSGQ_TX_FLAGS_PUSH))
		return GUNYAH_ERROR_OK;
	ret = test_rm_call(id, t.request, &payload, &payload_size, &lost);
	if (ret == -EPERM)
		status = GUNYAH_RM_ERROR_DENIED;
	else if (ret == -EBUSY)
		status = GUNYAH_RM_ERROR_BUSY;
	else if (ret == -ENOMEM)
		status = GUNYAH_RM_ERROR_NOMEM;
	else if (ret)
		status = GUNYAH_RM_ERROR_ARGUMENT_INVALID;
	if (t.use_raw_status && id == GUNYAH_RM_RPC_MEM_RECLAIM) {
		status = t.raw_status;
		if (status == GUNYAH_RM_ERROR_HANDLE_INVALID ||
		    status == GUNYAH_RM_ERROR_MEM_RELEASED)
			clear_bit(le32_to_cpu(((struct gunyah_rm_mem_release_req *)t.request)->mem_handle), t.live);
	}
	reply.reply.hdr = *hdr;
	reply.reply.hdr.type = FIELD_PREP(RM_RPC_TYPE_MASK, RM_RPC_TYPE_REPLY);
	reply.reply.err_code = cpu_to_le32(status);
	if (payload_size == sizeof(reply.handle))
		memcpy(&reply.handle, payload, payload_size);
	if (lost) {
		/* Real receive failure: allocation failure or an aborted partial reply. */
		reply.reply.hdr.type |= FIELD_PREP(RM_RPC_FRAGMENTS_MASK, 1);
		test_fail_reply_allocation = ret == -ENOMEM;
		gunyah_rm_process_reply(&t.rm, &reply, sizeof(reply));
		test_fail_reply_allocation = false;
		if (t.rm.active_rx_message)
			gunyah_rm_abort_message(&t.rm);
	} else {
		gunyah_rm_process_reply(&t.rm, &reply, sizeof(reply.reply) + payload_size);
	}
	kfree(payload);
	return GUNYAH_ERROR_OK;
}

enum gunyah_error gunyah_hypercall_msgq_recv(u64 capid, void *buff, size_t size,
					  size_t *recv_size, bool *ready)
{
	return GUNYAH_ERROR_MSGQUEUE_EMPTY;
}

long gunyah_dev_vm_mgr_ioctl(struct gunyah_rm *rm, unsigned int cmd, unsigned long arg)
{
	return -EOPNOTSUPP;
}

static u64 test_owner(phys_addr_t pa)
{
	void *e = xa_load(&t.owners, pa >> PAGE_SHIFT);

	return e ? xa_to_value(e) : TEST_HOST;
}

int gunyah_test_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src,
			       const struct qcom_scm_vmperm *newvm, unsigned int dest_cnt)
{
	unsigned int call = t.scm_calls++, i;
	u64 next = 0;

	if (call < 64 && (t.scm_not_issued & BIT_ULL(call)))
		return -ENOMEM;
	/* TrustZone moves a run only from the owners it has now. */
	if (*src != test_owner(mem_addr))
		return -EINVAL;
	for (i = 0; i < dest_cnt; i++)
		next |= BIT_ULL(newvm[i].vmid);
	if (call < 64 && (t.scm_fail & BIT_ULL(call))) {
		if (t.scm_move_on_failure & BIT_ULL(call))
			xa_store(&t.owners, mem_addr >> PAGE_SHIFT, xa_mk_value(next), GFP_KERNEL);
		/* A documented no-effect refusal, or an issued call without a completion. */
		return t.scm_unknown & BIT_ULL(call) ? -EIO : -ECANCELED;
	}
	xa_store(&t.owners, mem_addr >> PAGE_SHIFT, xa_mk_value(next), GFP_KERNEL);
	*src = next;
	return 0;
}

static int gunyah_mem_test_init(struct kunit *test)
{
	memset(&t, 0, sizeof(t));
	xa_init(&t.owners);
	xa_init_flags(&t.rm.call_xarray, XA_FLAGS_ALLOC);
	mutex_init(&t.rm.send_lock);
	init_completion(&t.rm.send_ready);
	complete(&t.rm.send_ready);
	BLOCKING_INIT_NOTIFIER_HEAD(&t.rm.nh);
	t.next_handle = 1;
	gunyah_vm_pieces = 0;
	gunyah_vm_pieces_lost = 0;
	gunyah_vm_pieces_max = GUNYAH_VM_PIECES_MAX;
	atomic64_set(&gunyah_vm_ordinary_lost_bytes, 0);
	return gunyah_rm_register_platform_ops(&qcom_scm_gunyah_rm_platform_ops);
}

static void gunyah_mem_test_exit(struct kunit *test)
{
	gunyah_rm_unregister_platform_ops(&qcom_scm_gunyah_rm_platform_ops);
	xa_destroy(&t.owners);
	xa_destroy(&t.rm.call_xarray);
}

static struct gunyah_vm *test_vm(struct kunit *test, const char *name)
{
	struct gunyah_vm *ghvm = kunit_kzalloc(test, sizeof(*ghvm), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ghvm);
	mt_init(&ghvm->mm);
	mt_init(&ghvm->bindings);
	init_rwsem(&ghvm->bindings_lock);
	ghvm->parent = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ghvm->parent);
	/* Only passed through to the fakes. */
	ghvm->rm = &t.rm;
	ghvm->vmid = TEST_VMID;
	ghvm->is_protected = true;
	KUNIT_ASSERT_EQ(test, 0, kunit_attach_mm());
	ghvm->mm_s = current->mm;
	return ghvm;
}

/* The memory steps of _gunyah_vm_put(): the last reclaim, then every binding and its file. */
static int test_vm_destroy(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_binding *b;
	unsigned long index = 0;
	int ret;

	ret = gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX);
	gunyah_vm_quarantine_parcels(ghvm);
	down_write(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, index, ULONG_MAX) {
		mtree_erase(&ghvm->bindings, gunyah_gpa_to_gfn(b->guest_phys_addr));
		if ((b->mem_type == VM_MEM_CMA || b->mem_type == VM_MEM_POOL) && b->cma.file)
			fput(b->cma.file);
		kfree(b);
	}
	up_write(&ghvm->bindings_lock);
	mtree_destroy(&ghvm->bindings);
	mtree_destroy(&ghvm->mm);
	flush_delayed_fput();
	return ret;
}

static int test_share(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel)
{
	u64 gfn = gunyah_gpa_to_gfn(TEST_GPA), nr;
	struct gunyah_vm_binding *b = mtree_load(&ghvm->bindings, gfn);

	if (!b)
		return -ENOENT;
	nr = gunyah_gpa_to_gfn(b->size);
	return gunyah_share_parcel(ghvm, vm_parcel, &gfn, &nr);
}

struct test_pool {
	struct gunyah_vm_pool pool;
	struct page *chunk[2];
};

static void test_pool_done(void *data)
{
	struct test_pool *tp = data;

	gunyah_vm_pool = NULL;
	/* Blocks the pool never got back stay out, as on the phone. */
	if (gen_pool_avail(tp->pool.blocks) != tp->pool.size)
		return;
	gen_pool_destroy(tp->pool.blocks);
	__free_pages(tp->chunk[0], get_order(TEST_BLOCK));
	__free_pages(tp->chunk[1], get_order(TEST_BLOCK));
}

/* A pool of two blocks that are not adjacent, so a file of both is lent in two runs. */
static void test_pool(struct kunit *test)
{
	struct test_pool *tp = kunit_kzalloc(test, sizeof(*tp), GFP_KERNEL);
	struct gunyah_vm_pool *pool = &tp->pool;
	struct page *c[4], *tmp;
	int i, j;

	KUNIT_ASSERT_NOT_NULL(test, tp);
	for (i = 0; i < 4; i++) {
		c[i] = alloc_pages(GFP_KERNEL, get_order(TEST_BLOCK));
		KUNIT_ASSERT_NOT_NULL(test, c[i]);
	}
	for (i = 0; i < 4; i++)
		for (j = i + 1; j < 4; j++)
			if (page_to_phys(c[j]) < page_to_phys(c[i])) {
				tmp = c[i];
				c[i] = c[j];
				c[j] = tmp;
			}
	pool->blocks = gen_pool_create(GUNYAH_VM_POOL_SHIFT, NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, pool->blocks);
	KUNIT_ASSERT_EQ(test, 0, gen_pool_add(pool->blocks, page_to_phys(c[0]), TEST_BLOCK,
					      NUMA_NO_NODE));
	KUNIT_ASSERT_EQ(test, 0, gen_pool_add(pool->blocks, page_to_phys(c[2]), TEST_BLOCK,
					      NUMA_NO_NODE));
	__free_pages(c[1], get_order(TEST_BLOCK));
	__free_pages(c[3], get_order(TEST_BLOCK));
	tp->chunk[0] = c[0];
	tp->chunk[1] = c[2];
	pool->size = 2 * TEST_BLOCK;
	mutex_init(&pool->lock);
	atomic64_set(&pool->lost, 0);
	gunyah_vm_pool = pool;
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test, test_pool_done, tp));
}

/* A file of the whole pool, bound to @ghvm for a lend at TEST_GPA. Its blocks carry a mark. */
static void test_pool_bind(struct kunit *test, struct gunyah_vm *ghvm, phys_addr_t block[2])
{
	struct gunyah_map_cma_mem_args args = {
		.guest_addr = TEST_GPA,
		.flags = GUNYAH_MEM_FORCE_LEND | GUNYAH_MEM_ALLOW_RWX,
		.size = 2 * TEST_BLOCK,
	};
	struct gunyah_vm_pool_mem *mem;
	struct file *file;
	int i;

	mem = kvzalloc(struct_size(mem, block, 2), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mem);
	mem->nr_blocks = 2;
	mem->size = 2 * TEST_BLOCK;
	mem->shift = GUNYAH_VM_POOL_SHIFT;
	mutex_init(&mem->lock);
	for (i = 0; i < 2; i++) {
		mem->block[i] = gen_pool_alloc(gunyah_vm_pool->blocks, TEST_BLOCK);
		KUNIT_ASSERT_NE(test, mem->block[i], 0);
		memset(phys_to_virt(mem->block[i]), TEST_MARK, PAGE_SIZE);
		block[i] = mem->block[i];
	}
	file = anon_inode_create_getfile("[gunyah-vm-pool]", &gunyah_vm_pool_mem_fops, mem,
					 O_RDWR, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	KUNIT_ASSERT_EQ(test, 0, gunyah_vm_binding_pool_alloc(ghvm, &args, file));
}

static u64 test_attr(struct kunit *test,
		     ssize_t (*show)(struct device *, struct device_attribute *, char *))
{
	/* sysfs_emit() writes only to a page. */
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	u64 v;

	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_GT(test, show(NULL, NULL, buf), 0);
	KUNIT_ASSERT_EQ(test, 0, kstrtou64(buf, 10, &v));
	return v;
}

/* The pool's blocks are out of use, unzeroed, and counted lost, with their two pieces. */
static void test_pool_quarantined(struct kunit *test, phys_addr_t block[2])
{
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_show), 2);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_uncertain_show), 2);
	KUNIT_EXPECT_EQ(test, test_attr(test, free_bytes_show), 0);
	KUNIT_EXPECT_EQ(test, test_attr(test, lost_bytes_show), 2 * TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, *(u8 *)phys_to_virt(block[0]), TEST_MARK);
	KUNIT_EXPECT_EQ(test, *(u8 *)phys_to_virt(block[1]), TEST_MARK);
}

/* Nothing is held, nothing is lost. */
static void test_all_back(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_show), 0);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_uncertain_show), 0);
	KUNIT_EXPECT_EQ(test, test_attr(test, ordinary_lost_bytes_show), 0);
}

/* An ordinary memory binding of @pages pages at TEST_GPA, every page faulted in. */
static unsigned long test_user_bind(struct kunit *test, struct gunyah_vm *ghvm,
				    unsigned long pages)
{
	struct gunyah_userspace_memory_region region = {
		.flags = GUNYAH_MEM_ALLOW_RWX,
		.guest_phys_addr = TEST_GPA,
		.memory_size = pages * PAGE_SIZE,
	};
	unsigned long i, uaddr;

	uaddr = kunit_vm_mmap(test, NULL, 0, region.memory_size, PROT_READ | PROT_WRITE,
			      MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_FALSE(test, !uaddr || IS_ERR_VALUE(uaddr));
	for (i = 0; i < pages; i++)
		KUNIT_ASSERT_EQ(test, 0, put_user(1, (int __user *)(uaddr + i * PAGE_SIZE)));
	region.userspace_addr = uaddr;
	KUNIT_ASSERT_EQ(test, 0, gunyah_vm_binding_alloc(ghvm, &region, true));
	return uaddr;
}

static bool test_pinned(struct kunit *test, unsigned long uaddr, phys_addr_t *pa)
{
	struct page *page;
	bool pinned;

	KUNIT_ASSERT_EQ(test, 1, get_user_pages_fast(uaddr, 1, 0, &page));
	pinned = folio_maybe_dma_pinned(page_folio(page));
	if (pa)
		*pa = page_to_phys(page);
	put_page(page);
	return pinned;
}

static void test_runs(struct gunyah_rm_mem_entry *runs, unsigned int n, u64 owner)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		/* Runs with gaps between them, as scattered memory gives. */
		runs[i].phys_addr = cpu_to_le64(0x40000000ULL + i * SZ_4M);
		runs[i].size = cpu_to_le64(SZ_2M);
		if (owner != TEST_HOST)
			xa_store(&t.owners, (0x40000000ULL + i * SZ_4M) >> PAGE_SHIFT,
				 xa_mk_value(owner), GFP_KERNEL);
	}
}

static struct gunyah_rm_mem_acl_entry test_acl = {
	.vmid = cpu_to_le16(TEST_VMID),
	.perms = GUNYAH_RM_ACL_R | GUNYAH_RM_ACL_W | GUNYAH_RM_ACL_X,
};

/* One run TrustZone refuses to give back is reported, and every other run still goes back. */
static void qcom_reclaim_keeps_first_failure(struct kunit *test)
{
	struct gunyah_rm_mem_entry runs[3];
	struct gunyah_rm_mem_parcel p = {
		.n_acl_entries = 1, .acl_entries = &test_acl,
		.n_mem_entries = 3, .mem_entries = runs,
	};
	unsigned int fail, i;

	for (fail = 0; fail < 3; fail++) {
		test_runs(runs, 3, TEST_VM);
		t.scm_calls = 0;
		t.scm_fail = BIT_ULL(fail);
		KUNIT_EXPECT_NE_MSG(test, 0, qcom_scm_gunyah_rm_post_mem_reclaim(NULL, &p),
				    "run %u stayed with the VM", fail);
		KUNIT_EXPECT_EQ(test, t.scm_calls, 3);
		for (i = 0; i < 3; i++)
			KUNIT_EXPECT_EQ(test, test_owner(le64_to_cpu(runs[i].phys_addr)),
					i == fail ? TEST_VM : TEST_HOST);
	}
}

/* A lend the hooks cannot roll back says so: one run is still assigned to the VM. */
static void qcom_share_rollback_failure(struct kunit *test)
{
	struct gunyah_rm_mem_entry runs[3];
	struct gunyah_rm_mem_parcel p = {
		.n_acl_entries = 1, .acl_entries = &test_acl,
		.n_mem_entries = 3, .mem_entries = runs,
	};

	test_runs(runs, 3, TEST_HOST);
	/* Run 2 is refused, then run 1 does not come back. */
	t.scm_fail = BIT_ULL(2) | BIT_ULL(3);
	KUNIT_EXPECT_EQ(test, qcom_scm_gunyah_rm_pre_mem_share(NULL, &p), -ENOTRECOVERABLE);
	KUNIT_EXPECT_EQ(test, test_owner(le64_to_cpu(runs[0].phys_addr)), TEST_HOST);
	KUNIT_EXPECT_EQ(test, test_owner(le64_to_cpu(runs[1].phys_addr)), TEST_VM);
	KUNIT_EXPECT_EQ(test, test_owner(le64_to_cpu(runs[2].phys_addr)), TEST_HOST);
}

/* The pool lend's platform rollback fails: the blocks stay out of the pool. */
static void pool_share_rollback_failure(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	/* Run 1 is refused, then run 0 does not come back. */
	t.scm_fail = BIT_ULL(1) | BIT_ULL(2);
	KUNIT_EXPECT_NE(test, 0, test_share(ghvm, vp));
	KUNIT_EXPECT_EQ(test, t.lends, 0);
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_VM);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_vm_destroy(ghvm);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_pool_quarantined(test, block);
}

/* The resource manager refuses the lend and one run does not come back from TrustZone. */
static void pool_refused_lend_rollback_failure(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -EPERM, false);
	/* Lent in calls 0 and 1, run 0's rollback (call 2) fails, run 1's works. */
	t.scm_fail = BIT_ULL(2);
	KUNIT_EXPECT_EQ(test, -EPERM, test_share(ghvm, vp));
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_VM);
	KUNIT_EXPECT_EQ(test, test_owner(block[1]), TEST_HOST);
	test_vm_destroy(ghvm);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_pool_quarantined(test, block);
}

/* The lend's reply is lost: TrustZone keeps the runs assigned and the pieces stay counted. */
static void pool_lost_lend_reply(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm"), *next;
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -EIO, true);
	KUNIT_EXPECT_EQ(test, -EIO, test_share(ghvm, vp));
	KUNIT_EXPECT_EQ(test, t.scm_calls, 2);
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_VM);
	KUNIT_EXPECT_EQ(test, test_owner(block[1]), TEST_VM);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_vm_destroy(ghvm);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_pool_quarantined(test, block);

	/* The next VM is admitted against those pieces too. */
	gunyah_vm_pieces_max = 3;
	next = test_vm(test, "gunyah_test_next_vm");
	KUNIT_EXPECT_EQ(test, -E2BIG, gunyah_vm_pieces_take(next, 0, 2));
	KUNIT_EXPECT_EQ(test, 0, gunyah_vm_pieces_take(next, 0, 1));
	gunyah_vm_pieces_give(next, 1);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
}

/* RM releases the parcel at teardown but TrustZone keeps run 0: nothing goes back to the pool. */
static void pool_reclaim_platform_failure(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	KUNIT_ASSERT_EQ(test, 0, test_share(ghvm, vp));
	t.scm_fail = BIT_ULL(2);
	KUNIT_EXPECT_NE(test, 0, gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_VM);
	KUNIT_EXPECT_EQ(test, test_owner(block[1]), TEST_HOST);
	/* A second teardown pass never sends the released handle again. */
	gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX);
	test_vm_destroy(ghvm);
	KUNIT_EXPECT_EQ(test, t.reclaims, 1);
	KUNIT_EXPECT_EQ(test, t.stale_reclaims, 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_pool_quarantined(test, block);
}

/* A refused reclaim keeps the parcel lent, and a later reclaim gets it back once. */
static void pool_repeated_teardown(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	KUNIT_ASSERT_EQ(test, 0, test_share(ghvm, vp));
	test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false);
	KUNIT_EXPECT_EQ(test, -EBUSY, gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_VM);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	KUNIT_EXPECT_EQ(test, 0, gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	KUNIT_EXPECT_EQ(test, 0, gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 0);
	KUNIT_EXPECT_EQ(test, 0, test_vm_destroy(ghvm));
	KUNIT_EXPECT_EQ(test, t.reclaims, 2);
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_HOST);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 0);
	KUNIT_EXPECT_EQ(test, test_attr(test, free_bytes_show), 2 * TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, test_attr(test, lost_bytes_show), 0);
	KUNIT_EXPECT_EQ(test, *(u8 *)phys_to_virt(block[0]), 0);
	test_all_back(test);
}

/* Ordinary memory: a lost lend reply keeps the page pinned and charged. */
static void gup_lost_lend_reply(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	unsigned long uaddr = test_user_bind(test, ghvm, 1);
	phys_addr_t pa;

	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -ENOMEM, true);
	KUNIT_EXPECT_EQ(test, -ENOMEM, test_share(ghvm, vp));
	KUNIT_EXPECT_TRUE(test, test_pinned(test, uaddr, &pa));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, test_owner(pa), TEST_VM);
	test_vm_destroy(ghvm);
	KUNIT_EXPECT_TRUE(test, test_pinned(test, uaddr, NULL));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_show), 1);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_uncertain_show), 1);
	KUNIT_EXPECT_EQ(test, test_attr(test, ordinary_lost_bytes_show), PAGE_SIZE);
}

/* Ordinary memory: a refused lend that TrustZone rolled back is the host's again. */
static void gup_refused_lend(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	unsigned long uaddr = test_user_bind(test, ghvm, 1);
	phys_addr_t pa;

	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -EPERM, false);
	KUNIT_EXPECT_EQ(test, -EPERM, test_share(ghvm, vp));
	KUNIT_EXPECT_FALSE(test, test_pinned(test, uaddr, &pa));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 0UL);
	KUNIT_EXPECT_EQ(test, test_owner(pa), TEST_HOST);
	test_vm_destroy(ghvm);
	test_all_back(test);
}

/* Ordinary memory: a refused reclaim keeps the pin, the next reclaim drops it once. */
static void gup_repeated_teardown(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	unsigned long uaddr = test_user_bind(test, ghvm, 1);
	phys_addr_t pa;

	KUNIT_ASSERT_EQ(test, 0, test_share(ghvm, vp));
	test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false);
	KUNIT_EXPECT_EQ(test, -EBUSY, gunyah_reclaim_parcels(ghvm, 0, ULONG_MAX));
	KUNIT_EXPECT_TRUE(test, test_pinned(test, uaddr, &pa));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
	KUNIT_EXPECT_EQ(test, 0, test_vm_destroy(ghvm));
	KUNIT_EXPECT_FALSE(test, test_pinned(test, uaddr, NULL));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 0UL);
	KUNIT_EXPECT_EQ(test, test_owner(pa), TEST_HOST);
	KUNIT_EXPECT_EQ(test, t.reclaims, 2);
	test_all_back(test);
}

/* More runs than one message holds: an append fails and its reclaim is refused. */
static void rpc_append_failure_keeps_handle(struct kunit *test)
{
	unsigned int n = GUNYAH_RM_MAX_MEM_ENTRIES + 1, i;
	struct gunyah_rm_mem_entry *runs = kunit_kcalloc(test, n, sizeof(*runs), GFP_KERNEL);
	struct gunyah_rm_mem_parcel p = {
		.n_acl_entries = 1, .acl_entries = &test_acl,
		.n_mem_entries = n, .mem_entries = runs,
		.mem_handle = GUNYAH_MEM_HANDLE_INVAL,
	};
	enum gunyah_rm_mem_owner owner;

	KUNIT_ASSERT_NOT_NULL(test, runs);
	test_runs(runs, n, TEST_HOST);
	test_rm_step(GUNYAH_RM_RPC_MEM_APPEND, -EPERM, false);
	test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false);
	KUNIT_EXPECT_EQ(test, -EPERM, gunyah_rm_mem_share(&t.rm, &p, &owner));
	/* The resource manager still holds the parcel: its handle comes out for a later reclaim. */
	KUNIT_EXPECT_EQ(test, owner, GUNYAH_RM_MEM_LENT);
	KUNIT_EXPECT_EQ(test, p.mem_handle, 1);
	KUNIT_EXPECT_EQ(test, t.scm_calls, n);
	KUNIT_EXPECT_EQ(test, 0, gunyah_rm_mem_reclaim(&t.rm, &p, &owner));
	KUNIT_EXPECT_EQ(test, owner, GUNYAH_RM_MEM_HOST);
	KUNIT_EXPECT_EQ(test, p.mem_handle, GUNYAH_MEM_HANDLE_INVAL);
	KUNIT_EXPECT_EQ(test, t.stale_reclaims, 0);
	for (i = 0; i < n; i++)
		KUNIT_EXPECT_EQ(test, test_owner(le64_to_cpu(runs[i].phys_addr)), TEST_HOST);
}

/* The owner each outcome of a lend and of a reclaim leaves. */
static void rpc_owner_outcomes(struct kunit *test)
{
	static const struct {
		const char *what;
		u32 msg_id;
		int ret;
		bool lost;
		u64 scm_fail;
		bool reclaim;
		int want_ret;
		enum gunyah_rm_mem_owner want;
	} cases[] = {
		{ "lent", 0, 0, false, 0, false, 0, GUNYAH_RM_MEM_LENT },
		{ "lend refused", GUNYAH_RM_RPC_MEM_LEND, -EPERM, false, 0, false, -EPERM,
		  GUNYAH_RM_MEM_HOST },
		{ "lend refused, rollback fails", GUNYAH_RM_RPC_MEM_LEND, -EPERM, false, BIT_ULL(2),
		  false, -EPERM, GUNYAH_RM_MEM_UNKNOWN },
		{ "lend reply lost", GUNYAH_RM_RPC_MEM_LEND, -EIO, true, 0, false, -EIO,
		  GUNYAH_RM_MEM_UNKNOWN },
		{ "platform refuses run 1", 0, 0, false, BIT_ULL(1), false, -ECANCELED,
		  GUNYAH_RM_MEM_HOST },
		{ "platform refuses run 1, run 0 stays", 0, 0, false, BIT_ULL(1) | BIT_ULL(2), false,
		  -ENOTRECOVERABLE, GUNYAH_RM_MEM_UNKNOWN },
		{ "reclaimed", 0, 0, false, 0, true, 0, GUNYAH_RM_MEM_HOST },
		{ "reclaim refused", GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false, 0, true, -EBUSY,
		  GUNYAH_RM_MEM_LENT },
		{ "reclaim reply lost", GUNYAH_RM_RPC_MEM_RECLAIM, -EIO, true, 0, true, -EIO,
		  GUNYAH_RM_MEM_UNKNOWN },
		{ "reclaimed, run 0 stays", 0, 0, false, BIT_ULL(2), true, -ECANCELED,
		  GUNYAH_RM_MEM_UNKNOWN },
	};
	struct gunyah_rm_mem_entry runs[2];
	struct gunyah_rm_mem_parcel p = {
		.n_acl_entries = 1, .acl_entries = &test_acl,
		.n_mem_entries = 2, .mem_entries = runs,
	};
	enum gunyah_rm_mem_owner owner;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		gunyah_mem_test_exit(test);
		KUNIT_ASSERT_EQ(test, 0, gunyah_mem_test_init(test));
		test_runs(runs, 2, TEST_HOST);
		p.mem_handle = GUNYAH_MEM_HANDLE_INVAL;
		if (cases[i].reclaim)
			KUNIT_ASSERT_EQ(test, 0, gunyah_rm_mem_share(&t.rm, &p, &owner));
		if (cases[i].msg_id)
			test_rm_step(cases[i].msg_id, cases[i].ret, cases[i].lost);
		t.scm_fail = cases[i].scm_fail;
		owner = -1;
		if (cases[i].reclaim)
			ret = gunyah_rm_mem_reclaim(&t.rm, &p, &owner);
		else
			ret = gunyah_rm_mem_share(&t.rm, &p, &owner);
		KUNIT_EXPECT_EQ_MSG(test, ret, cases[i].want_ret, "%s", cases[i].what);
		KUNIT_EXPECT_EQ_MSG(test, owner, cases[i].want, "%s", cases[i].what);
		/* A handle comes out exactly while the resource manager holds the parcel. */
		KUNIT_EXPECT_EQ_MSG(test, p.mem_handle != GUNYAH_MEM_HANDLE_INVAL,
				    owner == GUNYAH_RM_MEM_LENT, "%s", cases[i].what);
		/* Memory said to be the host's is the host's in TrustZone too. */
		if (owner == GUNYAH_RM_MEM_HOST) {
			KUNIT_EXPECT_EQ_MSG(test, test_owner(le64_to_cpu(runs[0].phys_addr)),
					    TEST_HOST, "%s", cases[i].what);
			KUNIT_EXPECT_EQ_MSG(test, test_owner(le64_to_cpu(runs[1].phys_addr)),
					    TEST_HOST, "%s", cases[i].what);
		}
	}
}

/* Ordinary memory still lent when its VM goes away stays pinned, counted and charged. */
static void gup_reclaim_refused_at_destroy(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	unsigned long uaddr = test_user_bind(test, ghvm, 1);

	KUNIT_ASSERT_EQ(test, 0, test_share(ghvm, vp));
	test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false);
	KUNIT_EXPECT_EQ(test, -EBUSY, test_vm_destroy(ghvm));
	KUNIT_EXPECT_TRUE(test, test_pinned(test, uaddr, NULL));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_show), 1);
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_uncertain_show), 1);
	KUNIT_EXPECT_EQ(test, test_attr(test, ordinary_lost_bytes_show), PAGE_SIZE);
}

/* A pool lend and its reclaim: the blocks come back zeroed and nothing is counted. */
static void pool_lend_and_reclaim(struct kunit *test)
{
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	struct gunyah_vm *ghvm = test_vm(test, "gunyah_test_vm");
	phys_addr_t block[2];

	test_pool(test);
	test_pool_bind(test, ghvm, block);
	KUNIT_ASSERT_EQ(test, 0, test_share(ghvm, vp));
	KUNIT_EXPECT_EQ(test, test_attr(test, vm_pieces_show), 2);
	KUNIT_EXPECT_EQ(test, test_attr(test, free_bytes_show), 0);
	KUNIT_EXPECT_EQ(test, 0, test_vm_destroy(ghvm));
	KUNIT_EXPECT_EQ(test, test_owner(block[0]), TEST_HOST);
	KUNIT_EXPECT_EQ(test, test_owner(block[1]), TEST_HOST);
	KUNIT_EXPECT_EQ(test, test_attr(test, free_bytes_show), 2 * TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, test_attr(test, lost_bytes_show), 0);
	KUNIT_EXPECT_EQ(test, *(u8 *)phys_to_virt(block[0]), 0);
	KUNIT_EXPECT_EQ(test, *(u8 *)phys_to_virt(block[1]), 0);
	test_all_back(test);
}

/* An issued SCM failure can have moved the failed run. The whole parcel stays out. */
static void pool_forward_scm_unknown(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "scm_unknown_pool");
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	phys_addr_t blocks[2];

	test_pool(test);
	test_pool_bind(test, vm, blocks);
	t.scm_fail = t.scm_unknown = t.scm_move_on_failure = BIT_ULL(0);
	KUNIT_EXPECT_NE(test, test_share(vm, vp), 0);
	KUNIT_EXPECT_EQ(test, t.lends, 0);
	test_vm_destroy(vm);
	test_pool_quarantined(test, blocks);
}

static void gup_forward_scm_unknown(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "scm_unknown_gup");
	struct gunyah_vm_parcel *vp = kunit_kzalloc(test, sizeof(*vp), GFP_KERNEL);
	unsigned long addr = test_user_bind(test, vm, 1);

	t.scm_fail = t.scm_unknown = t.scm_move_on_failure = BIT_ULL(0);
	KUNIT_EXPECT_NE(test, test_share(vm, vp), 0);
	test_vm_destroy(vm);
	KUNIT_EXPECT_TRUE(test, test_pinned(test, addr, NULL));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
	KUNIT_EXPECT_EQ(test, test_attr(test, ordinary_lost_bytes_show), PAGE_SIZE);
}

static void pool_same_binding_retry(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "pool_retry");
	struct gunyah_vm_parcel *vp = &vm->fw.parcel;
	phys_addr_t blocks[2];
	unsigned int calls;

	test_pool(test);
	test_pool_bind(test, vm, blocks);
	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -EIO, true);
	KUNIT_ASSERT_EQ(test, test_share(vm, vp), -EIO);
	calls = t.scm_calls;
	KUNIT_EXPECT_NE(test, test_share(vm, vp), 0);
	KUNIT_EXPECT_EQ(test, t.scm_calls, calls);
	KUNIT_EXPECT_EQ(test, t.lends, 1);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 2);
	test_vm_destroy(vm);
	test_pool_quarantined(test, blocks);
}

static void gup_same_binding_retry(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "gup_retry");
	struct gunyah_vm_parcel *vp = &vm->fw.parcel;
	unsigned long addr = test_user_bind(test, vm, 1);
	unsigned int calls;

	test_rm_step(GUNYAH_RM_RPC_MEM_LEND, -EIO, true);
	KUNIT_ASSERT_EQ(test, test_share(vm, vp), -EIO);
	calls = t.scm_calls;
	KUNIT_EXPECT_NE(test, test_share(vm, vp), 0);
	KUNIT_EXPECT_EQ(test, t.scm_calls, calls);
	KUNIT_EXPECT_EQ(test, t.lends, 1);
	test_vm_destroy(vm);
	KUNIT_EXPECT_TRUE(test, test_pinned(test, addr, NULL));
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
}

static void lent_same_binding_retry(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "lent_retry");
	struct gunyah_vm_parcel *vp = &vm->fw.parcel;
	u32 handle;

	test_user_bind(test, vm, 1);
	KUNIT_ASSERT_EQ(test, test_share(vm, vp), 0);
	handle = vp->parcel.mem_handle;
	KUNIT_EXPECT_NE(test, test_share(vm, vp), 0);
	KUNIT_EXPECT_EQ(test, vp->parcel.mem_handle, handle);
	KUNIT_EXPECT_EQ(test, t.lends, 1);
	KUNIT_EXPECT_EQ(test, test_vm_destroy(vm), 0);
	test_all_back(test);
}

static void rpc_invalid_lend_handle(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "bad_lend_handle");
	struct gunyah_vm_parcel *vp = &vm->fw.parcel;
	unsigned long addr = test_user_bind(test, vm, 1);

	t.invalid_handle = true;
	KUNIT_EXPECT_EQ(test, test_share(vm, vp), -EBADMSG);
	KUNIT_EXPECT_EQ(test, vp->parcel.mem_handle, GUNYAH_MEM_HANDLE_INVAL);
	test_vm_destroy(vm);
	KUNIT_EXPECT_TRUE(test, test_pinned(test, addr, NULL));
	KUNIT_EXPECT_EQ(test, t.reclaims, 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
}

static void rpc_reclaim_handle_identity(struct kunit *test)
{
	const u32 statuses[] = { GUNYAH_RM_ERROR_HANDLE_INVALID, GUNYAH_RM_ERROR_MEM_RELEASED,
		GUNYAH_RM_ERROR_LOOKUP_FAILED, GUNYAH_RM_ERROR_MEM_INVALID, 0x12345678 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(statuses); i++) {
		struct gunyah_vm *vm;
		struct gunyah_vm_parcel *vp;
		unsigned long addr;
		u32 handle;
		char name[32];

		gunyah_mem_test_exit(test);
		KUNIT_ASSERT_EQ(test, gunyah_mem_test_init(test), 0);
		snprintf(name, sizeof(name), "bad_reclaim_%u", i);
		vm = test_vm(test, name);
		vp = &vm->fw.parcel;
		addr = test_user_bind(test, vm, 1);
		KUNIT_ASSERT_EQ(test, test_share(vm, vp), 0);
		handle = vp->parcel.mem_handle;
		test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EINVAL, false);
		t.use_raw_status = true;
		t.raw_status = statuses[i];
		KUNIT_EXPECT_NE(test, gunyah_reclaim_parcels(vm, 0, ULONG_MAX), 0);
		KUNIT_EXPECT_EQ(test, vp->parcel.mem_handle, GUNYAH_MEM_HANDLE_INVAL);
		/* The firmware may reuse this number for a different parcel now. */
		set_bit(handle, t.live);
		t.use_raw_status = false;
		test_vm_destroy(vm);
		KUNIT_EXPECT_EQ(test, t.reclaims, 1);
		KUNIT_EXPECT_TRUE(test, test_bit(handle, t.live));
		KUNIT_EXPECT_TRUE(test, test_pinned(test, addr, NULL));
		KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
	}
}

static void qtvm_failed_rollback_binding(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "qtvm_rollback");
	struct gunyah_vm_parcel *array = NULL;
	struct gunyah_vm_binding *b;

	test_user_bind(test, vm, 1);
	/* Force failure after a live lend by making a second binding have no pages. */
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	b->guest_phys_addr = TEST_GPA + SZ_2M;
	b->size = PAGE_SIZE;
	b->share_type = VM_MEM_LEND;
	b->flags = GUNYAH_MEM_ALLOW_RWX;
	b->userspace_addr = 0;
	KUNIT_ASSERT_EQ(test, mtree_insert(&vm->bindings, gunyah_gpa_to_gfn(b->guest_phys_addr), b, GFP_KERNEL), 0);
	test_rm_step(GUNYAH_RM_RPC_MEM_RECLAIM, -EBUSY, false);
	KUNIT_EXPECT_NE(test, gunyah_share_range_as_parcels(vm, 0, ULONG_MAX, &array), 0);
	KUNIT_EXPECT_PTR_EQ(test, array, NULL);
	b = mtree_load(&vm->bindings, gunyah_gpa_to_gfn(TEST_GPA));
	/* Rollback must detach before freeing the array. No dangling dereference here. */
	KUNIT_EXPECT_PTR_EQ(test, b->vm_parcel, NULL);
	/* The pre-fix run must not traverse its known dangling pointer. */
	if (b->vm_parcel)
		b->vm_parcel = NULL;
	test_vm_destroy(vm);
	KUNIT_EXPECT_EQ(test, current->mm->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, test_attr(test, ordinary_lost_bytes_show), PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pieces_all(), 1);
}
/* A failed allocation before SCM entry leaves every block available to the host. */
static void pool_scm_not_issued(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "scm_not_issued");
	struct gunyah_vm_parcel *vp = &vm->fw.parcel;
	phys_addr_t blocks[2];

	test_pool(test);
	test_pool_bind(test, vm, blocks);
	t.scm_not_issued = BIT_ULL(0);
	KUNIT_EXPECT_EQ(test, test_share(vm, vp), -ENOMEM);
	KUNIT_EXPECT_EQ(test, t.lends, 0);
	test_vm_destroy(vm);
	test_all_back(test);
	KUNIT_EXPECT_EQ(test, test_attr(test, free_bytes_show), 2 * TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, test_attr(test, lost_bytes_show), 0);
}


static unsigned int test_cma_releases;

static int test_cma_release(struct inode *inode, struct file *file)
{
	test_cma_releases++;
	return 0;
}

static const struct file_operations test_cma_fops = {
	.release = test_cma_release,
};

/* A detached optional CMA parcel keeps the file that owns its backing alive. */
static void qtvm_cma_quarantine_keeps_file(struct kunit *test)
{
	struct gunyah_vm *vm = test_vm(test, "qtvm_cma_file");
	struct gunyah_vm_parcel vp = { .start = TEST_GPA >> PAGE_SHIFT, .pages = 1 };
	struct gunyah_vm_binding *b = kzalloc(sizeof(*b), GFP_KERNEL);
	struct file *file;

	KUNIT_ASSERT_NOT_NULL(test, b);
	test_cma_releases = 0;
	file = anon_inode_getfile("[gunyah-test-cma]", &test_cma_fops, NULL, O_RDWR);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	b->mem_type = VM_MEM_CMA;
	b->guest_phys_addr = TEST_GPA;
	b->size = PAGE_SIZE;
	b->vm_parcel = &vp;
	b->cma.file = file;
	KUNIT_ASSERT_EQ(test, mtree_insert(&vm->bindings, vp.start, b, GFP_KERNEL), 0);
	gunyah_vm_detach_quarantined(vm, &vp);
	KUNIT_EXPECT_PTR_EQ(test, b->vm_parcel, NULL);
	KUNIT_EXPECT_EQ(test, file_count(file), 2L);
	/* Preserve the fake file in the before run so its failure never causes a UAF. */
	if (file_count(file) == 1)
		get_file(file);
	test_vm_destroy(vm);
	KUNIT_EXPECT_EQ(test, test_cma_releases, 0);
	KUNIT_EXPECT_EQ(test, file_count(file), 1L);
	/* The fake file has no secure backing. Release its test-only quarantine. */
	fput(file);
	flush_delayed_fput();
	KUNIT_EXPECT_EQ(test, test_cma_releases, 1);
}

/* Error storage exists before startup and cannot survive reuse of a VM address. */
static void startup_error_state_lifetime(struct kunit *test)
{
	struct gunyah_vm *vm = kunit_kzalloc(test, sizeof(*vm), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, vm);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), -ENODEV);
	KUNIT_ASSERT_EQ(test, gunyah_vm_start_state_init(vm), 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), 0);
	gunyah_vm_start_state_fail(vm, -EIO);
	gunyah_vm_start_state_fail(vm, -EPERM);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), -EIO);
	KUNIT_EXPECT_LT(test, gunyah_vm_start_state_init(vm), 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), -EIO);
	gunyah_vm_start_state_release(vm);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), -ENODEV);
	KUNIT_ASSERT_EQ(test, gunyah_vm_start_state_init(vm), 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_start_error(vm), 0);
	gunyah_vm_start_state_release(vm);
	KUNIT_EXPECT_TRUE(test, xa_empty(&gunyah_vm_start_states));
}

/*
 * The decode service's VM keeps its room: while no file of its size is held, a
 * smaller file must leave that much free. With it held, or for a file of its
 * size, nothing is kept back, nor when lost blocks make room for it impossible.
 */
static void pool_keeps_room_for_the_decode_vm(struct kunit *test)
{
	struct gunyah_vm_pool *pool = kunit_kzalloc(test, sizeof(*pool), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, pool);
	pool->size = SZ_512M;
	atomic64_set(&pool->lost, 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pool_reserved(pool, SZ_128M), GUNYAH_VM_POOL_MAX_FILE);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pool_reserved(pool, GUNYAH_VM_POOL_MAX_FILE), 0);
	pool->largest = 1;
	KUNIT_EXPECT_EQ(test, gunyah_vm_pool_reserved(pool, SZ_128M), 0);
	pool->largest = 0;
	atomic64_set(&pool->lost, SZ_256M);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pool_reserved(pool, SZ_128M), 0);
	pool->size = SZ_256M;
	atomic64_set(&pool->lost, 0);
	KUNIT_EXPECT_EQ(test, gunyah_vm_pool_reserved(pool, SZ_128M), 0);
}

/* An ordinary-memory file of @size with blocks of order @top at most, its pieces in @pieces. */
static struct gunyah_vm_pool_mem *test_ordinary(struct kunit *test, u64 size, int top, size_t *pieces)
{
	struct gunyah_vm_pool_mem *mem;
	unsigned int got;
	unsigned long i;
	u64 *word;

	mem = kvzalloc(struct_size(mem, block, size >> PAGE_SHIFT), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mem);
	mem->nr_blocks = size >> PAGE_SHIFT;
	mem->size = size;
	mem->ordinary = true;
	mem->shift = PAGE_SHIFT;
	mutex_init(&mem->lock);
	gunyah_vm_ordinary_top = top;
	KUNIT_ASSERT_EQ(test, 0, gunyah_vm_ordinary_alloc(mem, &got));
	gunyah_vm_ordinary_top = -1;
	KUNIT_EXPECT_LE(test, got, (unsigned int)(top >= 0 ? top : MAX_PAGE_ORDER));
	for (i = 0; i < mem->nr_blocks; i++) {
		/* Every page zeroed, page aligned, none twice (they are in order within a block). */
		word = phys_to_virt(mem->block[i]);
		KUNIT_EXPECT_EQ(test, word[0] | word[PAGE_SIZE / sizeof(*word) - 1], 0);
		KUNIT_EXPECT_TRUE(test, PAGE_ALIGNED(mem->block[i]));
		if (i)
			KUNIT_EXPECT_NE(test, mem->block[i], mem->block[i - 1]);
	}
	*pieces = gunyah_vm_pool_pieces(mem);
	return mem;
}

/* Ordinary memory for a VM in the largest blocks the page allocator has: few pieces, pages back after release. */
static void ordinary_memory_in_large_blocks(struct kunit *test)
{
	struct gunyah_vm_pool_mem *mem;
	unsigned long before;
	size_t pieces;

	/* Pages on the per-CPU lists are not counted free: drained before each count. */
	drain_all_pages(NULL);
	before = global_zone_page_state(NR_FREE_PAGES);
	mem = test_ordinary(test, SZ_8M, -1, &pieces);
	/* At most 4 MiB blocks: 8 MiB in a few pieces where single pages would be 2048. */
	KUNIT_EXPECT_LE(test, pieces, 16);
	gunyah_vm_ordinary_free(mem, mem->nr_blocks);
	kvfree(mem);
	drain_all_pages(NULL);
	KUNIT_EXPECT_GE(test, global_zone_page_state(NR_FREE_PAGES) + SZ_1M / PAGE_SIZE, before);
}

/* A lower top order (an allocator without large blocks) still fills the file, in blocks of that order. */
static void ordinary_memory_falls_back_to_smaller_blocks(struct kunit *test)
{
	struct gunyah_vm_pool_mem *mem;
	size_t pieces;
	unsigned long i;

	mem = test_ordinary(test, SZ_1M, 2, &pieces);
	KUNIT_EXPECT_EQ(test, mem->nr_blocks, SZ_1M / PAGE_SIZE);
	/* Blocks of 4 pages: each starts a run on a 16 KiB boundary at least. */
	for (i = 0; i < mem->nr_blocks; i += 4)
		KUNIT_EXPECT_TRUE(test, IS_ALIGNED(mem->block[i], 4 * PAGE_SIZE));
	KUNIT_EXPECT_LE(test, pieces, mem->nr_blocks / 4);
	gunyah_vm_ordinary_free(mem, mem->nr_blocks);
	kvfree(mem);
	mem = test_ordinary(test, 3 * PAGE_SIZE, 0, &pieces);
	KUNIT_EXPECT_EQ(test, mem->nr_blocks, 3);
	gunyah_vm_ordinary_free(mem, mem->nr_blocks);
	kvfree(mem);
}

/* At most one direct compaction per file, only for a pageblock order or more that was not had. */
static void ordinary_memory_compacts_at_most_once(struct kunit *test)
{
	struct gunyah_vm_pool_mem *mem;
	unsigned int before = gunyah_vm_ordinary_compactions;
	size_t pieces;

	mem = test_ordinary(test, SZ_4M, -1, &pieces);
	KUNIT_EXPECT_LE(test, gunyah_vm_ordinary_compactions - before, 1);
	gunyah_vm_ordinary_free(mem, mem->nr_blocks);
	kvfree(mem);
	before = gunyah_vm_ordinary_compactions;
	mem = test_ordinary(test, SZ_1M, (int)pageblock_order - 1, &pieces);
	KUNIT_EXPECT_EQ(test, gunyah_vm_ordinary_compactions, before);
	gunyah_vm_ordinary_free(mem, mem->nr_blocks);
	kvfree(mem);
}

/* The budget of all VMs decides with the file's real pieces. */
static void ordinary_memory_pieces_fit_the_budget(struct kunit *test)
{
	u32 max = gunyah_vm_pieces_max, held = gunyah_vm_pieces;

	gunyah_vm_pieces_max = 100;
	gunyah_vm_pieces = 90;
	KUNIT_EXPECT_TRUE(test, gunyah_vm_pieces_fit(10));
	KUNIT_EXPECT_FALSE(test, gunyah_vm_pieces_fit(11));
	gunyah_vm_pieces_max = max;
	gunyah_vm_pieces = held;
}

static struct kunit_case gunyah_mem_test_cases[] = {
	KUNIT_CASE(startup_error_state_lifetime),
	KUNIT_CASE(qtvm_cma_quarantine_keeps_file),
	KUNIT_CASE(pool_scm_not_issued),
	KUNIT_CASE(pool_forward_scm_unknown),
	KUNIT_CASE(gup_forward_scm_unknown),
	KUNIT_CASE(pool_same_binding_retry),
	KUNIT_CASE(gup_same_binding_retry),
	KUNIT_CASE(lent_same_binding_retry),
	KUNIT_CASE(rpc_invalid_lend_handle),
	KUNIT_CASE(rpc_reclaim_handle_identity),
	KUNIT_CASE(qtvm_failed_rollback_binding),

	KUNIT_CASE(rpc_owner_outcomes),
	KUNIT_CASE(pool_lend_and_reclaim),
	KUNIT_CASE(qcom_reclaim_keeps_first_failure),
	KUNIT_CASE(qcom_share_rollback_failure),
	KUNIT_CASE(pool_share_rollback_failure),
	KUNIT_CASE(pool_refused_lend_rollback_failure),
	KUNIT_CASE(pool_lost_lend_reply),
	KUNIT_CASE(pool_reclaim_platform_failure),
	KUNIT_CASE(pool_repeated_teardown),
	KUNIT_CASE(pool_keeps_room_for_the_decode_vm),
	KUNIT_CASE(ordinary_memory_in_large_blocks),
	KUNIT_CASE(ordinary_memory_falls_back_to_smaller_blocks),
	KUNIT_CASE(ordinary_memory_compacts_at_most_once),
	KUNIT_CASE(ordinary_memory_pieces_fit_the_budget),
	KUNIT_CASE(gup_lost_lend_reply),
	KUNIT_CASE(gup_refused_lend),
	KUNIT_CASE(gup_repeated_teardown),
	KUNIT_CASE(gup_reclaim_refused_at_destroy),
	KUNIT_CASE(rpc_append_failure_keeps_handle),
	{}
};

static struct kunit_suite gunyah_mem_test_suite = {
	.name = "gunyah_mem_ownership",
	.init = gunyah_mem_test_init,
	.exit = gunyah_mem_test_exit,
	.test_cases = gunyah_mem_test_cases,
};
kunit_test_suite(gunyah_mem_test_suite);

MODULE_DESCRIPTION("KUnit test of Gunyah VM memory ownership after failures");
MODULE_LICENSE("GPL");
