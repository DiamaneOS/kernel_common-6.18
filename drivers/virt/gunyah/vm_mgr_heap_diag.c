// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only diagnostic: the hypervisor heaps around VM_START. Not for landing.
 * Logs the RM heap objects of this VM (resource type 11) once, and the stats
 * (VM_QUERY_HEAP_MEMORY, type 2) of each before a VM_START and after its reply.
 */
#include <linux/gunyah.h>
#include <linux/slab.h>
#include "rsc_mgr.h"
#include "vm_mgr.h"

#define DIAG_QUERY_HEAP 0x51000034
#define DIAG_RES_HEAP 11

struct diag_query { __le32 handle; u8 type; u8 reserved[3]; } __packed;
struct diag_stats { __le64 total, allocated, reserved, largest_free; } __packed;

static u32 diag_handles[8];
static int diag_n = -1;

static void diag_find(struct gunyah_rm *rm)
{
	struct gunyah_rm_hyp_resources *res;
	u16 self;
	int ret, i, n;

	diag_n = 0;
	ret = gunyah_rm_get_vmid(rm, &self);
	if (!ret)
		ret = gunyah_rm_get_hyp_resources(rm, self, &res);
	if (ret) {
		pr_info("heapdiag: own resources: %d\n", ret);
		return;
	}
	n = le32_to_cpu(res->n_entries);
	for (i = 0; i < n; i++) {
		struct gunyah_rm_hyp_resource *e = &res->entries[i];

		pr_info("heapdiag: self %u res %d type %u handle %u label %u cap %llx base %llx size %llx\n",
			self, i, e->type, le32_to_cpu(e->resource_handle), le32_to_cpu(e->resource_label),
			le64_to_cpu(e->cap_id), le64_to_cpu(e->base), le64_to_cpu(e->size));
		if (e->type == DIAG_RES_HEAP && diag_n + 3 <= ARRAY_SIZE(diag_handles)) {
			diag_handles[diag_n++] = le32_to_cpu(e->resource_handle);
			diag_handles[diag_n++] = le32_to_cpu(e->resource_label);
			diag_handles[diag_n++] = lower_32_bits(le64_to_cpu(e->cap_id));
		}
	}
	kfree(res);
}

void gunyah_heap_diag(struct gunyah_rm *rm, const char *when, u16 vmid)
{
	int i;

	if (diag_n < 0)
		diag_find(rm);
	for (i = 0; i < diag_n; i++) {
		struct diag_query q = { .handle = cpu_to_le32(diag_handles[i]), .type = 2 };
		struct diag_stats *s = NULL;
		size_t size = 0;
		int ret = gunyah_rm_call(rm, DIAG_QUERY_HEAP, &q, sizeof(q), (void **)&s, &size);

		if (!ret && size == sizeof(*s))
			pr_info("heapdiag: %s vm %u heap %u total %llu allocated %llu reserved %llu largest_free %llu\n",
				when, vmid, diag_handles[i], le64_to_cpu(s->total), le64_to_cpu(s->allocated),
				le64_to_cpu(s->reserved), le64_to_cpu(s->largest_free));
		else
			pr_info("heapdiag: %s vm %u heap %u: query %d size %zu\n", when, vmid, diag_handles[i], ret, size);
		kfree(s);
	}
}
