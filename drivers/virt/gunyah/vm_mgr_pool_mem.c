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
 * GH_VM_ANDROID_MAP_CMA_MEM, only to a protected VM. The VM's RAM and firmware
 * region are lent: the host loses access. Its bounce buffers are shared: the
 * host keeps access, as with ordinary memory. One open of the node makes at
 * most one file of at most GUNYAH_VM_POOL_MAX_FILE bytes, and a file is bound
 * to one VM once. When the file is released the blocks are zeroed and go back
 * to the pool, unless the hypervisor may still hold them: a parcel not
 * reclaimed, or a lend, share, rollback or reclaim whose outcome is unknown
 * (enum gunyah_rm_mem_owner). Such blocks stay out of use, unzeroed, until the
 * next boot and are counted in lost_bytes, next to size_bytes and free_bytes
 * in /sys/class/misc/gunyah_vm_pool/. Ordinary memory in the same state stays
 * pinned for good and is counted in ordinary_lost_bytes there.
 *
 * The same directory has the piece counts of all VMs together: vm_pieces, the
 * physically separate pieces the hypervisor holds or may hold now in parcels
 * lent or shared to any VM, pool or ordinary memory, and vm_pieces_max, the
 * most it may hold. vm_pieces_uncertain is the part of vm_pieces in parcels
 * whose owner is unknown or that outlived their VM: no reclaim can confirm
 * them later, so they stay counted until the next boot. Root may change
 * vm_pieces_max, for tests.
 */

#define pr_fmt(fmt) "gunyah_vm_pool: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/genalloc.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "vm_mgr_pool.h"

#define GUNYAH_VM_POOL_NAME		"gunyah_vm_pool"
#define GUNYAH_VM_POOL_SHIFT		21
#define GUNYAH_VM_POOL_BLOCK		BIT_ULL(GUNYAH_VM_POOL_SHIFT)
#define GUNYAH_VM_POOL_BLOCK_PAGES	(GUNYAH_VM_POOL_BLOCK >> PAGE_SHIFT)
/* The largest VM the pool is sized for: the decode service VM. */
#define GUNYAH_VM_POOL_MAX_FILE		(SZ_256M + SZ_32M)

/*
 * The pieces all VMs together may hold by default. The FP6 resource manager
 * (firmware 16.115.0) refuses VM_START (RM error 2, crosvm ENODEV) once the
 * parcels lent and shared to all running VMs pass a budget, whichever VM's
 * start passes it. Beside the decode service's VM, which holds 700 to 1500
 * pieces itself, test VMs of 15778 + 3397 pieces started and 8431 + 13216 were
 * refused, so the budget is about 19900 to 22300 in all. 19000 in all, the
 * decode service's VM counted, stays below every total that started. A lend or
 * share that would pass the limit is refused here before it is sent, so the
 * start fails at once with E2BIG. Another firmware may need another value:
 * gunyah_rsc_mgr.vm_pieces_max= on the kernel command line sets it at boot,
 * root may change it in vm_pieces_max, and everything that plans VM starts
 * reads it there.
 */
#define GUNYAH_VM_PIECES_MAX		19000

struct gunyah_vm_pool {
	struct miscdevice miscdev;
	struct gen_pool *blocks;
	phys_addr_t base;
	phys_addr_t size;
	/* Makes the room check and the allocation of a file's blocks one step. */
	struct mutex lock;
	/* Bytes the hypervisor may still hold, out of use until the next boot. */
	atomic64_t lost;
};

/* One open of /dev/gunyah_vm_pool. */
struct gunyah_vm_pool_client {
	struct gunyah_vm_pool *pool;
	bool used;
};

static struct gunyah_vm_pool *gunyah_vm_pool;

/*
 * The pieces of all VMs' parcels the hypervisor holds or may hold now, the part
 * of them no VM holds any more, and the most it may hold.
 */
