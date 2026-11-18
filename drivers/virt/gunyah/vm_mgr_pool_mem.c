// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 The DiamaneOS Project
 *
 * Protected VM memory from a pool set aside at boot.
 *
 * Without demand paging every page of a VM is lent before it starts, and the
 * resource manager refuses a VM whose memory comes in too many physically
 * separate pieces (GUNYAH_LEND_MAX_PIECES). Once free memory is scattered,
 * by uptime or by memory pressure, even a small VM can need that many. This
 * pool keeps memory away from the page allocator from boot and hands it out
 * in 2 MiB blocks, so a VM's memory comes in a few pieces however scattered
 * the rest of memory is.
 *
 * The pool is the memory named "gunyah_vm_pool" by reserve_mem= on the kernel
 * command line, or kernel.reserve_mem in the bootconfig, for example
 * reserve_mem=432M:2M:gunyah_vm_pool. Its size is exactly what that line
 * sets, and nothing else in the kernel allocates from it. Without that line
 * there is no pool and no device node.
 *
 * GH_ANDROID_CREATE_POOL_MEM_FD on /dev/gunyah_vm_pool takes the blocks for
 * one VM's memory at once and returns a file that holds them, or fails with
 * ENOMEM when the pool cannot hold that much, so the caller can fall back to
 * ordinary memory. The VMM maps the file and binds it to one VM with
 * GH_VM_ANDROID_MAP_CMA_MEM. Pool memory is only ever lent, and only to a
 * protected VM. The blocks go back to the pool when the file is released,
 * zeroed before their next use, unless a reclaim from the hypervisor failed.
 * Then they stay out of use until the next boot.
 */

#define pr_fmt(fmt) "gunyah_vm_pool: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/genalloc.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "vm_mgr.h"

#define GUNYAH_VM_POOL_NAME		"gunyah_vm_pool"
#define GUNYAH_VM_POOL_SHIFT		21
#define GUNYAH_VM_POOL_BLOCK		BIT_ULL(GUNYAH_VM_POOL_SHIFT)
#define GUNYAH_VM_POOL_BLOCK_PAGES	(GUNYAH_VM_POOL_BLOCK >> PAGE_SHIFT)

struct gunyah_vm_pool {
	struct miscdevice miscdev;
	struct gen_pool *blocks;
	phys_addr_t base;
	phys_addr_t size;
};

static struct gunyah_vm_pool *gunyah_vm_pool;

/* The memory of one VM: a file made by GH_ANDROID_CREATE_POOL_MEM_FD. */
struct gunyah_vm_pool_mem {
	struct mutex lock;
	u64 size;
	/* Bound to a VM. A file is bound once, so its blocks are lent to one VM only. */
	bool bound;
	/* Parcels of this file the hypervisor holds now. */
	unsigned int lent;
	/* A reclaim failed: the hypervisor may still hold blocks, which never go back to the pool. */
	bool lost;
	unsigned long nr_blocks;
	phys_addr_t block[] __counted_by(nr_blocks);
};

static const struct file_operations gunyah_vm_pool_mem_fops;

bool gunyah_vm_pool_mem_file(struct file *file)
{
	return file->f_op == &gunyah_vm_pool_mem_fops;
}

static size_t gunyah_vm_pool_pieces(struct gunyah_vm_pool_mem *mem)
{
	size_t n = 1;
	unsigned long i;

	for (i = 1; i < mem->nr_blocks; i++)
		if (mem->block[i] != mem->block[i - 1] + GUNYAH_VM_POOL_BLOCK)
			n++;
	return n;
}

static int gunyah_vm_pool_mem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct gunyah_vm_pool_mem *mem = file->private_data;
	unsigned long addr = vma->vm_start, len = vma->vm_end - vma->vm_start;
	unsigned long num, i;
	struct page **pages;
	struct page *first;
	u64 off, in, chunk;
	int ret = 0;

	/* crosvm maps guest memory shared. A private mapping would copy pages on write. */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_pgoff > (mem->size >> PAGE_SHIFT))
		return -EINVAL;
	off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	if (len > mem->size - off)
		return -EINVAL;

	pages = kmalloc_array(GUNYAH_VM_POOL_BLOCK_PAGES, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* Not VM_IO: crosvm marks guest memory MADV_DONTFORK, which an I/O mapping refuses. */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	while (len) {
		in = off & (GUNYAH_VM_POOL_BLOCK - 1);
		chunk = min_t(u64, len, GUNYAH_VM_POOL_BLOCK - in);
		first = pfn_to_page(PHYS_PFN(mem->block[off >> GUNYAH_VM_POOL_SHIFT] + in));
		num = chunk >> PAGE_SHIFT;
		for (i = 0; i < num; i++)
			pages[i] = first + i;
		ret = vm_insert_pages(vma, addr, pages, &num);
		if (ret)
			break;
		addr += chunk;
		off += chunk;
		len -= chunk;
	}

	kfree(pages);
	return ret;
}

