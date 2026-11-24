/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 The DiamaneOS Project */
#ifndef _GUNYAH_VM_START_STATE_H
#define _GUNYAH_VM_START_STATE_H

#include <linux/slab.h>
#include <linux/xarray.h>

struct gunyah_vm;

struct gunyah_vm_start_state {
	int error;
};

/* Keep startup state outside the exported VM type. Its lifetime is the VM's. */
static DEFINE_XARRAY(gunyah_vm_start_states);

static int gunyah_vm_start_state_init(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_start_state *state;
	int ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	ret = xa_insert(&gunyah_vm_start_states, (unsigned long)ghvm, state, GFP_KERNEL);
	if (ret)
		kfree(state);
	return ret;
}

/* A VM reference keeps this entry alive. Callers serialize starts with status_lock. */
static int gunyah_vm_start_error(struct gunyah_vm *ghvm)
{
	struct gunyah_vm_start_state *state;

	state = xa_load(&gunyah_vm_start_states, (unsigned long)ghvm);
	return state ? READ_ONCE(state->error) : -ENODEV;
}

static void gunyah_vm_start_state_fail(struct gunyah_vm *ghvm, int error)
{
	struct gunyah_vm_start_state *state;

	state = xa_load(&gunyah_vm_start_states, (unsigned long)ghvm);
	if (WARN_ON_ONCE(!state))
		return;
	if (!state->error)
		WRITE_ONCE(state->error, error);
}

static void gunyah_vm_start_state_release(struct gunyah_vm *ghvm)
{
	kfree(xa_erase(&gunyah_vm_start_states, (unsigned long)ghvm));
}

#endif
