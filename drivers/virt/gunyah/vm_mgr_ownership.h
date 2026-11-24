/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 The DiamaneOS Project */

/*
 * DiamaneOS: an optional QTVM rollback is about to free its parcel array.
 * Keep the pins or pool blocks and their pieces, but remove the array pointer.
 */
static void gunyah_vm_detach_quarantined(struct gunyah_vm *ghvm,
					struct gunyah_vm_parcel *vp)
{
	struct gunyah_vm_binding *b;

	down_write(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, vp->start);
	if (b && b->vm_parcel == vp) {
		if (b->mem_type == VM_MEM_USER)
			gunyah_vm_ordinary_lost(gunyah_gfn_to_gpa(vp->pages));
		else if (b->mem_type == VM_MEM_CMA) {
			/* The legacy release frees its backing on the last fput. */
			get_file(b->cma.file);
		} else if (b->mem_type == VM_MEM_POOL)
			gunyah_vm_pool_quarantine(b);
		gunyah_vm_pieces_uncertain(ghvm, vp->parcel.n_mem_entries);
		b->quarantined = true;
		b->vm_parcel = NULL;
	}
	up_write(&ghvm->bindings_lock);
	kvfree(vp->parcel.mem_entries);
	kfree(vp->parcel.acl_entries);
}
