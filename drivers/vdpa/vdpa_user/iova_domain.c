// SPDX-License-Identifier: GPL-2.0-only
/*
 * MMU-based software IOTLB.
 *
 * Copyright (C) 2020-2021 Bytedance Inc. and/or its affiliates. All rights reserved.
 *
 * Author: Xie Yongji <xieyongji@bytedance.com>
 *
 */

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/vdpa.h>

#include "iova_domain.h"

static int vduse_iotlb_add_range(struct vduse_domain_iotlb *iotlb,
				 u64 start, u64 last,
				 u64 addr, unsigned int perm,
				 struct file *file, u64 offset)
{
	struct vdpa_map_file *map_file;
	int ret;

	map_file = kmalloc(sizeof(*map_file), GFP_ATOMIC);
	if (!map_file)
		return -ENOMEM;

	map_file->file = get_file(file);
	map_file->offset = offset;

	ret = vhost_iotlb_add_range_ctx(iotlb->root, start, last,
					addr, perm, map_file);
	if (ret) {
		fput(map_file->file);
		kfree(map_file);
		return ret;
	}
	return 0;
}

static void vduse_iotlb_del_range(struct vduse_domain_iotlb *iotlb,
				  u64 start, u64 last)
{
	struct vdpa_map_file *map_file;
	struct vhost_iotlb_map *map;

	while ((map = vhost_iotlb_itree_first(iotlb->root, start, last))) {
		map_file = (struct vdpa_map_file *)map->opaque;
		fput(map_file->file);
		kfree(map_file);
		vhost_iotlb_map_free(iotlb->root, map);
	}
}

static void vduse_iotlb_deinit(struct vduse_domain_iotlb *iotlb)
{
	vhost_iotlb_free(iotlb->root);
}

static int vduse_iotlb_init(struct vduse_domain_iotlb *iotlb)
{
	iotlb->root = vhost_iotlb_alloc(0, 0);
	if (!iotlb->root)
		return -ENOMEM;

	spin_lock_init(&iotlb->lock);
	return 0;
}

int vduse_domain_set_map(struct vduse_iova_domain *domain,
			 struct vhost_iotlb *iotlb)
{
	struct vdpa_map_file *map_file;
	struct vhost_iotlb_map *map;
	u64 start = 0ULL, last = ULLONG_MAX;
	int ret;

	spin_lock(&domain->iotlb.lock);
	vduse_iotlb_del_range(&domain->iotlb, start, last);

	for (map = vhost_iotlb_itree_first(iotlb, start, last); map;
	     map = vhost_iotlb_itree_next(map, start, last)) {
		map_file = (struct vdpa_map_file *)map->opaque;
		ret = vduse_iotlb_add_range(&domain->iotlb, map->start,
					    map->last, map->addr,
					    map->perm, map_file->file,
					    map_file->offset);
		if (ret)
			goto err;
	}
	spin_unlock(&domain->iotlb.lock);

	return 0;
err:
	vduse_iotlb_del_range(&domain->iotlb, start, last);
	spin_unlock(&domain->iotlb.lock);
	return ret;
}

void vduse_domain_clear_map(struct vduse_iova_domain *domain,
			    struct vhost_iotlb *iotlb)
{
	struct vhost_iotlb_map *map;
	u64 start = 0ULL, last = ULLONG_MAX;

	spin_lock(&domain->iotlb.lock);
	for (map = vhost_iotlb_itree_first(iotlb, start, last); map;
	     map = vhost_iotlb_itree_next(map, start, last)) {
		vduse_iotlb_del_range(&domain->iotlb, map->start, map->last);
	}
	spin_unlock(&domain->iotlb.lock);
}

static int dir_to_perm(enum dma_data_direction dir)
{
	int perm = -EFAULT;

	switch (dir) {
	case DMA_FROM_DEVICE:
		perm = VHOST_MAP_WO;
		break;
	case DMA_TO_DEVICE:
		perm = VHOST_MAP_RO;
		break;
	case DMA_BIDIRECTIONAL:
		perm = VHOST_MAP_RW;
		break;
	default:
		WARN_ON(1);
		break;
	}

	return perm;
}

