/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2026 The DiamaneOS Project
 *
 * The memory pool for protected VMs (vm_mgr_pool_mem.c).
 */

#ifndef _GUNYAH_VM_MGR_POOL_H
#define _GUNYAH_VM_MGR_POOL_H

#include "vm_mgr.h"

#ifdef CONFIG_GUNYAH_LEND_UPFRONT
int gunyah_vm_pool_init(void);
void gunyah_vm_pool_exit(void);
bool gunyah_vm_pool_mem_file(struct file *file);
int gunyah_vm_binding_pool_alloc(struct gunyah_vm *ghvm,
				 struct gunyah_map_cma_mem_args *args, struct file *file);
int gunyah_vm_pool_share_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
				struct gunyah_vm_binding *b, u64 *gfn, u64 *nr);
int gunyah_vm_pool_reclaim_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
				  struct gunyah_vm_binding *b);
#else
static inline int gunyah_vm_pool_init(void)
{
	return 0;
}
static inline void gunyah_vm_pool_exit(void)
{
}
static inline bool gunyah_vm_pool_mem_file(struct file *file)
{
	return false;
}
static inline int gunyah_vm_binding_pool_alloc(struct gunyah_vm *ghvm,
					       struct gunyah_map_cma_mem_args *args,
					       struct file *file)
{
	fput(file);
	return -EINVAL;
}
static inline int gunyah_vm_pool_share_parcel(struct gunyah_vm *ghvm,
					      struct gunyah_vm_parcel *vm_parcel,
					      struct gunyah_vm_binding *b, u64 *gfn, u64 *nr)
{
	return -EINVAL;
}
static inline int gunyah_vm_pool_reclaim_parcel(struct gunyah_vm *ghvm,
						struct gunyah_vm_parcel *vm_parcel,
						struct gunyah_vm_binding *b)
{
	return -EINVAL;
}
#endif

#endif
