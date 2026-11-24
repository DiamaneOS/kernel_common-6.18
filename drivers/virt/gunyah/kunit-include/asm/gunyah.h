/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2026 The DiamaneOS Project
 *
 * What gunyah_mem_test.c needs of arch/arm64/include/asm/gunyah.h on UML.
 */
#ifndef _ASM_GUNYAH_H
#define _ASM_GUNYAH_H

#define ARCH_GUNYAH_DEFAULT_MEMTYPE	0xf

/* The test has no interrupt controller. RPC reply processing is real. */
struct irq_fwspec;
static inline int arch_gunyah_fill_irq_fwspec_params(u32 virq, struct irq_fwspec *fwspec)
{
	return -EOPNOTSUPP;
}

#endif