static DEFINE_SPINLOCK(gunyah_vm_pieces_lock);
static u32 gunyah_vm_pieces;
static u32 gunyah_vm_pieces_lost;
static u32 gunyah_vm_pieces_max = GUNYAH_VM_PIECES_MAX;
/* Bytes of ordinary memory kept pinned for good: the hypervisor may hold them. */
static atomic64_t gunyah_vm_ordinary_lost_bytes = ATOMIC64_INIT(0);
/* Only the boot value: the pool's vm_pieces_max file reads and changes it later. */
module_param_named(vm_pieces_max, gunyah_vm_pieces_max, uint, 0);
MODULE_PARM_DESC(vm_pieces_max, "Pieces all VMs together may hold (default 19000)");

/*
 * Counts a parcel of @n pieces at @gfn before it is lent or shared, or refuses
 * it with -E2BIG when the VM or all VMs together would hold too many. Called
 * with the VM's bindings_lock held for writing. A parcel the hypervisor did
 * not take is given back with gunyah_vm_pieces_give(), so is a reclaimed one.
 */
int gunyah_vm_pieces_take(struct gunyah_vm *ghvm, u64 gfn, size_t n)
{
	u32 all, max;
	bool fits;

	if (ghvm->lent_pieces + n > GUNYAH_LEND_MAX_PIECES) {
		dev_warn(ghvm->parent,
			 "VM memory at gfn %llx is in %zu pieces, %zu for the whole VM, more than %d: not lent\n",
			 gfn, n, ghvm->lent_pieces + n, GUNYAH_LEND_MAX_PIECES);
		return -E2BIG;
	}

	spin_lock(&gunyah_vm_pieces_lock);
	all = gunyah_vm_pieces;
	max = gunyah_vm_pieces_max;
	fits = n <= max && all <= max - n;
	if (fits)
		WRITE_ONCE(gunyah_vm_pieces, all + n);
	spin_unlock(&gunyah_vm_pieces_lock);
	if (!fits) {
		dev_warn(ghvm->parent,
			 "VM memory at gfn %llx is in %zu pieces, %llu for all VMs, more than %u: not lent\n",
			 gfn, n, (u64)all + n, max);
		return -E2BIG;
	}

	ghvm->lent_pieces += n;
	return 0;
}

void gunyah_vm_pieces_give(struct gunyah_vm *ghvm, size_t n)
{
	n = min_t(size_t, n, ghvm->lent_pieces);
	ghvm->lent_pieces -= n;
	spin_lock(&gunyah_vm_pieces_lock);
	WRITE_ONCE(gunyah_vm_pieces, gunyah_vm_pieces - min_t(u32, gunyah_vm_pieces, n));
	spin_unlock(&gunyah_vm_pieces_lock);
}

u32 gunyah_vm_pieces_all(void)
{
	return READ_ONCE(gunyah_vm_pieces);
}

/*
 * @n of the VM's pieces are in parcels whose owner is unknown or that outlive
 * the VM. They stay in the count of all VMs, charged to none, until the next
 * boot: nothing can confirm their return.
 */
void gunyah_vm_pieces_uncertain(struct gunyah_vm *ghvm, size_t n)
{
	n = min_t(size_t, n, ghvm->lent_pieces);
	if (!n)
		return;
	ghvm->lent_pieces -= n;
	spin_lock(&gunyah_vm_pieces_lock);
	WRITE_ONCE(gunyah_vm_pieces_lost, gunyah_vm_pieces_lost + n);
	spin_unlock(&gunyah_vm_pieces_lock);
	dev_err(ghvm->parent, "%zu pieces the hypervisor may hold stay counted, %u in all\n", n,
		READ_ONCE(gunyah_vm_pieces_lost));
}

void gunyah_vm_ordinary_lost(u64 bytes)
{
	atomic64_add(bytes, &gunyah_vm_ordinary_lost_bytes);
}

