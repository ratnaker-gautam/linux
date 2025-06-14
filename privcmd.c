// SPDX-License-Identifier: GPL-2.0-only
/******************************************************************************
 * privcmd.c
 *
 * Interface to privileged domain-0 commands.
 *
 * Copyright (c) 2002-2004, K A Fraser, B Dragovic
 */

#define pr_fmt(fmt) "xen:" KBUILD_MODNAME ": " fmt

#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/miscdevice.h>
#include <linux/moduleparam.h>
#include <linux/virtio_mmio.h>

#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>

#include <xen/xen.h>
#include <xen/events.h>
#include <xen/privcmd.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <xen/interface/hvm/dm_op.h>
#include <xen/interface/hvm/ioreq.h>
#include <xen/features.h>
#include <xen/page.h>
#include <xen/xen-ops.h>
#include <xen/balloon.h>

#include "privcmd.h"

MODULE_DESCRIPTION("Xen hypercall passthrough driver");
MODULE_LICENSE("GPL");

#define PRIV_VMA_LOCKED ((void *)1)

static unsigned int privcmd_dm_op_max_num = 16;
module_param_named(dm_op_max_nr_bufs, privcmd_dm_op_max_num, uint, 0644);
MODULE_PARM_DESC(dm_op_max_nr_bufs,
		 "Maximum number of buffers per dm_op hypercall");

static unsigned int privcmd_dm_op_buf_max_size = 4096;
module_param_named(dm_op_buf_max_size, privcmd_dm_op_buf_max_size, uint,
		   0644);
MODULE_PARM_DESC(dm_op_buf_max_size,
		 "Maximum size of a dm_op hypercall buffer");

struct privcmd_data {
	domid_t domid;
};

static int privcmd_vma_range_is_mapped(
               struct vm_area_struct *vma,
               unsigned long addr,
               unsigned long nr_pages);

static long privcmd_ioctl_hypercall(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct privcmd_hypercall hypercall;
	long ret;

	/* Disallow arbitrary hypercalls if restricted */
	if (data->domid != DOMID_INVALID)
		return -EPERM;

	if (copy_from_user(&hypercall, udata, sizeof(hypercall)))
		return -EFAULT;

	xen_preemptible_hcall_begin();
	ret = privcmd_call(hypercall.op,
			   hypercall.arg[0], hypercall.arg[1],
			   hypercall.arg[2], hypercall.arg[3],
			   hypercall.arg[4]);
	xen_preemptible_hcall_end();

	return ret;
}

static void free_page_list(struct list_head *pages)
{
	struct page *p, *n;

	list_for_each_entry_safe(p, n, pages, lru)
		__free_page(p);

	INIT_LIST_HEAD(pages);
}

/*
 * Given an array of items in userspace, return a list of pages
 * containing the data.  If copying fails, either because of memory
 * allocation failure or a problem reading user memory, return an
 * error code; its up to the caller to dispose of any partial list.
 */
static int gather_array(struct list_head *pagelist,
			unsigned nelem, size_t size,
			const void __user *data)
{
	unsigned pageidx;
	void *pagedata;
	int ret;

	if (size > PAGE_SIZE)
		return 0;

	pageidx = PAGE_SIZE;
	pagedata = NULL;	/* quiet, gcc */
	while (nelem--) {
		if (pageidx > PAGE_SIZE-size) {
			struct page *page = alloc_page(GFP_KERNEL);

			ret = -ENOMEM;
			if (page == NULL)
				goto fail;

			pagedata = page_address(page);

			list_add_tail(&page->lru, pagelist);
			pageidx = 0;
		}

		ret = -EFAULT;
		if (copy_from_user(pagedata + pageidx, data, size))
			goto fail;

		data += size;
		pageidx += size;
	}

	ret = 0;

fail:
	return ret;
}

/*
 * Call function "fn" on each element of the array fragmented
 * over a list of pages.
 */
static int traverse_pages(unsigned nelem, size_t size,
			  struct list_head *pos,
			  int (*fn)(void *data, void *state),
			  void *state)
{
	void *pagedata;
	unsigned pageidx;
	int ret = 0;

	BUG_ON(size > PAGE_SIZE);

	pageidx = PAGE_SIZE;
	pagedata = NULL;	/* hush, gcc */

	while (nelem--) {
		if (pageidx > PAGE_SIZE-size) {
			struct page *page;
			pos = pos->next;
			page = list_entry(pos, struct page, lru);
			pagedata = page_address(page);
			pageidx = 0;
		}

		ret = (*fn)(pagedata + pageidx, state);
		if (ret)
			break;
		pageidx += size;
	}

	return ret;
}

/*
 * Similar to traverse_pages, but use each page as a "block" of
 * data to be processed as one unit.
 */
static int traverse_pages_block(unsigned nelem, size_t size,
				struct list_head *pos,
				int (*fn)(void *data, int nr, void *state),
				void *state)
{
	void *pagedata;
	int ret = 0;

	BUG_ON(size > PAGE_SIZE);

	while (nelem) {
		int nr = (PAGE_SIZE/size);
		struct page *page;
		if (nr > nelem)
			nr = nelem;
		pos = pos->next;
		page = list_entry(pos, struct page, lru);
		pagedata = page_address(page);
		ret = (*fn)(pagedata, nr, state);
		if (ret)
			break;
		nelem -= nr;
	}

	return ret;
}

struct mmap_gfn_state {
	unsigned long va;
	struct vm_area_struct *vma;
	domid_t domain;
};

static int mmap_gfn_range(void *data, void *state)
{
	struct privcmd_mmap_entry *msg = data;
	struct mmap_gfn_state *st = state;
	struct vm_area_struct *vma = st->vma;
	int rc;

	/* Do not allow range to wrap the address space. */
	if ((msg->npages > (LONG_MAX >> PAGE_SHIFT)) ||
	    ((unsigned long)(msg->npages << PAGE_SHIFT) >= -st->va))
		return -EINVAL;

	/* Range chunks must be contiguous in va space. */
	if ((msg->va != st->va) ||
	    ((msg->va+(msg->npages<<PAGE_SHIFT)) > vma->vm_end))
		return -EINVAL;

	rc = xen_remap_domain_gfn_range(vma,
					msg->va & PAGE_MASK,
					msg->mfn, msg->npages,
					vma->vm_page_prot,
					st->domain, NULL);
	if (rc < 0)
		return rc;

	st->va += msg->npages << PAGE_SHIFT;

	return 0;
}