static int gunyah_vm_pool_mem_release(struct inode *inode, struct file *file)
{
	struct gunyah_vm_pool_mem *mem = file->private_data;
	unsigned long i;

	if (mem->lent || mem->lost) {
		pr_err("%llu MiB the hypervisor did not give back stay out of the pool until the next boot\n",
		       mem->size >> 20);
	} else {
		for (i = 0; i < mem->nr_blocks; i++)
			gen_pool_free(gunyah_vm_pool->blocks, mem->block[i], GUNYAH_VM_POOL_BLOCK);
	}
	kvfree(mem);
	return 0;
}

static const struct file_operations gunyah_vm_pool_mem_fops = {
	.owner = THIS_MODULE,
	.mmap = gunyah_vm_pool_mem_mmap,
	.release = gunyah_vm_pool_mem_release,
	.llseek = noop_llseek,
};

static long gunyah_vm_pool_create_mem_fd(struct gunyah_vm_pool *pool, void __user *argp)
{
	struct gunyah_vm_pool_mem_args args;
	struct gunyah_vm_pool_mem *mem;
	unsigned long n, i;
	struct inode *inode;
	struct file *file;
	int fd, ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;
	if (args.flags || !args.size || !PAGE_ALIGNED(args.size))
		return -EINVAL;
	if (args.size > gen_pool_avail(pool->blocks)) {
		pr_info("no room for %llu MiB, %zu MiB free: the VM gets ordinary memory\n",
			args.size >> 20, gen_pool_avail(pool->blocks) >> 20);
		return -ENOMEM;
	}

	n = DIV_ROUND_UP(args.size, GUNYAH_VM_POOL_BLOCK);
	mem = kvzalloc(struct_size(mem, block, n), GFP_KERNEL_ACCOUNT);
	if (!mem)
		return -ENOMEM;
	mem->nr_blocks = n;
	mem->size = args.size;
	mutex_init(&mem->lock);

	for (i = 0; i < n; i++) {
		mem->block[i] = gen_pool_alloc(pool->blocks, GUNYAH_VM_POOL_BLOCK);
		if (!mem->block[i]) {
			pr_info("no room for %llu MiB, %zu MiB free: the VM gets ordinary memory\n",
				args.size >> 20, gen_pool_avail(pool->blocks) >> 20);
			ret = -ENOMEM;
			goto free_blocks;
		}
	}

	/* The boot reservation and an earlier VM may have left data behind. */
	for (i = 0; i < n; i++) {
		memset(phys_to_virt(mem->block[i]), 0, GUNYAH_VM_POOL_BLOCK);
		cond_resched();
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto free_blocks;
	}

	file = anon_inode_create_getfile("[gunyah-vm-pool]", &gunyah_vm_pool_mem_fops,
					 mem, O_RDWR, NULL);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto put_fd;
	}
	inode = file_inode(file);
	inode->i_mode |= S_IFREG;
	i_size_write(inode, args.size);
	file->f_flags |= O_LARGEFILE;

	pr_info("%llu MiB for a VM in %zu pieces, %zu MiB left\n", args.size >> 20,
		gunyah_vm_pool_pieces(mem), gen_pool_avail(pool->blocks) >> 20);
	fd_install(fd, file);
	return fd;

put_fd:
	put_unused_fd(fd);
free_blocks:
	while (i--)
		gen_pool_free(pool->blocks, mem->block[i], GUNYAH_VM_POOL_BLOCK);
	kvfree(mem);
	return ret;
}

