# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_GUNYAH_HYPERCALLS)		+= gunyah.o

gunyah_rsc_mgr-y += rsc_mgr.o rsc_mgr_rpc.o vm_mgr.o vm_mgr_mem.o vm_mgr_heap_diag.o
gunyah_rsc_mgr-$(CONFIG_GUNYAH_CMA_MEM) += vm_mgr_cma_mem.o
# DiamaneOS: the VM memory pool set aside at boot
gunyah_rsc_mgr-$(CONFIG_GUNYAH_LEND_UPFRONT) += vm_mgr_pool_mem.o
obj-$(CONFIG_GUNYAH) += gunyah_rsc_mgr.o gunyah_vcpu.o
obj-$(CONFIG_GUNYAH_QCOM_TRUSTED_VM) += gunyah_qtvm.o
obj-$(CONFIG_GUNYAH_PLATFORM_HOOKS) += gunyah_platform_hooks.o
obj-$(CONFIG_GUNYAH_QCOM_PLATFORM) += gunyah_qcom.o
obj-$(CONFIG_GUNYAH_IRQFD) += gunyah_irqfd.o
obj-$(CONFIG_GUNYAH_IOEVENTFD) += gunyah_ioeventfd.o
# DiamaneOS: the memory code with a scripted resource manager and TrustZone, on UML
obj-$(CONFIG_GUNYAH_MEM_KUNIT_TEST) += gunyah_mem_test.o
CFLAGS_gunyah_mem_test.o += -I$(src)/kunit-include