static long privcmd_ioctl_mmap(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct privcmd_mmap mmapcmd;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int rc;
	LIST_HEAD(pagelist);
	struct mmap_gfn_state state;

	/* We only support privcmd_ioctl_mmap_batch for non-auto-translated. */
	if (xen_feature(XENFEAT_auto_translated_physmap))
		return -ENOSYS;

	if (copy_from_user(&mmapcmd, udata, sizeof(mmapcmd)))
		return -EFAULT;

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != mmapcmd.dom)
		return -EPERM;

	rc = gather_array(&pagelist,
			  mmapcmd.num, sizeof(struct privcmd_mmap_entry),
			  mmapcmd.entry);

	if (rc || list_empty(&pagelist))
		goto out;

	mmap_write_lock(mm);

	{
		struct page *page = list_first_entry(&pagelist,
						     struct page, lru);
		struct privcmd_mmap_entry *msg = page_address(page);

		vma = vma_lookup(mm, msg->va);
		rc = -EINVAL;

		if (!vma || (msg->va != vma->vm_start) || vma->vm_private_data)
			goto out_up;
		vma->vm_private_data = PRIV_VMA_LOCKED;
	}

	state.va = vma->vm_start;
	state.vma = vma;
	state.domain = mmapcmd.dom;

	rc = traverse_pages(mmapcmd.num, sizeof(struct privcmd_mmap_entry),
			    &pagelist,
			    mmap_gfn_range, &state);


out_up:
	mmap_write_unlock(mm);

out:
	free_page_list(&pagelist);

	return rc;
}

struct mmap_batch_state {
	domid_t domain;
	unsigned long va;
	struct vm_area_struct *vma;
	int index;
	/* A tristate:
	 *      0 for no errors
	 *      1 if at least one error has happened (and no
	 *          -ENOENT errors have happened)
	 *      -ENOENT if at least 1 -ENOENT has happened.
	 */
	int global_error;
	int version;

	/* User-space gfn array to store errors in the second pass for V1. */
	xen_pfn_t __user *user_gfn;
	/* User-space int array to store errors in the second pass for V2. */
	int __user *user_err;
};

/* auto translated dom0 note: if domU being created is PV, then gfn is
 * mfn(addr on bus). If it's auto xlated, then gfn is pfn (input to HAP).
 */
static int mmap_batch_fn(void *data, int nr, void *state)
{
	xen_pfn_t *gfnp = data;
	struct mmap_batch_state *st = state;
	struct vm_area_struct *vma = st->vma;
	struct page **pages = vma->vm_private_data;
	struct page **cur_pages = NULL;
	int ret;

	if (xen_feature(XENFEAT_auto_translated_physmap))
		cur_pages = &pages[st->index];

	BUG_ON(nr < 0);
	ret = xen_remap_domain_gfn_array(st->vma, st->va & PAGE_MASK, gfnp, nr,
					 (int *)gfnp, st->vma->vm_page_prot,
					 st->domain, cur_pages);

	/* Adjust the global_error? */
	if (ret != nr) {
		if (ret == -ENOENT)
			st->global_error = -ENOENT;
		else {
			/* Record that at least one error has happened. */
			if (st->global_error == 0)
				st->global_error = 1;
		}
	}
	st->va += XEN_PAGE_SIZE * nr;
	st->index += nr / XEN_PFN_PER_PAGE;

	return 0;
}

static int mmap_return_error(int err, struct mmap_batch_state *st)
{
	int ret;

	if (st->version == 1) {
		if (err) {
			xen_pfn_t gfn;

			ret = get_user(gfn, st->user_gfn);
			if (ret < 0)
				return ret;
			/*
			 * V1 encodes the error codes in the 32bit top
			 * nibble of the gfn (with its known
			 * limitations vis-a-vis 64 bit callers).
			 */
			gfn |= (err == -ENOENT) ?
				PRIVCMD_MMAPBATCH_PAGED_ERROR :
				PRIVCMD_MMAPBATCH_MFN_ERROR;
			return __put_user(gfn, st->user_gfn++);
		} else
			st->user_gfn++;
	} else { /* st->version == 2 */
		if (err)
			return __put_user(err, st->user_err++);
		else
			st->user_err++;
	}

	return 0;
}