void vduse_domain_reset_zc_map(struct vduse_iova_domain *domain)
{
	if (!domain->zc_map)
		return;

	spin_lock(&domain->iotlb.lock);
	if (!domain->zc_map)
		goto unlock;

	vduse_iotlb_del_range(&domain->iotlb, 1ULL << MAX_PHYSMEM_BITS,
			      ULLONG_MAX);
	domain->zc_map = 0;
unlock:
	spin_unlock(&domain->iotlb.lock);
}

bool vduse_domain_is_zc_map(struct vduse_iova_domain *domain,
			    struct vhost_iotlb_map *map)
{
	if (!domain->zc_map)
	       return false;

	if (map->start < 1ULL << MAX_PHYSMEM_BITS)
		return false;

	return true;
}

static int vduse_domain_init_zc_map(struct vduse_iova_domain *domain)
{
	int ret = 0;

	if (domain->zc_map)
		return 0;

	spin_lock(&domain->iotlb.lock);
	if (domain->zc_map)
		goto unlock;

	ret = vduse_iotlb_add_range(&domain->iotlb, 1ULL << MAX_PHYSMEM_BITS,
				    ULLONG_MAX, 0, VHOST_MAP_RW,
				    domain->file, 0);
	if (ret)
		goto unlock;

	domain->zc_map = 1;
unlock:
	spin_unlock(&domain->iotlb.lock);
	return ret;
}

static dma_addr_t vduse_domain_map_page_zc(struct vduse_iova_domain *domain,
					   struct page *page,
					   unsigned long offset,
					   size_t size,
					   enum dma_data_direction dir,
					   unsigned long attrs)
{
	phys_addr_t pa = page_to_phys(page) + offset;
	u64 va = pa | (1ULL << MAX_PHYSMEM_BITS);
	struct vduse_domain_iotlb *iotlb = &domain->iotlb_zc;
	atomic_t *inuse;

	if (vduse_domain_init_zc_map(domain))
		return DMA_MAPPING_ERROR;

	inuse = kmalloc(sizeof(atomic_t), GFP_ATOMIC);
	if (!inuse)
		return DMA_MAPPING_ERROR;

	atomic_set(inuse, 0);

	spin_lock(&iotlb->lock);
	if (vhost_iotlb_add_range_ctx(iotlb->root, pa, pa + size - 1, va,
				      dir_to_perm(dir), inuse)) {
		spin_unlock(&iotlb->lock);
		return DMA_MAPPING_ERROR;
	}
	spin_unlock(&iotlb->lock);

	return va;
}

static void vduse_domain_unmap_page_zc(struct vduse_iova_domain *domain,
				       dma_addr_t dma_addr, size_t size,
				       enum dma_data_direction dir,
				       unsigned long attrs)
{
	u64 pa = dma_addr & PHYS_ADDR_MASK;
	struct vduse_domain_iotlb *iotlb = &domain->iotlb_zc;
	struct vhost_iotlb_map *map;
	atomic_t *inuse;

	spin_lock(&iotlb->lock);
	map = vhost_iotlb_itree_first(iotlb->root, pa, pa + size - 1);
	if (WARN_ON(!map)) {
		spin_unlock(&iotlb->lock);
		return;
	}
	vhost_iotlb_map_remove(iotlb->root, map);
	inuse = (atomic_t *)map->opaque;
	spin_unlock(&iotlb->lock);

	while (atomic_read(inuse) != 0)
		cpu_relax();
	kfree(map);
	kfree(inuse);
}

static ssize_t vduse_domain_rw_page_zc(struct vduse_iova_domain *domain,
				       dma_addr_t dma_addr, bool write,
				       struct iov_iter *iter)
{
	u64 pa = dma_addr & PHYS_ADDR_MASK;
	struct vduse_domain_iotlb *iotlb = &domain->iotlb_zc;
	size_t size = iov_iter_count(iter), total_len = 0;
	atomic_t *inuse = NULL;
	size_t offset;
	struct page *page;
	struct vhost_iotlb_map *map;
	void *addr;
	ssize_t ret;