/* The memory of one VM: a file made by GH_ANDROID_CREATE_POOL_MEM_FD. */
struct gunyah_vm_pool_mem {
	struct mutex lock;
	u64 size;
	/* Bound to a VM. A file is bound once, so its blocks are lent to one VM only. */
	bool bound;
	/* Parcels of this file the resource manager holds now. */
	unsigned int lent;
	/* The hypervisor may still hold blocks: an outcome was unknown (enum gunyah_rm_mem_owner). */
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
	struct gunyah_vm_pool *pool = gunyah_vm_pool;
	u64 bytes = (u64)mem->nr_blocks * GUNYAH_VM_POOL_BLOCK;
	unsigned long i;

	if (mem->lent || mem->lost) {
		pr_err("%llu MiB the hypervisor may still hold stay out of the pool until the next boot, %lld MiB lost in all\n",
		       bytes >> 20, atomic64_add_return(bytes, &pool->lost) >> 20);
	} else {
		/* No VM's data stays behind in the pool. */
		for (i = 0; i < mem->nr_blocks; i++) {
			memset(phys_to_virt(mem->block[i]), 0, GUNYAH_VM_POOL_BLOCK);
			cond_resched();
		}
		mutex_lock(&pool->lock);
		for (i = 0; i < mem->nr_blocks; i++)
			gen_pool_free(pool->blocks, mem->block[i], GUNYAH_VM_POOL_BLOCK);
		mutex_unlock(&pool->lock);
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

static long gunyah_vm_pool_create_mem_fd(struct gunyah_vm_pool_client *client, void __user *argp)
{
	struct gunyah_vm_pool *pool = client->pool;
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
	if (args.size > GUNYAH_VM_POOL_MAX_FILE)
		return -EFBIG;
	/* One file per open: a VM manager opens the node for each VM. */
	mutex_lock(&pool->lock);
	ret = client->used ? -EBUSY : 0;
	client->used = true;
	mutex_unlock(&pool->lock);
	if (ret)
		return ret;

	n = DIV_ROUND_UP(args.size, GUNYAH_VM_POOL_BLOCK);
	mem = kvzalloc(struct_size(mem, block, n), GFP_KERNEL_ACCOUNT);
	if (!mem)
		return -ENOMEM;
	mem->nr_blocks = n;
	mem->size = args.size;
	mutex_init(&mem->lock);

	/* All or nothing, in arrival order. */
	mutex_lock(&pool->lock);
	if ((u64)n * GUNYAH_VM_POOL_BLOCK > gen_pool_avail(pool->blocks)) {
		pr_info("no room for %llu MiB, %zu MiB free: the VM gets ordinary memory\n",
			args.size >> 20, gen_pool_avail(pool->blocks) >> 20);
		mutex_unlock(&pool->lock);
		kvfree(mem);
		return -ENOMEM;
	}
	for (i = 0; i < n; i++) {
		mem->block[i] = gen_pool_alloc(pool->blocks, GUNYAH_VM_POOL_BLOCK);
		/* Cannot happen with the room checked under the lock. */
		if (WARN_ON_ONCE(!mem->block[i])) {
			while (i--)
				gen_pool_free(pool->blocks, mem->block[i], GUNYAH_VM_POOL_BLOCK);
			mutex_unlock(&pool->lock);
			kvfree(mem);
			return -ENOMEM;
		}
	}
	mutex_unlock(&pool->lock);

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
	/* Never lent, already zeroed. */
	mutex_lock(&pool->lock);
	while (i--)
		gen_pool_free(pool->blocks, mem->block[i], GUNYAH_VM_POOL_BLOCK);
	mutex_unlock(&pool->lock);
	kvfree(mem);
	return ret;
}

static int gunyah_vm_pool_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct gunyah_vm_pool_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL_ACCOUNT);
	if (!client)
		return -ENOMEM;
	client->pool = container_of(miscdev, struct gunyah_vm_pool, miscdev);
	filp->private_data = client;
	return 0;
}