static int mmap_return_errors(void *data, int nr, void *state)
{
	struct mmap_batch_state *st = state;
	int *errs = data;
	int i;
	int ret;

	for (i = 0; i < nr; i++) {
		ret = mmap_return_error(errs[i], st);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Allocate pfns that are then mapped with gfns from foreign domid. Update
 * the vma with the page info to use later.
 * Returns: 0 if success, otherwise -errno
 */
static int alloc_empty_pages(struct vm_area_struct *vma, int numpgs)
{
	int rc;
	struct page **pages;

	pages = kvcalloc(numpgs, sizeof(pages[0]), GFP_KERNEL);
	if (pages == NULL)
		return -ENOMEM;

	rc = xen_alloc_unpopulated_pages(numpgs, pages);
	if (rc != 0) {
		pr_warn("%s Could not alloc %d pfns rc:%d\n", __func__,
			numpgs, rc);
		kvfree(pages);
		return -ENOMEM;
	}
	BUG_ON(vma->vm_private_data != NULL);
	vma->vm_private_data = pages;

	return 0;
}

static const struct vm_operations_struct privcmd_vm_ops;

static long privcmd_ioctl_mmap_batch(
	struct file *file, void __user *udata, int version)
{
	struct privcmd_data *data = file->private_data;
	int ret;
	struct privcmd_mmapbatch_v2 m;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long nr_pages;
	LIST_HEAD(pagelist);
	struct mmap_batch_state state;

	switch (version) {
	case 1:
		if (copy_from_user(&m, udata, sizeof(struct privcmd_mmapbatch)))
			return -EFAULT;
		/* Returns per-frame error in m.arr. */
		m.err = NULL;
		if (!access_ok(m.arr, m.num * sizeof(*m.arr)))
			return -EFAULT;
		break;
	case 2:
		if (copy_from_user(&m, udata, sizeof(struct privcmd_mmapbatch_v2)))
			return -EFAULT;
		/* Returns per-frame error code in m.err. */
		if (!access_ok(m.err, m.num * (sizeof(*m.err))))
			return -EFAULT;
		break;
	default:
		return -EINVAL;
	}

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != m.dom)
		return -EPERM;

	nr_pages = DIV_ROUND_UP(m.num, XEN_PFN_PER_PAGE);
	if ((m.num <= 0) || (nr_pages > (LONG_MAX >> PAGE_SHIFT)))
		return -EINVAL;

	ret = gather_array(&pagelist, m.num, sizeof(xen_pfn_t), m.arr);

	if (ret)
		goto out;
	if (list_empty(&pagelist)) {
		ret = -EINVAL;
		goto out;
	}

	if (version == 2) {
		/* Zero error array now to only copy back actual errors. */
		if (clear_user(m.err, sizeof(int) * m.num)) {
			ret = -EFAULT;
			goto out;
		}
	}

	mmap_write_lock(mm);

	vma = find_vma(mm, m.addr);
	if (!vma ||
	    vma->vm_ops != &privcmd_vm_ops) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * Caller must either:
	 *
	 * Map the whole VMA range, which will also allocate all the
	 * pages required for the auto_translated_physmap case.
	 *
	 * Or
	 *
	 * Map unmapped holes left from a previous map attempt (e.g.,
	 * because those foreign frames were previously paged out).
	 */
	if (vma->vm_private_data == NULL) {
		if (m.addr != vma->vm_start ||
		    m.addr + (nr_pages << PAGE_SHIFT) != vma->vm_end) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (xen_feature(XENFEAT_auto_translated_physmap)) {
			ret = alloc_empty_pages(vma, nr_pages);
			if (ret < 0)
				goto out_unlock;
		} else
			vma->vm_private_data = PRIV_VMA_LOCKED;
	} else {
		if (m.addr < vma->vm_start ||
		    m.addr + (nr_pages << PAGE_SHIFT) > vma->vm_end) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (privcmd_vma_range_is_mapped(vma, m.addr, nr_pages)) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	state.domain        = m.dom;
	state.vma           = vma;
	state.va            = m.addr;
	state.index         = 0;
	state.global_error  = 0;
	state.version       = version;

	BUILD_BUG_ON(((PAGE_SIZE / sizeof(xen_pfn_t)) % XEN_PFN_PER_PAGE) != 0);
	/* mmap_batch_fn guarantees ret == 0 */
	BUG_ON(traverse_pages_block(m.num, sizeof(xen_pfn_t),
				    &pagelist, mmap_batch_fn, &state));

	mmap_write_unlock(mm);

	if (state.global_error) {
		/* Write back errors in second pass. */
		state.user_gfn = (xen_pfn_t *)m.arr;
		state.user_err = m.err;
		ret = traverse_pages_block(m.num, sizeof(xen_pfn_t),
					   &pagelist, mmap_return_errors, &state);
	} else
		ret = 0;

	/* If we have not had any EFAULT-like global errors then set the global
	 * error to -ENOENT if necessary. */
	if ((ret == 0) && (state.global_error == -ENOENT))
		ret = -ENOENT;

out:
	free_page_list(&pagelist);
	return ret;

out_unlock:
	mmap_write_unlock(mm);
	goto out;
}

static int lock_pages(
	struct privcmd_dm_op_buf kbufs[], unsigned int num,
	struct page *pages[], unsigned int nr_pages, unsigned int *pinned)
{
	unsigned int i, off = 0;

	for (i = 0; i < num; ) {
		unsigned int requested;
		int page_count;

		requested = DIV_ROUND_UP(
			offset_in_page(kbufs[i].uptr) + kbufs[i].size,
			PAGE_SIZE) - off;
		if (requested > nr_pages)
			return -ENOSPC;

		page_count = pin_user_pages_fast(
			(unsigned long)kbufs[i].uptr + off * PAGE_SIZE,
			requested, FOLL_WRITE, pages);
		if (page_count <= 0)
			return page_count ? : -EFAULT;

		*pinned += page_count;
		nr_pages -= page_count;
		pages += page_count;

		off = (requested == page_count) ? 0 : off + page_count;
		i += !off;
	}

	return 0;
}

static void unlock_pages(struct page *pages[], unsigned int nr_pages)
{
	unpin_user_pages_dirty_lock(pages, nr_pages, true);
}

static long privcmd_ioctl_dm_op(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct privcmd_dm_op kdata;
	struct privcmd_dm_op_buf *kbufs;
	unsigned int nr_pages = 0;
	struct page **pages = NULL;
	struct xen_dm_op_buf *xbufs = NULL;
	unsigned int i;
	long rc;
	unsigned int pinned = 0;

	if (copy_from_user(&kdata, udata, sizeof(kdata)))
		return -EFAULT;

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != kdata.dom)
		return -EPERM;

	if (kdata.num == 0)
		return 0;

	if (kdata.num > privcmd_dm_op_max_num)
		return -E2BIG;

	kbufs = kcalloc(kdata.num, sizeof(*kbufs), GFP_KERNEL);
	if (!kbufs)
		return -ENOMEM;

	if (copy_from_user(kbufs, kdata.ubufs,
			   sizeof(*kbufs) * kdata.num)) {
		rc = -EFAULT;
		goto out;
	}

	for (i = 0; i < kdata.num; i++) {
		if (kbufs[i].size > privcmd_dm_op_buf_max_size) {
			rc = -E2BIG;
			goto out;
		}

		if (!access_ok(kbufs[i].uptr,
			       kbufs[i].size)) {
			rc = -EFAULT;
			goto out;
		}

		nr_pages += DIV_ROUND_UP(
			offset_in_page(kbufs[i].uptr) + kbufs[i].size,
			PAGE_SIZE);
	}

	pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		rc = -ENOMEM;
		goto out;
	}

	xbufs = kcalloc(kdata.num, sizeof(*xbufs), GFP_KERNEL);
	if (!xbufs) {
		rc = -ENOMEM;
		goto out;
	}

	rc = lock_pages(kbufs, kdata.num, pages, nr_pages, &pinned);
	if (rc < 0)
		goto out;

	for (i = 0; i < kdata.num; i++) {
		set_xen_guest_handle(xbufs[i].h, kbufs[i].uptr);
		xbufs[i].size = kbufs[i].size;
	}

	xen_preemptible_hcall_begin();
	rc = HYPERVISOR_dm_op(kdata.dom, kdata.num, xbufs);
	xen_preemptible_hcall_end();

out:
	unlock_pages(pages, pinned);
	kfree(xbufs);
	kfree(pages);
	kfree(kbufs);

	return rc;
}