	spin_lock(&iotlb->lock);
	while ((map = vhost_iotlb_itree_first(iotlb->root,
					      pa, pa + size -1))) {
		if (pa < map->start || pa + size - 1 > map->last ||
		    (write && map->perm == VHOST_MAP_RO) ||
		    (!write && map->perm == VHOST_MAP_WO))
			continue;

		inuse = (atomic_t *)map->opaque;
		atomic_inc(inuse);
		break;
	}
	spin_unlock(&iotlb->lock);
	if (!inuse)
		return -EINVAL;

	while (iov_iter_count(iter)) {
		page = pfn_to_page(PFN_DOWN(pa));
		offset = offset_in_page(pa);
		size = min_t(size_t, PAGE_SIZE - offset, iov_iter_count(iter));
		addr = kmap_local_page(page);
		if (write)
			ret = copy_from_iter(addr + offset, size, iter);
		else
			ret = copy_to_iter(addr + offset, size, iter);

		kunmap_local(addr);
		if (ret != size) {
			total_len = -EFAULT;
			break;
		}
		pa += size;
		total_len += size;
	}
	atomic_dec(inuse);

	return total_len;
}

static ssize_t vduse_domain_write_iter(struct kiocb *iocb,
				       struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct vduse_iova_domain *domain = file->private_data;

	return vduse_domain_rw_page_zc(domain, iocb->ki_pos, true, from);
}

static ssize_t vduse_domain_read_iter(struct kiocb *iocb,
				      struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct vduse_iova_domain *domain = file->private_data;

	return vduse_domain_rw_page_zc(domain, iocb->ki_pos, false, to);
}

static int vduse_domain_map_bounce_page(struct vduse_iova_domain *domain,
					 u64 iova, u64 size, u64 paddr)
{
	struct vduse_bounce_map *map;
	u64 last = iova + size - 1;

	while (iova <= last) {
		map = &domain->bounce_maps[iova >> PAGE_SHIFT];
		if (!map->bounce_page) {
			map->bounce_page = alloc_page(GFP_ATOMIC);
			if (!map->bounce_page)
				return -ENOMEM;
		}
		map->orig_phys = paddr;
		paddr += PAGE_SIZE;
		iova += PAGE_SIZE;
	}
	return 0;
}

static void vduse_domain_unmap_bounce_page(struct vduse_iova_domain *domain,
					   u64 iova, u64 size)
{
	struct vduse_bounce_map *map;
	u64 last = iova + size - 1;

	while (iova <= last) {
		map = &domain->bounce_maps[iova >> PAGE_SHIFT];
		map->orig_phys = INVALID_PHYS_ADDR;
		iova += PAGE_SIZE;
	}
}

static void do_bounce(phys_addr_t orig, void *addr, size_t size,
		      enum dma_data_direction dir)
{
	unsigned long pfn = PFN_DOWN(orig);
	unsigned int offset = offset_in_page(orig);
	struct page *page;
	unsigned int sz = 0;

	while (size) {
		sz = min_t(size_t, PAGE_SIZE - offset, size);

		page = pfn_to_page(pfn);
		if (dir == DMA_TO_DEVICE)
			memcpy_from_page(addr, page, offset, sz);
		else
			memcpy_to_page(page, offset, addr, sz);

		size -= sz;
		pfn++;
		addr += sz;
		offset = 0;
	}
}

static void vduse_domain_bounce(struct vduse_iova_domain *domain,
				dma_addr_t iova, size_t size,
				enum dma_data_direction dir)
{
	struct vduse_bounce_map *map;
	unsigned int offset;
	void *addr;
	size_t sz;

	if (iova >= domain->bounce_size)
		return;