static int gunyah_vm_pool_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static long gunyah_vm_pool_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gunyah_vm_pool_client *client = filp->private_data;

	switch (cmd) {
	case GH_ANDROID_CREATE_POOL_MEM_FD:
		return gunyah_vm_pool_create_mem_fd(client, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static ssize_t size_bytes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (u64)gunyah_vm_pool->size);
}
static DEVICE_ATTR_RO(size_bytes);

static ssize_t free_bytes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%zu\n", gen_pool_avail(gunyah_vm_pool->blocks));
}
static DEVICE_ATTR_RO(free_bytes);

static ssize_t lost_bytes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&gunyah_vm_pool->lost));
}
static DEVICE_ATTR_RO(lost_bytes);

static ssize_t vm_pieces_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(gunyah_vm_pieces));
}
static DEVICE_ATTR_RO(vm_pieces);

static ssize_t vm_pieces_uncertain_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(gunyah_vm_pieces_lost));
}
static DEVICE_ATTR_RO(vm_pieces_uncertain);

static ssize_t ordinary_lost_bytes_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&gunyah_vm_ordinary_lost_bytes));
}
static DEVICE_ATTR_RO(ordinary_lost_bytes);

static ssize_t vm_pieces_max_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(gunyah_vm_pieces_max));
}

static ssize_t vm_pieces_max_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	u32 max;
	int ret;

	ret = kstrtou32(buf, 10, &max);
	if (ret)
		return ret;
	spin_lock(&gunyah_vm_pieces_lock);
	WRITE_ONCE(gunyah_vm_pieces_max, max);
	spin_unlock(&gunyah_vm_pieces_lock);
	pr_info("all VMs may hold %u pieces (default %u)\n", max, GUNYAH_VM_PIECES_MAX);
	return count;
}
static DEVICE_ATTR_RW(vm_pieces_max);

static struct attribute *gunyah_vm_pool_attrs[] = {
	&dev_attr_size_bytes.attr,
	&dev_attr_free_bytes.attr,
	&dev_attr_lost_bytes.attr,
	&dev_attr_ordinary_lost_bytes.attr,
	&dev_attr_vm_pieces.attr,
	&dev_attr_vm_pieces_uncertain.attr,
	&dev_attr_vm_pieces_max.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gunyah_vm_pool);