static long privcmd_ioctl_restrict(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	domid_t dom;

	if (copy_from_user(&dom, udata, sizeof(dom)))
		return -EFAULT;

	/* Set restriction to the specified domain, or check it matches */
	if (data->domid == DOMID_INVALID)
		data->domid = dom;
	else if (data->domid != dom)
		return -EINVAL;

	return 0;
}

static long privcmd_ioctl_mmap_resource(struct file *file,
				struct privcmd_mmap_resource __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct privcmd_mmap_resource kdata;
	xen_pfn_t *pfns = NULL;
	struct xen_mem_acquire_resource xdata = { };
	int rc;

	if (copy_from_user(&kdata, udata, sizeof(kdata)))
		return -EFAULT;

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != kdata.dom)
		return -EPERM;

	/* Both fields must be set or unset */
	if (!!kdata.addr != !!kdata.num)
		return -EINVAL;

	xdata.domid = kdata.dom;
	xdata.type = kdata.type;
	xdata.id = kdata.id;

	if (!kdata.addr && !kdata.num) {
		/* Query the size of the resource. */
		rc = HYPERVISOR_memory_op(XENMEM_acquire_resource, &xdata);
		if (rc)
			return rc;
		return __put_user(xdata.nr_frames, &udata->num);
	}

	mmap_write_lock(mm);

	vma = find_vma(mm, kdata.addr);
	if (!vma || vma->vm_ops != &privcmd_vm_ops) {
		rc = -EINVAL;
		goto out;
	}

	pfns = kcalloc(kdata.num, sizeof(*pfns), GFP_KERNEL | __GFP_NOWARN);
	if (!pfns) {
		rc = -ENOMEM;
		goto out;
	}

	if (IS_ENABLED(CONFIG_XEN_AUTO_XLATE) &&
	    xen_feature(XENFEAT_auto_translated_physmap)) {
		unsigned int nr = DIV_ROUND_UP(kdata.num, XEN_PFN_PER_PAGE);
		struct page **pages;
		unsigned int i;

		rc = alloc_empty_pages(vma, nr);
		if (rc < 0)
			goto out;

		pages = vma->vm_private_data;

		for (i = 0; i < kdata.num; i++) {
			xen_pfn_t pfn =
				page_to_xen_pfn(pages[i / XEN_PFN_PER_PAGE]);

			pfns[i] = pfn + (i % XEN_PFN_PER_PAGE);
		}
	} else
		vma->vm_private_data = PRIV_VMA_LOCKED;

	xdata.frame = kdata.idx;
	xdata.nr_frames = kdata.num;
	set_xen_guest_handle(xdata.frame_list, pfns);

	xen_preemptible_hcall_begin();
	rc = HYPERVISOR_memory_op(XENMEM_acquire_resource, &xdata);
	xen_preemptible_hcall_end();

	if (rc)
		goto out;

	if (IS_ENABLED(CONFIG_XEN_AUTO_XLATE) &&
	    xen_feature(XENFEAT_auto_translated_physmap)) {
		rc = xen_remap_vma_range(vma, kdata.addr, kdata.num << PAGE_SHIFT);
	} else {
		unsigned int domid =
			(xdata.flags & XENMEM_rsrc_acq_caller_owned) ?
			DOMID_SELF : kdata.dom;
		int num, *errs = (int *)pfns;

		BUILD_BUG_ON(sizeof(*errs) > sizeof(*pfns));
		num = xen_remap_domain_mfn_array(vma,
						 kdata.addr & PAGE_MASK,
						 pfns, kdata.num, errs,
						 vma->vm_page_prot,
						 domid);
		if (num < 0)
			rc = num;
		else if (num != kdata.num) {
			unsigned int i;

			for (i = 0; i < num; i++) {
				rc = errs[i];
				if (rc < 0)
					break;
			}
		} else
			rc = 0;
	}

out:
	mmap_write_unlock(mm);
	kfree(pfns);

	return rc;
}

#ifdef CONFIG_XEN_PRIVCMD_EVENTFD
/* Irqfd support */
static struct workqueue_struct *irqfd_cleanup_wq;
static DEFINE_SPINLOCK(irqfds_lock);
static LIST_HEAD(irqfds_list);

struct privcmd_kernel_irqfd {
	struct xen_dm_op_buf xbufs;
	domid_t dom;
	bool error;
	struct eventfd_ctx *eventfd;
	struct work_struct shutdown;
	wait_queue_entry_t wait;
	struct list_head list;
	poll_table pt;
};

static void irqfd_deactivate(struct privcmd_kernel_irqfd *kirqfd)
{
	lockdep_assert_held(&irqfds_lock);

	list_del_init(&kirqfd->list);
	queue_work(irqfd_cleanup_wq, &kirqfd->shutdown);
}

static void irqfd_shutdown(struct work_struct *work)
{
	struct privcmd_kernel_irqfd *kirqfd =
		container_of(work, struct privcmd_kernel_irqfd, shutdown);
	u64 cnt;

	eventfd_ctx_remove_wait_queue(kirqfd->eventfd, &kirqfd->wait, &cnt);
	eventfd_ctx_put(kirqfd->eventfd);
	kfree(kirqfd);
}

static void irqfd_inject(struct privcmd_kernel_irqfd *kirqfd)
{
	u64 cnt;
	long rc;

	eventfd_ctx_do_read(kirqfd->eventfd, &cnt);

	xen_preemptible_hcall_begin();
	rc = HYPERVISOR_dm_op(kirqfd->dom, 1, &kirqfd->xbufs);
	xen_preemptible_hcall_end();

	/* Don't repeat the error message for consecutive failures */
	if (rc && !kirqfd->error) {
		pr_err("Failed to configure irq for guest domain: %d\n",
		       kirqfd->dom);
	}

	kirqfd->error = rc;
}