	while (size) {
		map = &domain->bounce_maps[iova >> PAGE_SHIFT];
		offset = offset_in_page(iova);
		sz = min_t(size_t, PAGE_SIZE - offset, size);

		if (WARN_ON(!map->bounce_page ||
			    map->orig_phys == INVALID_PHYS_ADDR))
			return;

		addr = kmap_local_page(map->bounce_page);
		do_bounce(map->orig_phys + offset, addr + offset, sz, dir);
		kunmap_local(addr);
		size -= sz;
		iova += sz;
	}
}

static struct page *
vduse_domain_get_coherent_page(struct vduse_iova_domain *domain, u64 iova)
{
	u64 start = iova & PAGE_MASK;
	u64 last = start + PAGE_SIZE - 1;
	struct vhost_iotlb_map *map;
	struct page *page = NULL;

	spin_lock(&domain->iotlb.lock);
	map = vhost_iotlb_itree_first(domain->iotlb.root, start, last);
	if (!map)
		goto out;

	page = pfn_to_page((map->addr + iova - map->start) >> PAGE_SHIFT);
	get_page(page);
out:
	spin_unlock(&domain->iotlb.lock);

	return page;
}

static struct page *
vduse_domain_get_bounce_page(struct vduse_iova_domain *domain, u64 iova)
{
	struct vduse_bounce_map *map;
	struct page *page = NULL;

	read_lock(&domain->bounce_lock);
	map = &domain->bounce_maps[iova >> PAGE_SHIFT];
	if (domain->user_bounce_pages || !map->bounce_page)
		goto out;

	page = map->bounce_page;
	get_page(page);
out:
	read_unlock(&domain->bounce_lock);

	return page;
}

static void
vduse_domain_free_kernel_bounce_pages(struct vduse_iova_domain *domain)
{
	struct vduse_bounce_map *map;
	unsigned long pfn, bounce_pfns;

	bounce_pfns = domain->bounce_size >> PAGE_SHIFT;

	for (pfn = 0; pfn < bounce_pfns; pfn++) {
		map = &domain->bounce_maps[pfn];
		if (WARN_ON(map->orig_phys != INVALID_PHYS_ADDR))
			continue;

		if (!map->bounce_page)
			continue;

		__free_page(map->bounce_page);
		map->bounce_page = NULL;
	}
}

int vduse_domain_add_user_bounce_pages(struct vduse_iova_domain *domain,
				       struct page **pages, int count)
{
	struct vduse_bounce_map *map;
	int i, ret;

	/* Now we don't support partial mapping */
	if (count != (domain->bounce_size >> PAGE_SHIFT))
		return -EINVAL;

	write_lock(&domain->bounce_lock);
	ret = -EEXIST;
	if (domain->user_bounce_pages)
		goto out;

	for (i = 0; i < count; i++) {
		map = &domain->bounce_maps[i];
		if (map->bounce_page) {
			/* Copy kernel page to user page if it's in use */
			if (map->orig_phys != INVALID_PHYS_ADDR)
				memcpy_to_page(pages[i], 0,
					       page_address(map->bounce_page),
					       PAGE_SIZE);
			__free_page(map->bounce_page);
		}
		map->bounce_page = pages[i];
		get_page(pages[i]);
	}
	domain->user_bounce_pages = true;
	ret = 0;
out:
	write_unlock(&domain->bounce_lock);

	return ret;
}

void vduse_domain_remove_user_bounce_pages(struct vduse_iova_domain *domain)
{
	struct vduse_bounce_map *map;
	unsigned long i, count;

	write_lock(&domain->bounce_lock);
	if (!domain->user_bounce_pages)
		goto out;

	count = domain->bounce_size >> PAGE_SHIFT;
	for (i = 0; i < count; i++) {
		struct page *page = NULL;

		map = &domain->bounce_maps[i];
		if (WARN_ON(!map->bounce_page))
			continue;

		/* Copy user page to kernel page if it's in use */
		if (map->orig_phys != INVALID_PHYS_ADDR) {
			page = alloc_page(GFP_ATOMIC | __GFP_NOFAIL);
			memcpy_from_page(page_address(page),
					 map->bounce_page, 0, PAGE_SIZE);
		}
		put_page(map->bounce_page);
		map->bounce_page = page;
	}
	domain->user_bounce_pages = false;
out:
	write_unlock(&domain->bounce_lock);
}

