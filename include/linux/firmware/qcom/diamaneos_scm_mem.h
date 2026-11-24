/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 The DiamaneOS Project */
#ifndef _DIAMANEOS_SCM_MEM_H
#define _DIAMANEOS_SCM_MEM_H

#include <linux/errno.h>
#include <linux/types.h>

/*
 * DiamaneOS: the assignment interface keeps these failures distinct without
 * changing its exported prototype. NOT_ISSUED is its local allocation failure.
 * REFUSED is reserved for a documented refusal with no ownership change.
 * No FP6 firmware refusal has that established contract, so every failed
 * issued call, including a service error or a wait-queue failure, is UNKNOWN.
 * A notifier error is UNKNOWN too. A notifier that suppresses the SCM call
 * returns success and leaves ownership to the resource manager operation.
 */
enum qcom_scm_assign_failure {
	QCOM_SCM_ASSIGN_NOT_ISSUED = -ENOMEM,
	QCOM_SCM_ASSIGN_REFUSED = -ECANCELED,
	QCOM_SCM_ASSIGN_UNKNOWN = -ENOTRECOVERABLE,
};

static inline bool qcom_scm_assign_is_unknown(int ret)
{
	return ret && ret != QCOM_SCM_ASSIGN_NOT_ISSUED &&
		ret != QCOM_SCM_ASSIGN_REFUSED;
}

#endif