static long gunyah_vm_pool_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = filp->private_data;
	struct gunyah_vm_pool *pool = container_of(miscdev, struct gunyah_vm_pool, miscdev);

	switch (cmd) {
	case GH_ANDROID_CREATE_POOL_MEM_FD:
		return gunyah_vm_pool_create_mem_fd(pool, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations gunyah_vm_pool_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gunyah_vm_pool_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

/* Takes the reference to @file: dropped on failure, else when the binding goes. */
int gunyah_vm_binding_pool_alloc(struct gunyah_vm *ghvm,
				 struct gunyah_map_cma_mem_args *args, struct file *file)
{
	struct gunyah_vm_pool_mem *mem = file->private_data;
	struct gunyah_vm_binding *binding;
	u64 end;
	int ret;

	ret = -EINVAL;
	if (!args->size || !PAGE_ALIGNED(args->size) || !PAGE_ALIGNED(args->guest_addr) ||
	    !PAGE_ALIGNED(args->offset))
		goto put_file;
	/* Pool memory is lent, never shared with the host. */
	if (!(args->flags & GUNYAH_MEM_FORCE_LEND) || (args->flags & GUNYAH_MEM_FORCE_SHARE))
		goto put_file;
	ret = -EOVERFLOW;
	if (check_add_overflow(args->guest_addr, args->size, &end) ||
	    check_add_overflow(args->offset, args->size, &end) || end > mem->size)
		goto put_file;

	ret = -ENOMEM;
	binding = kzalloc(sizeof(*binding), GFP_KERNEL_ACCOUNT);
	if (!binding)
		goto put_file;

	mutex_lock(&mem->lock);
	if (mem->bound) {
		mutex_unlock(&mem->lock);
		ret = -EBUSY;
		goto free_binding;
	}
	mem->bound = true;
	mutex_unlock(&mem->lock);

	binding->mem_type = VM_MEM_POOL;
	binding->cma.file = file;
	binding->cma.offset = args->offset;
	binding->guest_phys_addr = args->guest_addr;
	binding->label = args->label;
	binding->size = args->size;
	binding->flags = args->flags;
	binding->share_type = VM_MEM_LEND;
	binding->vm_parcel = NULL;

	down_write(&ghvm->bindings_lock);
	ret = mtree_insert_range(&ghvm->bindings,
				 gunyah_gpa_to_gfn(binding->guest_phys_addr),
				 gunyah_gpa_to_gfn(binding->guest_phys_addr + args->size - 1),
				 binding, GFP_KERNEL);
	up_write(&ghvm->bindings_lock);
	if (!ret)
		return 0;

	mutex_lock(&mem->lock);
	mem->bound = false;
	mutex_unlock(&mem->lock);
free_binding:
	kfree(binding);
put_file:
	fput(file);
	return ret;
}

/* Called with the VM's bindings_lock held for writing. */
int gunyah_vm_pool_share_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
				struct gunyah_vm_binding *b, u64 *gfn, u64 *nr)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct gunyah_vm_pool_mem *mem = b->cma.file->private_data;
	struct gunyah_rm_mem_entry *e;
	u64 rel, off, end, pos, in, chunk;
	phys_addr_t pa;
	size_t n = 0;
	int ret;

	/* Only a protected VM gets pool memory. */
	if (!ghvm->is_protected || b->share_type != VM_MEM_LEND)
		return -EPERM;

	rel = gunyah_gfn_to_gpa(*gfn) - b->guest_phys_addr;
	if (gunyah_gfn_to_gpa(*gfn) < b->guest_phys_addr || rel > b->size ||
	    gunyah_gfn_to_gpa(*nr) > b->size - rel)
		return -EINVAL;
	off = b->cma.offset + rel;
	end = off + gunyah_gfn_to_gpa(*nr);
	if (end > mem->size)
		return -EINVAL;

	parcel->mem_entries = kvcalloc(DIV_ROUND_UP(end - off, GUNYAH_VM_POOL_BLOCK) + 1,
				       sizeof(parcel->mem_entries[0]), GFP_KERNEL_ACCOUNT);
	if (!parcel->mem_entries)
		return -ENOMEM;

	/* One entry for each run of physically adjacent blocks. */
	for (pos = off; pos < end; pos += chunk) {
		in = pos & (GUNYAH_VM_POOL_BLOCK - 1);
		chunk = min_t(u64, end - pos, GUNYAH_VM_POOL_BLOCK - in);
		pa = mem->block[pos >> GUNYAH_VM_POOL_SHIFT] + in;
		e = n ? &parcel->mem_entries[n - 1] : NULL;
		if (e && le64_to_cpu(e->phys_addr) + le64_to_cpu(e->size) == pa) {
			e->size = cpu_to_le64(le64_to_cpu(e->size) + chunk);
		} else {
			parcel->mem_entries[n].phys_addr = cpu_to_le64(pa);
			parcel->mem_entries[n].size = cpu_to_le64(chunk);
			n++;
		}
	}
	parcel->n_mem_entries = n;

	if (ghvm->lent_pieces + n > GUNYAH_LEND_MAX_PIECES) {
		dev_warn(ghvm->parent,
			 "VM memory at gfn %llx is in %zu pieces, %zu for the whole VM, more than %d: not lent\n",
			 *gfn, n, ghvm->lent_pieces + n, GUNYAH_LEND_MAX_PIECES);
		ret = -E2BIG;
		goto free_mem_entries;
	}

	mutex_lock(&mem->lock);
	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	if (!ret)
		mem->lent++;
	mutex_unlock(&mem->lock);
	dev_info(ghvm->parent, "lend gfn %llx from the pool: %llu pages in %zu pieces, %zu for the whole VM, ret %d\n",
		 *gfn, *nr, n, ghvm->lent_pieces + (ret ? 0 : n), ret);
	if (ret)
		goto free_mem_entries;
	ghvm->lent_pieces += n;

	vm_parcel->start = *gfn;
	vm_parcel->pages = *nr;
	b->vm_parcel = vm_parcel;
	return 0;

free_mem_entries:
	kvfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
	return ret;
}