void vduse_domain_reset_bounce_map(struct vduse_iova_domain *domain)
{
	if (!domain->bounce_map)
		return;

	spin_lock(&domain->iotlb.lock);
	if (!domain->bounce_map)
		goto unlock;

	vduse_iotlb_del_range(&domain->iotlb, 0, domain->bounce_size - 1);
	domain->bounce_map = 0;
unlock:
	spin_unlock(&domain->iotlb.lock);
}

bool vduse_domain_is_bounce_map(struct vduse_iova_domain *domain,
				struct vhost_iotlb_map *map)
{
	if (!domain->bounce_map)
	       return false;

	if (map->start != 0 ||
	    map->last != domain->bounce_size - 1)
		return false;

	return true;
}

static int vduse_domain_init_bounce_map(struct vduse_iova_domain *domain)
{
	int ret = 0;

	if (domain->bounce_map)
		return 0;

	spin_lock(&domain->iotlb.lock);
	if (domain->bounce_map)
		goto unlock;

	ret = vduse_iotlb_add_range(&domain->iotlb, 0, domain->bounce_size - 1,
				    0, VHOST_MAP_RW, domain->file, 0);
	if (ret)
		goto unlock;

	domain->bounce_map = 1;
unlock:
	spin_unlock(&domain->iotlb.lock);
	return ret;
}

static dma_addr_t
vduse_domain_alloc_iova(struct iova_domain *iovad,
			unsigned long size, unsigned long limit)
{
	unsigned long shift = iova_shift(iovad);
	unsigned long iova_len = iova_align(iovad, size) >> shift;
	unsigned long iova_pfn;

	iova_pfn = alloc_iova_fast(iovad, iova_len, limit >> shift, true);

	return (dma_addr_t)iova_pfn << shift;
}

static void vduse_domain_free_iova(struct iova_domain *iovad,
				   dma_addr_t iova, size_t size)
{
	unsigned long shift = iova_shift(iovad);
	unsigned long iova_len = iova_align(iovad, size) >> shift;

	free_iova_fast(iovad, iova >> shift, iova_len);
}

static dma_addr_t
vduse_domain_map_page_bounce(struct vduse_iova_domain *domain,
			     struct page *page, unsigned long offset,
			     size_t size, enum dma_data_direction dir,
			     unsigned long attrs)
{
	struct iova_domain *iovad = &domain->stream_iovad;
	unsigned long limit = domain->bounce_size - 1;
	phys_addr_t pa = page_to_phys(page) + offset;
	dma_addr_t iova = vduse_domain_alloc_iova(iovad, size, limit);

	if (!iova)
		return DMA_MAPPING_ERROR;

	if (vduse_domain_init_bounce_map(domain))
		goto err;

	read_lock(&domain->bounce_lock);
	if (vduse_domain_map_bounce_page(domain, (u64)iova, (u64)size, pa))
		goto err_unlock;

	if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)
		vduse_domain_bounce(domain, iova, size, DMA_TO_DEVICE);

	read_unlock(&domain->bounce_lock);

	return iova;
err_unlock:
	read_unlock(&domain->bounce_lock);
err:
	vduse_domain_free_iova(iovad, iova, size);
	return DMA_MAPPING_ERROR;
}

static void
vduse_domain_unmap_page_bounce(struct vduse_iova_domain *domain,
			       dma_addr_t dma_addr, size_t size,
			       enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct iova_domain *iovad = &domain->stream_iovad;

	read_lock(&domain->bounce_lock);
	if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
		vduse_domain_bounce(domain, dma_addr, size, DMA_FROM_DEVICE);

	vduse_domain_unmap_bounce_page(domain, (u64)dma_addr, (u64)size);
	read_unlock(&domain->bounce_lock);
	vduse_domain_free_iova(iovad, dma_addr, size);
}