static int
irqfd_wakeup(wait_queue_entry_t *wait, unsigned int mode, int sync, void *key)
{
	struct privcmd_kernel_irqfd *kirqfd =
		container_of(wait, struct privcmd_kernel_irqfd, wait);
	__poll_t flags = key_to_poll(key);

	if (flags & EPOLLIN)
		irqfd_inject(kirqfd);

	if (flags & EPOLLHUP) {
		unsigned long flags;

		spin_lock_irqsave(&irqfds_lock, flags);
		irqfd_deactivate(kirqfd);
		spin_unlock_irqrestore(&irqfds_lock, flags);
	}

	return 0;
}

static void
irqfd_poll_func(struct file *file, wait_queue_head_t *wqh, poll_table *pt)
{
	struct privcmd_kernel_irqfd *kirqfd =
		container_of(pt, struct privcmd_kernel_irqfd, pt);

	add_wait_queue_priority(wqh, &kirqfd->wait);
}

static int privcmd_irqfd_assign(struct privcmd_irqfd *irqfd)
{
	struct privcmd_kernel_irqfd *kirqfd, *tmp;
	unsigned long flags;
	__poll_t events;
	struct fd f;
	void *dm_op;
	int ret;

	kirqfd = kzalloc(sizeof(*kirqfd) + irqfd->size, GFP_KERNEL);
	if (!kirqfd)
		return -ENOMEM;
	dm_op = kirqfd + 1;

	if (copy_from_user(dm_op, u64_to_user_ptr(irqfd->dm_op), irqfd->size)) {
		ret = -EFAULT;
		goto error_kfree;
	}

	kirqfd->xbufs.size = irqfd->size;
	set_xen_guest_handle(kirqfd->xbufs.h, dm_op);
	kirqfd->dom = irqfd->dom;
	INIT_WORK(&kirqfd->shutdown, irqfd_shutdown);

	f = fdget(irqfd->fd);
	if (!f.file) {
		ret = -EBADF;
		goto error_kfree;
	}

	kirqfd->eventfd = eventfd_ctx_fileget(f.file);
	if (IS_ERR(kirqfd->eventfd)) {
		ret = PTR_ERR(kirqfd->eventfd);
		goto error_fd_put;
	}

	/*
	 * Install our own custom wake-up handling so we are notified via a
	 * callback whenever someone signals the underlying eventfd.
	 */
	init_waitqueue_func_entry(&kirqfd->wait, irqfd_wakeup);
	init_poll_funcptr(&kirqfd->pt, irqfd_poll_func);

	spin_lock_irqsave(&irqfds_lock, flags);

	list_for_each_entry(tmp, &irqfds_list, list) {
		if (kirqfd->eventfd == tmp->eventfd) {
			ret = -EBUSY;
			spin_unlock_irqrestore(&irqfds_lock, flags);
			goto error_eventfd;
		}
	}

	list_add_tail(&kirqfd->list, &irqfds_list);
	spin_unlock_irqrestore(&irqfds_lock, flags);

	/*
	 * Check if there was an event already pending on the eventfd before we
	 * registered, and trigger it as if we didn't miss it.
	 */
	events = vfs_poll(f.file, &kirqfd->pt);
	if (events & EPOLLIN)
		irqfd_inject(kirqfd);

	/*
	 * Do not drop the file until the kirqfd is fully initialized, otherwise
	 * we might race against the EPOLLHUP.
	 */
	fdput(f);
	return 0;

error_eventfd:
	eventfd_ctx_put(kirqfd->eventfd);

error_fd_put:
	fdput(f);

error_kfree:
	kfree(kirqfd);
	return ret;
}

static int privcmd_irqfd_deassign(struct privcmd_irqfd *irqfd)
{
	struct privcmd_kernel_irqfd *kirqfd;
	struct eventfd_ctx *eventfd;
	unsigned long flags;

	eventfd = eventfd_ctx_fdget(irqfd->fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	spin_lock_irqsave(&irqfds_lock, flags);

	list_for_each_entry(kirqfd, &irqfds_list, list) {
		if (kirqfd->eventfd == eventfd) {
			irqfd_deactivate(kirqfd);
			break;
		}
	}

	spin_unlock_irqrestore(&irqfds_lock, flags);

	eventfd_ctx_put(eventfd);

	/*
	 * Block until we know all outstanding shutdown jobs have completed so
	 * that we guarantee there will not be any more interrupts once this
	 * deassign function returns.
	 */
	flush_workqueue(irqfd_cleanup_wq);

	return 0;
}

static long privcmd_ioctl_irqfd(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct privcmd_irqfd irqfd;

	if (copy_from_user(&irqfd, udata, sizeof(irqfd)))
		return -EFAULT;

	/* No other flags should be set */
	if (irqfd.flags & ~PRIVCMD_IRQFD_FLAG_DEASSIGN)
		return -EINVAL;

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != irqfd.dom)
		return -EPERM;

	if (irqfd.flags & PRIVCMD_IRQFD_FLAG_DEASSIGN)
		return privcmd_irqfd_deassign(&irqfd);

	return privcmd_irqfd_assign(&irqfd);
}

static int privcmd_irqfd_init(void)
{
	irqfd_cleanup_wq = alloc_workqueue("privcmd-irqfd-cleanup", 0, 0);
	if (!irqfd_cleanup_wq)
		return -ENOMEM;

	return 0;
}

static void privcmd_irqfd_exit(void)
{
	struct privcmd_kernel_irqfd *kirqfd, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&irqfds_lock, flags);

	list_for_each_entry_safe(kirqfd, tmp, &irqfds_list, list)
		irqfd_deactivate(kirqfd);

	spin_unlock_irqrestore(&irqfds_lock, flags);

	destroy_workqueue(irqfd_cleanup_wq);
}

/* Ioeventfd Support */
#define QUEUE_NOTIFY_VQ_MASK 0xFFFF

static DEFINE_MUTEX(ioreq_lock);
static LIST_HEAD(ioreq_list);

/* per-eventfd structure */
struct privcmd_kernel_ioeventfd {
	struct eventfd_ctx *eventfd;
	struct list_head list;
	u64 addr;
	unsigned int addr_len;
	unsigned int vq;
};