/* Called with the VM's bindings_lock held for writing. */
int gunyah_vm_pool_reclaim_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
				  struct gunyah_vm_binding *b)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct gunyah_vm_pool_mem *mem = b->cma.file->private_data;
	int ret;

	if (parcel->mem_handle == GUNYAH_MEM_HANDLE_INVAL)
		return 0;

	mutex_lock(&mem->lock);
	ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel);
	if (ret)
		/* The hypervisor may still hold the memory: never hand it out again. */
		mem->lost = true;
	else
		mem->lent--;
	mutex_unlock(&mem->lock);
	if (ret) {
		dev_err(ghvm->parent, "Failed to reclaim pool memory: %d\n", ret);
		return ret;
	}

	ghvm->lent_pieces -= min_t(u32, ghvm->lent_pieces, parcel->n_mem_entries);
	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
	kvfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
	kfree(parcel->acl_entries);
	parcel->acl_entries = NULL;
	vm_parcel->start = 0;
	vm_parcel->pages = 0;
	b->vm_parcel = NULL;
	return 0;
}

int gunyah_vm_pool_init(void)
{
	struct gunyah_vm_pool *pool;
	phys_addr_t base, size;
	int ret;

	if (!reserve_mem_find_by_name(GUNYAH_VM_POOL_NAME, &base, &size))
		return 0;
	if (!IS_ALIGNED(base, GUNYAH_VM_POOL_BLOCK) || size < GUNYAH_VM_POOL_BLOCK) {
		pr_err("%pa bytes at %pa: not in 2 MiB blocks, no pool\n", &size, &base);
		return -EINVAL;
	}
	size = ALIGN_DOWN(size, GUNYAH_VM_POOL_BLOCK);

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	pool->base = base;
	pool->size = size;
	pool->blocks = gen_pool_create(ilog2(GUNYAH_VM_POOL_BLOCK), NUMA_NO_NODE);
	if (!pool->blocks) {
		ret = -ENOMEM;
		goto free_pool;
	}
	ret = gen_pool_add(pool->blocks, base, size, NUMA_NO_NODE);
	if (ret)
		goto destroy;

	pool->miscdev.name = GUNYAH_VM_POOL_NAME;
	pool->miscdev.minor = MISC_DYNAMIC_MINOR;
	pool->miscdev.fops = &gunyah_vm_pool_dev_fops;
	ret = misc_register(&pool->miscdev);
	if (ret)
		goto destroy;

	gunyah_vm_pool = pool;
	pr_info("%llu MiB at %pa for protected VM memory\n", (u64)size >> 20, &base);
	return 0;

destroy:
	gen_pool_destroy(pool->blocks);
free_pool:
	kfree(pool);
	return ret;
}

void gunyah_vm_pool_exit(void)
{
	struct gunyah_vm_pool *pool = gunyah_vm_pool;

	if (!pool)
		return;
	misc_deregister(&pool->miscdev);
	/* Files still open keep their blocks, so the pool stays allocated. */
}