dma_addr_t vduse_domain_map_page(struct vduse_iova_domain *domain,
				 struct page *page, unsigned long offset,
				 size_t size, enum dma_data_direction dir,
				 unsigned long attrs)
{
	if (domain->enable_zc && size >= domain->zc_size)
		return vduse_domain_map_page_zc(domain, page, offset,
						size, dir, attrs);

	return vduse_domain_map_page_bounce(domain, page, offset,
					    size, dir, attrs);
}

void vduse_domain_unmap_page(struct vduse_iova_domain *domain,
			     dma_addr_t dma_addr, size_t size,
			     enum dma_data_direction dir,
			     unsigned long attrs)
{
	if (domain->enable_zc && size >= domain->zc_size)
		return vduse_domain_unmap_page_zc(domain, dma_addr,
						  size, dir, attrs);

	return vduse_domain_unmap_page_bounce(domain, dma_addr,
					      size, dir, attrs);
}

void *vduse_domain_alloc_coherent(struct vduse_iova_domain *domain,
				  size_t size, dma_addr_t *dma_addr,
				  gfp_t flag, unsigned long attrs)
{
	struct iova_domain *iovad = &domain->consistent_iovad;
	unsigned long limit = domain->iova_limit;
	dma_addr_t iova = vduse_domain_alloc_iova(iovad, size, limit);
	void *orig = alloc_pages_exact(size, flag);

	if (!iova || !orig)
		goto err;

	spin_lock(&domain->iotlb.lock);
	if (vduse_iotlb_add_range(&domain->iotlb, (u64)iova,
				  (u64)iova + size - 1,
				  virt_to_phys(orig), VHOST_MAP_RW,
				  domain->file, (u64)iova)) {
		spin_unlock(&domain->iotlb.lock);
		goto err;
	}
	spin_unlock(&domain->iotlb.lock);

	*dma_addr = iova;

	return orig;
err:
	*dma_addr = DMA_MAPPING_ERROR;
	if (orig)
		free_pages_exact(orig, size);
	if (iova)
		vduse_domain_free_iova(iovad, iova, size);

	return NULL;
}

void vduse_domain_free_coherent(struct vduse_iova_domain *domain, size_t size,
				void *vaddr, dma_addr_t dma_addr,
				unsigned long attrs)
{
	struct iova_domain *iovad = &domain->consistent_iovad;
	struct vhost_iotlb_map *map;
	struct vdpa_map_file *map_file;
	phys_addr_t pa;

	spin_lock(&domain->iotlb.lock);
	map = vhost_iotlb_itree_first(domain->iotlb.root, (u64)dma_addr,
				      (u64)dma_addr + size - 1);
	if (WARN_ON(!map)) {
		spin_unlock(&domain->iotlb.lock);
		return;
	}
	map_file = (struct vdpa_map_file *)map->opaque;
	fput(map_file->file);
	kfree(map_file);
	pa = map->addr;
	vhost_iotlb_map_free(domain->iotlb.root, map);
	spin_unlock(&domain->iotlb.lock);

	vduse_domain_free_iova(iovad, dma_addr, size);
	free_pages_exact(phys_to_virt(pa), size);
}

static vm_fault_t vduse_domain_mmap_fault(struct vm_fault *vmf)
{
	struct vduse_iova_domain *domain = vmf->vma->vm_private_data;
	unsigned long iova = vmf->pgoff << PAGE_SHIFT;
	struct page *page;

	if (!domain)
		return VM_FAULT_SIGBUS;

	if (iova < domain->bounce_size)
		page = vduse_domain_get_bounce_page(domain, iova);
	else
		page = vduse_domain_get_coherent_page(domain, iova);

	if (!page)
		return VM_FAULT_SIGBUS;

	vmf->page = page;

	return 0;
}

static const struct vm_operations_struct vduse_domain_mmap_ops = {
	.fault = vduse_domain_mmap_fault,
};