/* per-guest CPU / port structure */
struct ioreq_port {
	int vcpu;
	unsigned int port;
	struct privcmd_kernel_ioreq *kioreq;
};

/* per-guest structure */
struct privcmd_kernel_ioreq {
	domid_t dom;
	unsigned int vcpus;
	u64 uioreq;
	struct ioreq *ioreq;
	spinlock_t lock; /* Protects ioeventfds list */
	struct list_head ioeventfds;
	struct list_head list;
	struct ioreq_port ports[] __counted_by(vcpus);
};

static irqreturn_t ioeventfd_interrupt(int irq, void *dev_id)
{
	struct ioreq_port *port = dev_id;
	struct privcmd_kernel_ioreq *kioreq = port->kioreq;
	struct ioreq *ioreq = &kioreq->ioreq[port->vcpu];
	struct privcmd_kernel_ioeventfd *kioeventfd;
	unsigned int state = STATE_IOREQ_READY;

	if (ioreq->state != STATE_IOREQ_READY ||
	    ioreq->type != IOREQ_TYPE_COPY || ioreq->dir != IOREQ_WRITE)
		return IRQ_NONE;

	/*
	 * We need a barrier, smp_mb(), here to ensure reads are finished before
	 * `state` is updated. Since the lock implementation ensures that
	 * appropriate barrier will be added anyway, we can avoid adding
	 * explicit barrier here.
	 *
	 * Ideally we don't need to update `state` within the locks, but we do
	 * that here to avoid adding explicit barrier.
	 */

	spin_lock(&kioreq->lock);
	ioreq->state = STATE_IOREQ_INPROCESS;

	list_for_each_entry(kioeventfd, &kioreq->ioeventfds, list) {
		if (ioreq->addr == kioeventfd->addr + VIRTIO_MMIO_QUEUE_NOTIFY &&
		    ioreq->size == kioeventfd->addr_len &&
		    (ioreq->data & QUEUE_NOTIFY_VQ_MASK) == kioeventfd->vq) {
			eventfd_signal(kioeventfd->eventfd);
			state = STATE_IORESP_READY;
			break;
		}
	}
	spin_unlock(&kioreq->lock);

	/*
	 * We need a barrier, smp_mb(), here to ensure writes are finished
	 * before `state` is updated. Since the lock implementation ensures that
	 * appropriate barrier will be added anyway, we can avoid adding
	 * explicit barrier here.
	 */

	ioreq->state = state;

	if (state == STATE_IORESP_READY) {
		notify_remote_via_evtchn(port->port);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void ioreq_free(struct privcmd_kernel_ioreq *kioreq)
{
	struct ioreq_port *ports = kioreq->ports;
	int i;

	lockdep_assert_held(&ioreq_lock);

	list_del(&kioreq->list);

	for (i = kioreq->vcpus - 1; i >= 0; i--)
		unbind_from_irqhandler(irq_from_evtchn(ports[i].port), &ports[i]);

	kfree(kioreq);
}

static
struct privcmd_kernel_ioreq *alloc_ioreq(struct privcmd_ioeventfd *ioeventfd)
{
	struct privcmd_kernel_ioreq *kioreq;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct page **pages;
	unsigned int *ports;
	int ret, size, i;

	lockdep_assert_held(&ioreq_lock);

	size = struct_size(kioreq, ports, ioeventfd->vcpus);
	kioreq = kzalloc(size, GFP_KERNEL);
	if (!kioreq)
		return ERR_PTR(-ENOMEM);

	kioreq->dom = ioeventfd->dom;
	kioreq->vcpus = ioeventfd->vcpus;
	kioreq->uioreq = ioeventfd->ioreq;
	spin_lock_init(&kioreq->lock);
	INIT_LIST_HEAD(&kioreq->ioeventfds);

	/* The memory for ioreq server must have been mapped earlier */
	mmap_write_lock(mm);
	vma = find_vma(mm, (unsigned long)ioeventfd->ioreq);
	if (!vma) {
		pr_err("Failed to find vma for ioreq page!\n");
		mmap_write_unlock(mm);
		ret = -EFAULT;
		goto error_kfree;
	}

	pages = vma->vm_private_data;
	kioreq->ioreq = (struct ioreq *)(page_to_virt(pages[0]));
	mmap_write_unlock(mm);

	ports = memdup_array_user(u64_to_user_ptr(ioeventfd->ports),
				  kioreq->vcpus, sizeof(*ports));
	if (IS_ERR(ports)) {
		ret = PTR_ERR(ports);
		goto error_kfree;
	}

	for (i = 0; i < kioreq->vcpus; i++) {
		kioreq->ports[i].vcpu = i;
		kioreq->ports[i].port = ports[i];
		kioreq->ports[i].kioreq = kioreq;

		ret = bind_evtchn_to_irqhandler_lateeoi(ports[i],
				ioeventfd_interrupt, IRQF_SHARED, "ioeventfd",
				&kioreq->ports[i]);
		if (ret < 0)
			goto error_unbind;
	}

	kfree(ports);

	list_add_tail(&kioreq->list, &ioreq_list);

	return kioreq;

error_unbind:
	while (--i >= 0)
		unbind_from_irqhandler(irq_from_evtchn(ports[i]), &kioreq->ports[i]);

	kfree(ports);
error_kfree:
	kfree(kioreq);
	return ERR_PTR(ret);
}

static struct privcmd_kernel_ioreq *
get_ioreq(struct privcmd_ioeventfd *ioeventfd, struct eventfd_ctx *eventfd)
{
	struct privcmd_kernel_ioreq *kioreq;
	unsigned long flags;

	list_for_each_entry(kioreq, &ioreq_list, list) {
		struct privcmd_kernel_ioeventfd *kioeventfd;

		/*
		 * kioreq fields can be accessed here without a lock as they are
		 * never updated after being added to the ioreq_list.
		 */
		if (kioreq->uioreq != ioeventfd->ioreq) {
			continue;
		} else if (kioreq->dom != ioeventfd->dom ||
			   kioreq->vcpus != ioeventfd->vcpus) {
			pr_err("Invalid ioeventfd configuration mismatch, dom (%u vs %u), vcpus (%u vs %u)\n",
			       kioreq->dom, ioeventfd->dom, kioreq->vcpus,
			       ioeventfd->vcpus);
			return ERR_PTR(-EINVAL);
		}

		/* Look for a duplicate eventfd for the same guest */
		spin_lock_irqsave(&kioreq->lock, flags);
		list_for_each_entry(kioeventfd, &kioreq->ioeventfds, list) {
			if (eventfd == kioeventfd->eventfd) {
				spin_unlock_irqrestore(&kioreq->lock, flags);
				return ERR_PTR(-EBUSY);
			}
		}
		spin_unlock_irqrestore(&kioreq->lock, flags);

		return kioreq;
	}

	/* Matching kioreq isn't found, allocate a new one */
	return alloc_ioreq(ioeventfd);
}

static void ioeventfd_free(struct privcmd_kernel_ioeventfd *kioeventfd)
{
	list_del(&kioeventfd->list);
	eventfd_ctx_put(kioeventfd->eventfd);
	kfree(kioeventfd);
}

static int privcmd_ioeventfd_assign(struct privcmd_ioeventfd *ioeventfd)
{
	struct privcmd_kernel_ioeventfd *kioeventfd;
	struct privcmd_kernel_ioreq *kioreq;
	unsigned long flags;
	struct fd f;
	int ret;

	/* Check for range overflow */
	if (ioeventfd->addr + ioeventfd->addr_len < ioeventfd->addr)
		return -EINVAL;

	/* Vhost requires us to support length 1, 2, 4, and 8 */
	if (!(ioeventfd->addr_len == 1 || ioeventfd->addr_len == 2 ||
	      ioeventfd->addr_len == 4 || ioeventfd->addr_len == 8))
		return -EINVAL;

	/* 4096 vcpus limit enough ? */
	if (!ioeventfd->vcpus || ioeventfd->vcpus > 4096)
		return -EINVAL;

	kioeventfd = kzalloc(sizeof(*kioeventfd), GFP_KERNEL);
	if (!kioeventfd)
		return -ENOMEM;

	f = fdget(ioeventfd->event_fd);
	if (!f.file) {
		ret = -EBADF;
		goto error_kfree;
	}

	kioeventfd->eventfd = eventfd_ctx_fileget(f.file);
	fdput(f);

	if (IS_ERR(kioeventfd->eventfd)) {
		ret = PTR_ERR(kioeventfd->eventfd);
		goto error_kfree;
	}

	kioeventfd->addr = ioeventfd->addr;
	kioeventfd->addr_len = ioeventfd->addr_len;
	kioeventfd->vq = ioeventfd->vq;

	mutex_lock(&ioreq_lock);
	kioreq = get_ioreq(ioeventfd, kioeventfd->eventfd);
	if (IS_ERR(kioreq)) {
		mutex_unlock(&ioreq_lock);
		ret = PTR_ERR(kioreq);
		goto error_eventfd;
	}

	spin_lock_irqsave(&kioreq->lock, flags);
	list_add_tail(&kioeventfd->list, &kioreq->ioeventfds);
	spin_unlock_irqrestore(&kioreq->lock, flags);

	mutex_unlock(&ioreq_lock);

	return 0;

error_eventfd:
	eventfd_ctx_put(kioeventfd->eventfd);

error_kfree:
	kfree(kioeventfd);
	return ret;
}

static int privcmd_ioeventfd_deassign(struct privcmd_ioeventfd *ioeventfd)
{
	struct privcmd_kernel_ioreq *kioreq, *tkioreq;
	struct eventfd_ctx *eventfd;
	unsigned long flags;
	int ret = 0;

	eventfd = eventfd_ctx_fdget(ioeventfd->event_fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	mutex_lock(&ioreq_lock);
	list_for_each_entry_safe(kioreq, tkioreq, &ioreq_list, list) {
		struct privcmd_kernel_ioeventfd *kioeventfd, *tmp;
		/*
		 * kioreq fields can be accessed here without a lock as they are
		 * never updated after being added to the ioreq_list.
		 */
		if (kioreq->dom != ioeventfd->dom ||
		    kioreq->uioreq != ioeventfd->ioreq ||
		    kioreq->vcpus != ioeventfd->vcpus)
			continue;

		spin_lock_irqsave(&kioreq->lock, flags);
		list_for_each_entry_safe(kioeventfd, tmp, &kioreq->ioeventfds, list) {
			if (eventfd == kioeventfd->eventfd) {
				ioeventfd_free(kioeventfd);
				spin_unlock_irqrestore(&kioreq->lock, flags);

				if (list_empty(&kioreq->ioeventfds))
					ioreq_free(kioreq);
				goto unlock;
			}
		}
		spin_unlock_irqrestore(&kioreq->lock, flags);
		break;
	}

	pr_err("Ioeventfd isn't already assigned, dom: %u, addr: %llu\n",
	       ioeventfd->dom, ioeventfd->addr);
	ret = -ENODEV;

unlock:
	mutex_unlock(&ioreq_lock);
	eventfd_ctx_put(eventfd);

	return ret;
}

static long privcmd_ioctl_ioeventfd(struct file *file, void __user *udata)
{
	struct privcmd_data *data = file->private_data;
	struct privcmd_ioeventfd ioeventfd;

	if (copy_from_user(&ioeventfd, udata, sizeof(ioeventfd)))
		return -EFAULT;

	/* No other flags should be set */
	if (ioeventfd.flags & ~PRIVCMD_IOEVENTFD_FLAG_DEASSIGN)
		return -EINVAL;

	/* If restriction is in place, check the domid matches */
	if (data->domid != DOMID_INVALID && data->domid != ioeventfd.dom)
		return -EPERM;

	if (ioeventfd.flags & PRIVCMD_IOEVENTFD_FLAG_DEASSIGN)
		return privcmd_ioeventfd_deassign(&ioeventfd);

	return privcmd_ioeventfd_assign(&ioeventfd);
}

static void privcmd_ioeventfd_exit(void)
{
	struct privcmd_kernel_ioreq *kioreq, *tmp;
	unsigned long flags;

	mutex_lock(&ioreq_lock);
	list_for_each_entry_safe(kioreq, tmp, &ioreq_list, list) {
		struct privcmd_kernel_ioeventfd *kioeventfd, *tmp;

		spin_lock_irqsave(&kioreq->lock, flags);
		list_for_each_entry_safe(kioeventfd, tmp, &kioreq->ioeventfds, list)
			ioeventfd_free(kioeventfd);
		spin_unlock_irqrestore(&kioreq->lock, flags);

		ioreq_free(kioreq);
	}
	mutex_unlock(&ioreq_lock);
}
#else
static inline long privcmd_ioctl_irqfd(struct file *file, void __user *udata)
{
	return -EOPNOTSUPP;
}

static inline int privcmd_irqfd_init(void)
{
	return 0;
}

static inline void privcmd_irqfd_exit(void)
{
}

static inline long privcmd_ioctl_ioeventfd(struct file *file, void __user *udata)
{
	return -EOPNOTSUPP;
}

static inline void privcmd_ioeventfd_exit(void)
{
}
#endif /* CONFIG_XEN_PRIVCMD_EVENTFD */

static long privcmd_ioctl(struct file *file,
			  unsigned int cmd, unsigned long data)
{
	int ret = -ENOTTY;
	void __user *udata = (void __user *) data;

	switch (cmd) {
	case IOCTL_PRIVCMD_HYPERCALL:
		ret = privcmd_ioctl_hypercall(file, udata);
		break;

	case IOCTL_PRIVCMD_MMAP:
		ret = privcmd_ioctl_mmap(file, udata);
		break;

	case IOCTL_PRIVCMD_MMAPBATCH:
		ret = privcmd_ioctl_mmap_batch(file, udata, 1);
		break;

	case IOCTL_PRIVCMD_MMAPBATCH_V2:
		ret = privcmd_ioctl_mmap_batch(file, udata, 2);
		break;

	case IOCTL_PRIVCMD_DM_OP:
		ret = privcmd_ioctl_dm_op(file, udata);
		break;

	case IOCTL_PRIVCMD_RESTRICT:
		ret = privcmd_ioctl_restrict(file, udata);
		break;

	case IOCTL_PRIVCMD_MMAP_RESOURCE:
		ret = privcmd_ioctl_mmap_resource(file, udata);
		break;

	case IOCTL_PRIVCMD_IRQFD:
		ret = privcmd_ioctl_irqfd(file, udata);
		break;

	case IOCTL_PRIVCMD_IOEVENTFD:
		ret = privcmd_ioctl_ioeventfd(file, udata);
		break;

	default:
		break;
	}

	return ret;
}

static int privcmd_open(struct inode *ino, struct file *file)
{
	struct privcmd_data *data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	/* DOMID_INVALID implies no restriction */
	data->domid = DOMID_INVALID;

	file->private_data = data;
	return 0;
}

static int privcmd_release(struct inode *ino, struct file *file)
{
	struct privcmd_data *data = file->private_data;

	kfree(data);
	return 0;
}

static void privcmd_close(struct vm_area_struct *vma)
{
	struct page **pages = vma->vm_private_data;
	int numpgs = vma_pages(vma);
	int numgfns = (vma->vm_end - vma->vm_start) >> XEN_PAGE_SHIFT;
	int rc;

	if (!xen_feature(XENFEAT_auto_translated_physmap) || !numpgs || !pages)
		return;

	rc = xen_unmap_domain_gfn_range(vma, numgfns, pages);
	if (rc == 0)
		xen_free_unpopulated_pages(numpgs, pages);
	else
		pr_crit("unable to unmap MFN range: leaking %d pages. rc=%d\n",
			numpgs, rc);
	kvfree(pages);
}

static vm_fault_t privcmd_fault(struct vm_fault *vmf)
{
	printk(KERN_DEBUG "privcmd_fault: vma=%p %lx-%lx, pgoff=%lx, uv=%p\n",
	       vmf->vma, vmf->vma->vm_start, vmf->vma->vm_end,
	       vmf->pgoff, (void *)vmf->address);

	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct privcmd_vm_ops = {
	.close = privcmd_close,
	.fault = privcmd_fault
};

static int privcmd_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* DONTCOPY is essential for Xen because copy_page_range doesn't know
	 * how to recreate these mappings */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTCOPY |
			 VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &privcmd_vm_ops;
	vma->vm_private_data = NULL;

	return 0;
}

/*
 * For MMAPBATCH*. This allows asserting the singleshot mapping
 * on a per pfn/pte basis. Mapping calls that fail with ENOENT
 * can be then retried until success.
 */
static int is_mapped_fn(pte_t *pte, unsigned long addr, void *data)
{
	return pte_none(ptep_get(pte)) ? 0 : -EBUSY;
}

static int privcmd_vma_range_is_mapped(
	           struct vm_area_struct *vma,
	           unsigned long addr,
	           unsigned long nr_pages)
{
	return apply_to_page_range(vma->vm_mm, addr, nr_pages << PAGE_SHIFT,
				   is_mapped_fn, NULL) != 0;
}

const struct file_operations xen_privcmd_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = privcmd_ioctl,
	.open = privcmd_open,
	.release = privcmd_release,
	.mmap = privcmd_mmap,
};
EXPORT_SYMBOL_GPL(xen_privcmd_fops);

static struct miscdevice privcmd_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xen/privcmd",
	.fops = &xen_privcmd_fops,
};

static int __init privcmd_init(void)
{
	int err;

	if (!xen_domain())
		return -ENODEV;

	err = misc_register(&privcmd_dev);
	if (err != 0) {
		pr_err("Could not register Xen privcmd device\n");
		return err;
	}

	err = misc_register(&xen_privcmdbuf_dev);
	if (err != 0) {
		pr_err("Could not register Xen hypercall-buf device\n");
		goto err_privcmdbuf;
	}

	err = privcmd_irqfd_init();
	if (err != 0) {
		pr_err("irqfd init failed\n");
		goto err_irqfd;
	}

	return 0;

err_irqfd:
	misc_deregister(&xen_privcmdbuf_dev);
err_privcmdbuf:
	misc_deregister(&privcmd_dev);
	return err;
}

static void __exit privcmd_exit(void)
{
	privcmd_ioeventfd_exit();
	privcmd_irqfd_exit();
	misc_deregister(&privcmd_dev);
	misc_deregister(&xen_privcmdbuf_dev);
}

module_init(privcmd_init);
module_exit(privcmd_exit);