static const struct file_operations gunyah_vm_pool_dev_fops = {
	.owner = THIS_MODULE,
	.open = gunyah_vm_pool_open,
	.release = gunyah_vm_pool_release,
	.unlocked_ioctl = gunyah_vm_pool_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

/* Takes the reference to @file: dropped on failure, else when the binding goes. */
int gunyah_vm_binding_pool_alloc(struct gunyah_vm *ghvm,
				 struct gunyah_map_cma_mem_args *args, struct file *file)
{
	struct gunyah_vm_pool_mem *mem = file->private_data;
	bool share = args->flags & GUNYAH_MEM_FORCE_SHARE;
	struct gunyah_vm_binding *binding;
	u64 end;
	int ret;

	ret = -EINVAL;
	if (!args->size || !PAGE_ALIGNED(args->size) || !PAGE_ALIGNED(args->guest_addr) ||
	    !PAGE_ALIGNED(args->offset))
		goto put_file;
	/* Lent (RAM, firmware) or shared with the host (bounce buffers), said explicitly. */
	if (share == !!(args->flags & GUNYAH_MEM_FORCE_LEND))
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
	binding->share_type = share ? VM_MEM_SHARE : VM_MEM_LEND;
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
	const char *how = b->share_type == VM_MEM_LEND ? "lend" : "share";
	enum gunyah_rm_mem_owner owner = GUNYAH_RM_MEM_UNKNOWN;
	struct gunyah_rm_mem_entry *e;
	u64 rel, off, end, pos, in, chunk;
	phys_addr_t pa;
	size_t n = 0;
	int ret;

	/* DiamaneOS: quarantine applies to the file as well as its VM binding. */
	mutex_lock(&mem->lock);
	ret = mem->lost || mem->lent || b->quarantined ? -EBUSY : 0;
	mutex_unlock(&mem->lock);
	if (ret)
		return ret;

	/* Only a protected VM gets pool memory. */
	if (!ghvm->is_protected)
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

	ret = gunyah_vm_pieces_take(ghvm, *gfn, n);
	if (ret)
		goto free_mem_entries;

	mutex_lock(&mem->lock);
	ret = gunyah_rm_mem_share(ghvm->rm, parcel, &owner);
	if (owner == GUNYAH_RM_MEM_LENT)
		mem->lent++;
	else if (owner == GUNYAH_RM_MEM_UNKNOWN) {
		mem->lost = true;
		b->quarantined = true;
	}
	mutex_unlock(&mem->lock);
	if (owner == GUNYAH_RM_MEM_HOST)
		gunyah_vm_pieces_give(ghvm, n);
	else if (owner == GUNYAH_RM_MEM_UNKNOWN)
		gunyah_vm_pieces_uncertain(ghvm, n);
	if (ret && owner != GUNYAH_RM_MEM_HOST)
		dev_err(ghvm->parent, "%s of %llu MiB from the pool ended with %d, the hypervisor %s it: the blocks are quarantined\n",
			how, gunyah_gfn_to_gpa(*nr) >> 20, ret,
			owner == GUNYAH_RM_MEM_LENT ? "holds" : "may hold");
	dev_info(ghvm->parent, "%s gfn %llx from the pool: %llu pages in %zu pieces, %u for the whole VM, %u for all VMs, ret %d\n",
		 how, *gfn, *nr, n, ghvm->lent_pieces, gunyah_vm_pieces_all(), ret);
	/* A parcel the resource manager holds stays bound, also after a failure, for its reclaim. */
	if (owner != GUNYAH_RM_MEM_LENT)
		goto free_mem_entries;

	vm_parcel->start = *gfn;
	vm_parcel->pages = *nr;
	b->vm_parcel = vm_parcel;
	return ret;

free_mem_entries:
	kvfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
	return ret;
}

/* DiamaneOS: retain the pool backing after its parcel record goes away. */
void gunyah_vm_pool_quarantine(struct gunyah_vm_binding *b)
{
	struct gunyah_vm_pool_mem *mem = b->cma.file->private_data;

	mutex_lock(&mem->lock);
	mem->lost = true;
	mutex_unlock(&mem->lock);
}

/* Called with the VM's bindings_lock held for writing. */
int gunyah_vm_pool_reclaim_parcel(struct gunyah_vm *ghvm, struct gunyah_vm_parcel *vm_parcel,
				  struct gunyah_vm_binding *b)
{
	struct gunyah_rm_mem_parcel *parcel = &vm_parcel->parcel;
	struct gunyah_vm_pool_mem *mem = b->cma.file->private_data;
	enum gunyah_rm_mem_owner owner = GUNYAH_RM_MEM_UNKNOWN;
	int ret;

	if (parcel->mem_handle == GUNYAH_MEM_HANDLE_INVAL)
		return 0;

	/*
	 * A refused reclaim leaves the parcel lent, for a later reclaim. Once
	 * its owner is unknown, the blocks are never handed out again.
	 */
	mutex_lock(&mem->lock);
	ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel, &owner);
	if (!ret)
		mem->lent--;
	else if (owner == GUNYAH_RM_MEM_UNKNOWN) {
		mem->lost = true;
		b->quarantined = true;
	}
	mutex_unlock(&mem->lock);
	if (ret) {
		dev_err(ghvm->parent, "Failed to reclaim pool memory: %d\n", ret);
		return ret;
	}

	gunyah_vm_pieces_give(ghvm, parcel->n_mem_entries);
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
	pool->miscdev.groups = gunyah_vm_pool_groups;
	mutex_init(&pool->lock);
	atomic64_set(&pool->lost, 0);
	/* Set before the node and its sysfs files appear, which read it. */
	gunyah_vm_pool = pool;
	ret = misc_register(&pool->miscdev);
	if (ret) {
		gunyah_vm_pool = NULL;
		goto destroy;
	}

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