static int vduse_domain_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vduse_iova_domain *domain = file->private_data;

	vma->vm_flags |= VM_DONTDUMP | VM_DONTEXPAND;
	vma->vm_private_data = domain;
	vma->vm_ops = &vduse_domain_mmap_ops;

	return 0;
}

static int vduse_domain_release(struct inode *inode, struct file *file)
{
	struct vduse_iova_domain *domain = file->private_data;

	spin_lock(&domain->iotlb.lock);
	vduse_iotlb_del_range(&domain->iotlb, 0, ULLONG_MAX);
	vduse_domain_remove_user_bounce_pages(domain);
	vduse_domain_free_kernel_bounce_pages(domain);
	spin_unlock(&domain->iotlb.lock);
	put_iova_domain(&domain->stream_iovad);
	put_iova_domain(&domain->consistent_iovad);
	vduse_iotlb_deinit(&domain->iotlb);
	vfree(domain->bounce_maps);
	kfree(domain);

	return 0;
}

static const struct file_operations vduse_domain_fops = {
	.owner = THIS_MODULE,
	.mmap = vduse_domain_mmap,
	.read_iter = vduse_domain_read_iter,
	.write_iter = vduse_domain_write_iter,
	.release = vduse_domain_release,
};

void vduse_domain_destroy(struct vduse_iova_domain *domain)
{
	fput(domain->file);
}

struct vduse_iova_domain *
vduse_domain_create(unsigned long iova_limit, size_t bounce_size,
		    bool enable_zc, unsigned long zc_size)
{
	struct vduse_iova_domain *domain;
	struct file *file;
	struct vduse_bounce_map *map;
	unsigned long pfn, bounce_pfns;
	int ret;

	if (enable_zc && MAX_PHYSMEM_BITS >= BITS_PER_TYPE(dma_addr_t))
		return NULL;

	bounce_pfns = PAGE_ALIGN(bounce_size) >> PAGE_SHIFT;
	if (iova_limit <= bounce_size)
		return NULL;

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return NULL;

	if (vduse_iotlb_init(&domain->iotlb))
		goto err_iotlb;

	if (enable_zc && vduse_iotlb_init(&domain->iotlb_zc))
		goto err_iotlb_zc;

	domain->enable_zc = enable_zc;
	domain->zc_size = zc_size;
	domain->iova_limit = iova_limit;
	domain->bounce_size = PAGE_ALIGN(bounce_size);
	domain->bounce_maps = vzalloc(bounce_pfns *
				sizeof(struct vduse_bounce_map));
	if (!domain->bounce_maps)
		goto err_map;

	for (pfn = 0; pfn < bounce_pfns; pfn++) {
		map = &domain->bounce_maps[pfn];
		map->orig_phys = INVALID_PHYS_ADDR;
	}
	file = anon_inode_getfile("[vduse-domain]", &vduse_domain_fops,
				domain, O_RDWR);
	if (IS_ERR(file))
		goto err_file;

	file->f_mode |= (FMODE_PREAD | FMODE_PWRITE);
	domain->file = file;
	rwlock_init(&domain->bounce_lock);
	init_iova_domain(&domain->stream_iovad,
			PAGE_SIZE, IOVA_START_PFN);
	ret = iova_domain_init_rcaches(&domain->stream_iovad);
	if (ret)
		goto err_iovad_stream;
	init_iova_domain(&domain->consistent_iovad,
			PAGE_SIZE, bounce_pfns);
	ret = iova_domain_init_rcaches(&domain->consistent_iovad);
	if (ret)
		goto err_iovad_consistent;

	return domain;
err_iovad_consistent:
	put_iova_domain(&domain->stream_iovad);
err_iovad_stream:
	fput(file);
err_file:
	vfree(domain->bounce_maps);
err_map:
	vduse_iotlb_deinit(&domain->iotlb_zc);
err_iotlb_zc:
	vduse_iotlb_deinit(&domain->iotlb);
err_iotlb:
	kfree(domain);
	return NULL;
}

int vduse_domain_init(void)
{
	return iova_cache_get();
}

void vduse_domain_exit(void)
{
	iova_cache_put();
}
