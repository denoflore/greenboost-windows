// SPDX-License-Identifier: GPL-2.0
/*
 * GreenBoost v2.3 — 3-Tier GPU Memory Pool via DMA-BUF + CUDA UVM
 *
 * Tier 1 — RTX 5070 GDDR7 12 GB        ~336 GB/s  (192-bit @ 14 Gbps)
 * Tier 2 — DDR4-3600 dual-ch 51 GB     ~57.6 GB/s local / ~32 GB/s via PCIe 4.0 x16
 * Tier 3 — NVMe swap 576 GB            ~7.25 GB/s seq / ~1.8 GB/s random swap
 *
 * CPU topology (i9-14900KF):
 *   P-cores : CPU 0-15  (8 phys × 2 HT, up to 6 GHz) — watchdog pinned here
 *   E-cores : CPU 16-31 (16 phys × 1,   up to 4.4 GHz)
 *   Golden  : CPU 4-7   (core_id 8,12 — actual 6 GHz TVB boost)
 *
 * Architecture:
 *   /dev/greenboost  — char device, IOCTLs
 *   GB_IOCTL_ALLOC   — pin DDR4 pages (T2) or swappable 4K (T3), export DMA-BUF fd
 *   GB_IOCTL_GET_INFO— 3-tier pool statistics
 *   sysfs            — pool_info, hw_info, active_buffers
 *   watchdog kthread — RAM safety + NVMe swap pressure, pinned to P-cores
 *
 * Hardware : ASRock B760M-ITX/D4 | i9-14900KF | RTX 5070 OC | 64 GB DDR4-3600
 * Kernel   : 6.19    NVIDIA driver : 580.x / CUDA 13
 * Author   : Ferran Duarri    License : GPL v2
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/dma-buf.h>        /* pulls in iosys-map.h */
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sysinfo.h>
#include <linux/idr.h>
#include <linux/swap.h>           /* get_nr_swap_pages()              */
#include <linux/cpumask.h>        /* cpumask_var_t, set_cpus_allowed  */
#include <linux/topology.h>       /* num_online_cpus()                */
#include <linux/eventfd.h>        /* eventfd_ctx_fdget, eventfd_signal */
#include "greenboost_ioctl.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ferran Duarri");
MODULE_DESCRIPTION("GreenBoost v2.3 — 3-tier pool: RTX5070 VRAM + DDR4 + NVMe swap");
MODULE_VERSION("2.3.0");
MODULE_IMPORT_NS("DMA_BUF");

/* 2 MiB hugepage constants */
#define GB_HPAGE_ORDER  9u

/* Memory tier identifiers */
enum gb_tier { GB_TIER2_DDR4 = 2, GB_TIER3_NVME = 3 };
#define GB_HPAGE_SIZE   (PAGE_SIZE << GB_HPAGE_ORDER)   /* 2 097 152 bytes  */
#define GB_HPAGES_PER   (1u << GB_HPAGE_ORDER)          /* 512 sub-pages    */

/* ------------------------------------------------------------------ */
/*  Names                                                               */
/* ------------------------------------------------------------------ */

#define DRIVER_NAME  "greenboost"
#define DEVICE_NAME  "greenboost"
#define CLASS_NAME   "greenboost"

/* ------------------------------------------------------------------ */
/*  Module parameters — 3-tier: RTX 5070 | DDR4 | NVMe swap           */
/* ------------------------------------------------------------------ */

/* Tier 1 */
static int physical_vram_gb  = 12;  /* RTX 5070 physical VRAM          */

/* Tier 2 */
static int virtual_vram_gb   = 51;  /* 80% of 64 GB DDR4               */
static int safety_reserve_gb = 12;  /* always keep ≥12 GB free in RAM  */

/* Tier 3 */
static int nvme_swap_gb      = 576; /* NVMe swap capacity (Samsung 990) */
static int nvme_pool_gb      = 512; /* GreenBoost soft cap on T3 use    */

/* CPU topology (i9-14900KF on ASRock B760M-ITX/D4) */
static int pcores_max_cpu    = 15;  /* last P-core logical CPU (0-15)   */
static int golden_cpu_min    =  4;  /* first golden-core CPU (6 GHz)    */
static int golden_cpu_max    =  7;  /* last  golden-core CPU (6 GHz)    */
static int pcores_only       =  1;  /* pin watchdog to P-cores          */

static int debug_mode        =  0;
static int use_hugepages     =  1;  /* 2 MB compound pages (T2 only)    */

module_param(physical_vram_gb,  int, 0444);
MODULE_PARM_DESC(physical_vram_gb,
	"Tier 1: Physical VRAM in GB — RTX 5070 default: 12");

module_param(virtual_vram_gb,   int, 0444);
MODULE_PARM_DESC(virtual_vram_gb,
	"Tier 2: DDR4 pool cap in GB — default: 51 (80% of 64 GB DDR4-3600)");

module_param(safety_reserve_gb, int, 0444);
MODULE_PARM_DESC(safety_reserve_gb,
	"Tier 2: Minimum free system RAM in GB — default: 12");

module_param(nvme_swap_gb,      int, 0444);
MODULE_PARM_DESC(nvme_swap_gb,
	"Tier 3: Total NVMe swap capacity in GB — default: 576 (Samsung 990 EVO Plus)");

module_param(nvme_pool_gb,      int, 0444);
MODULE_PARM_DESC(nvme_pool_gb,
	"Tier 3: GreenBoost soft cap on T3 allocations in GB — default: 512");

module_param(pcores_max_cpu,    int, 0444);
MODULE_PARM_DESC(pcores_max_cpu,
	"Highest P-core logical CPU number — i9-14900KF default: 15 (CPUs 0-15)");

module_param(golden_cpu_min,    int, 0444);
MODULE_PARM_DESC(golden_cpu_min,
	"First golden-core CPU (6 GHz TVB, i9-14900KF: cpu4)");

module_param(golden_cpu_max,    int, 0444);
MODULE_PARM_DESC(golden_cpu_max,
	"Last golden-core CPU (6 GHz TVB, i9-14900KF: cpu7)");

module_param(pcores_only,       int, 0444);
MODULE_PARM_DESC(pcores_only,
	"Pin watchdog kthread to P-cores only (1=yes, 0=any CPU) — default: 1");

module_param(debug_mode,        int, 0644);
MODULE_PARM_DESC(debug_mode,
	"Debug verbosity: 0=off 1=on");

module_param(use_hugepages,     int, 0444);
MODULE_PARM_DESC(use_hugepages,
	"Allocate 2 MB compound pages for lower TLB/DMA overhead (default: 1)");

#define gb_dbg(fmt, ...) \
	do { if (debug_mode) pr_info(DRIVER_NAME ": " fmt, ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/*  Per-buffer object                                                   */
/* ------------------------------------------------------------------ */

struct gb_buf {
	/* 4K page path */
	struct page    **pages;
	/* 2MB hugepage path */
	struct page    **hpages;
	unsigned int     nhpages;
	/* common */
	bool             hugepages;
	bool             user_pinned;    /* which path is active            */
	unsigned int     npages;      /* total in 4K units               */
	size_t           size;
	int              id;          /* IDR id (0 = not yet registered) */
	int              tier;        /* GB_TIER2_DDR4 or GB_TIER3_NVME  */
	struct dma_buf  *dmabuf;
	/* LRU and lifecycle fields (v2.3) */
	struct list_head  lru_node;     /* link in gb_device.lru_list    */
	unsigned long     alloc_jiffies;/* jiffies at alloc time         */
	unsigned long     last_jiffies; /* jiffies of last madvise HOT   */
	u32               alloc_flags;  /* GB_ALLOC_* flags              */
	u8                frozen;       /* 1 = never evict from T2       */
};

/* ------------------------------------------------------------------ */
/*  Global device state                                                 */
/* ------------------------------------------------------------------ */

struct gb_device {
	dev_t            devt;
	struct cdev      cdev;
	struct class    *cls;
	struct device   *dev;

	struct mutex     lock;            /* protects idr                   */
	struct idr       idr;             /* id → struct gb_buf *           */

	/* Tier 2 — DDR4 pool */
	atomic_t         active_bufs;     /* live DMA-BUF objects           */
	atomic64_t       pool_allocated;  /* bytes currently pinned (T2)    */
	atomic_t         oom_active;      /* 1 when safety guard tripped    */

	/* Tier 3 — NVMe swap */
	atomic64_t       nvme_allocated;  /* bytes in T3 allocations        */
	atomic_t         swap_pressure;   /* 0=ok 1=warn 2=critical         */

	struct task_struct *watchdog;
	/* LRU tracking (v2.3) */
	struct list_head    lru_list;     /* T2 buffers in LRU order       */
	spinlock_t          lru_lock;     /* protects lru_list             */
	struct eventfd_ctx *pressure_efd; /* signaled on pressure change   */
};

/* Read swap info (MB units).
 * total_swap_pages is not exported; use nvme_swap_gb module param as total.
 * get_nr_swap_pages() reads the exported nr_swap_pages atomic for free count.
 */
static void gb_read_swap_mb(u64 *total_mb, u64 *free_mb)
{
	*total_mb = (u64)nvme_swap_gb * 1024ULL;
	*free_mb  = ((u64)get_nr_swap_pages() * PAGE_SIZE) >> 20;
	/* clamp: free cannot exceed total */
	if (*free_mb > *total_mb)
		*free_mb = *total_mb;
}

static struct gb_device gb_dev;

/* ------------------------------------------------------------------ */
/*  DMA-BUF operations                                                  */
/* ------------------------------------------------------------------ */

static struct sg_table *gb_map_dma_buf(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct gb_buf *buf = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;
	unsigned int i;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (buf->hugepages) {
		/* Compact sg_table: one entry per 2 MB hugepage */
		ret = sg_alloc_table(sgt, buf->nhpages, GFP_KERNEL);
		if (ret) { kfree(sgt); return ERR_PTR(ret); }
		for (i = 0; i < buf->nhpages; i++)
			sg_set_page(&sgt->sgl[i], buf->hpages[i], GB_HPAGE_SIZE, 0);
	} else {
		ret = sg_alloc_table_from_pages(sgt, buf->pages, buf->npages,
						0, buf->size, GFP_KERNEL);
		if (ret) { kfree(sgt); return ERR_PTR(ret); }
	}

	ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	gb_dbg("mapped %zuMB (%s) for %s\n", buf->size >> 20,
	       buf->hugepages ? "2MB pages" : "4K pages",
	       dev_name(attach->dev));
	return sgt;
}

static void gb_unmap_dma_buf(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static void gb_release(struct dma_buf *dmabuf)
{
	struct gb_buf *buf = dmabuf->priv;
	unsigned int i;

	gb_dbg("release buffer id=%d size=%zuMB (%s)\n",
	       buf->id, buf->size >> 20,
	       buf->hugepages ? "2MB pages" : "4K pages");

	/* Remove from LRU list */
	spin_lock(&gb_dev.lru_lock);
	list_del_init(&buf->lru_node);
	spin_unlock(&gb_dev.lru_lock);

	/* Remove from IDR if registered */
	if (buf->id > 0) {
		mutex_lock(&gb_dev.lock);
		idr_remove(&gb_dev.idr, buf->id);
		mutex_unlock(&gb_dev.lock);
	}

	atomic_dec(&gb_dev.active_bufs);
	if (buf->tier == GB_TIER3_NVME)
		atomic64_sub(buf->size, &gb_dev.nvme_allocated);
	else
		atomic64_sub(buf->size, &gb_dev.pool_allocated);

	if (buf->user_pinned) {
		for (i = 0; i < buf->npages; i++)
			unpin_user_page(buf->pages[i]);
		kvfree(buf->pages);
	} else if (buf->hugepages) {
		for (i = 0; i < buf->nhpages; i++)
			__free_pages(buf->hpages[i], GB_HPAGE_ORDER);
		kvfree(buf->hpages);
	} else {
		for (i = 0; i < buf->npages; i++)
			__free_page(buf->pages[i]);
		kvfree(buf->pages);
	}
	kfree(buf);
}

static int gb_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct gb_buf *buf = dmabuf->priv;
	unsigned long addr = vma->vm_start;
	unsigned int i, j;
	int ret;

	if ((vma->vm_end - vma->vm_start) > buf->size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	if (buf->hugepages) {
		for (i = 0; i < buf->nhpages && addr < vma->vm_end; i++) {
			for (j = 0; j < GB_HPAGES_PER && addr < vma->vm_end;
			     j++, addr += PAGE_SIZE) {
				ret = vm_insert_page(vma, addr, buf->hpages[i] + j);
				if (ret)
					return ret;
			}
		}
	} else {
		for (i = 0; i < buf->npages && addr < vma->vm_end;
		     i++, addr += PAGE_SIZE) {
			ret = vm_insert_page(vma, addr, buf->pages[i]);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int gb_vmap_op(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct gb_buf *buf = dmabuf->priv;
	void *vaddr;

	if (buf->hugepages) {
		/* Expand compound pages to flat 4K array for vmap */
		struct page **tmp;
		unsigned int i, j, k = 0;

		tmp = kvmalloc_array(buf->npages, sizeof(*tmp), GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;
		for (i = 0; i < buf->nhpages; i++)
			for (j = 0; j < GB_HPAGES_PER; j++)
				tmp[k++] = buf->hpages[i] + j;
		vaddr = vmap(tmp, buf->npages, VM_MAP, PAGE_KERNEL);
		kvfree(tmp);
	} else {
		vaddr = vmap(buf->pages, buf->npages, VM_MAP, PAGE_KERNEL);
	}

	if (!vaddr)
		return -ENOMEM;

	iosys_map_set_vaddr(map, vaddr);
	return 0;
}

static void gb_vunmap_op(struct dma_buf *dmabuf, struct iosys_map *map)
{
	vunmap(map->vaddr);
	iosys_map_clear(map);
}

static const struct dma_buf_ops gb_dma_buf_ops = {
	.map_dma_buf   = gb_map_dma_buf,
	.unmap_dma_buf = gb_unmap_dma_buf,
	.release       = gb_release,
	.mmap          = gb_mmap,
	.vmap          = gb_vmap_op,
	.vunmap        = gb_vunmap_op,
};

/* ------------------------------------------------------------------ */
/*  Page pinning from userspace (FOLL_LONGTERM)                       */
/* ------------------------------------------------------------------ */

static struct gb_buf *gb_pin_user_buf(u64 vaddr, size_t size, u32 flags)
{
	struct gb_buf *buf;
	unsigned int np = DIV_ROUND_UP(size, PAGE_SIZE);
	size_t aligned_size = (size_t)np * PAGE_SIZE;
	int ret;
	unsigned int i;
	u64 t2_max = (u64)virtual_vram_gb * (1ULL << 30);
	u64 t2_used = (u64)atomic64_read(&gb_dev.pool_allocated);

	if (t2_used + aligned_size > t2_max) {
		pr_warn(DRIVER_NAME ": T2 cap reached for pinned memory\n");
		return ERR_PTR(-ENOMEM);
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	buf->pages = kvcalloc(np, sizeof(struct page *), GFP_KERNEL);
	if (!buf->pages) {
		kfree(buf);
		return ERR_PTR(-ENOMEM);
	}

	/* Pin the user pages with FOLL_LONGTERM so they can be safely used for DMA */
	mmap_read_lock(current->mm);
	ret = pin_user_pages(vaddr, np, FOLL_WRITE | FOLL_LONGTERM, buf->pages);
	mmap_read_unlock(current->mm);

	if (ret < 0 || ret != np) {
		if (ret > 0) {
			for (i = 0; i < ret; i++)
				unpin_user_page(buf->pages[i]);
		}
		kvfree(buf->pages);
		kfree(buf);
		pr_err(DRIVER_NAME ": pin_user_pages failed: %d\n", ret);
		return ERR_PTR(ret < 0 ? ret : -ENOMEM);
	}

	buf->hugepages = false;
	buf->user_pinned = true;
	buf->npages = np;
	buf->size = aligned_size;

	buf->id = 0;
	buf->tier = GB_TIER2_DDR4;
	buf->alloc_flags = flags;
	buf->alloc_jiffies = jiffies;
	buf->last_jiffies = jiffies;
	buf->frozen = (flags & GB_ALLOC_FROZEN) ? 1 : 0;
	INIT_LIST_HEAD(&buf->lru_node);

	atomic64_add(buf->size, &gb_dev.pool_allocated);
	atomic_inc(&gb_dev.active_bufs);

	spin_lock(&gb_dev.lru_lock);
	list_add_tail(&buf->lru_node, &gb_dev.lru_list);
	spin_unlock(&gb_dev.lru_lock);

	gb_dbg("pinned %u user pages (%zuMB)\n", np, aligned_size >> 20);
	return buf;
}

/* ------------------------------------------------------------------ */
/*  Page pool allocator                                                 */
/* ------------------------------------------------------------------ */

static struct gb_buf *gb_alloc_buf(size_t size, u32 flags)
{
	struct gb_buf *buf;
	unsigned int i, j;
	u64 t2_max    = (u64)virtual_vram_gb * (1ULL << 30);
	u64 t3_max    = (u64)nvme_pool_gb    * (1ULL << 30);
	u64 t2_used   = (u64)atomic64_read(&gb_dev.pool_allocated);
	u64 t3_used   = (u64)atomic64_read(&gb_dev.nvme_allocated);
	struct sysinfo si;
	u64 free_bytes, reserve_bytes;
	bool tier3 = false;

	/* Safety: reject if too little free RAM */
	si_meminfo(&si);
	free_bytes    = (u64)si.freeram * si.mem_unit;
	reserve_bytes = (u64)safety_reserve_gb * (1ULL << 30);

	if (free_bytes < reserve_bytes + size) {
		atomic_set(&gb_dev.oom_active, 1);
		pr_warn(DRIVER_NAME
			": OOM guard: free=%lluMB < reserve=%dGB + req=%zuMB\n",
			free_bytes >> 20, safety_reserve_gb, size >> 20);
		return ERR_PTR(-ENOMEM);
	}

	if (t2_used + size > t2_max) {
		/* Tier 2 (DDR4) full — try Tier 3 (NVMe-spillable) */
		if (t3_used + size > t3_max) {
			pr_warn(DRIVER_NAME
				": T3 NVMe cap reached — %dGB limit, used=%lluMB\n",
				nvme_pool_gb, t3_used >> 20);
			return ERR_PTR(-ENOSPC);
		}
		tier3 = true;
		gb_dbg("T2 DDR4 full (%lluMB/%dGB) — spilling to T3 NVMe\n",
		       t2_used >> 20, virtual_vram_gb);
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	/*
	 * Tier 3 (NVMe-spillable): force 4K pages, no hugepages.
	 * GFP_HIGHUSER allows the kernel to reclaim/swap these pages
	 * to NVMe under memory pressure — that IS the spill mechanism.
	 * Tier 2 (DDR4): use hugepages for lower DMA scatter overhead.
	 */
	if (tier3)
		goto alloc_4k;

	/* --- 2 MB hugepage path (Tier 2 only) --- */
	if (use_hugepages && !(flags & GB_ALLOC_NO_HUGEPAGE)) {
		unsigned int nhp = DIV_ROUND_UP(size, GB_HPAGE_SIZE);
		size_t hsize    = (size_t)nhp * GB_HPAGE_SIZE;

		buf->hpages = kvcalloc(nhp, sizeof(struct page *), GFP_KERNEL);
		if (!buf->hpages)
			goto fallback_4k;

		for (i = 0; i < nhp; i++) {
			buf->hpages[i] = alloc_pages(
				GFP_KERNEL | __GFP_ZERO | __GFP_COMP | __GFP_NOWARN,
				GB_HPAGE_ORDER);
			if (!buf->hpages[i]) {
				for (j = 0; j < i; j++)
					__free_pages(buf->hpages[j], GB_HPAGE_ORDER);
				kvfree(buf->hpages);
				buf->hpages = NULL;
				gb_dbg("hugepage alloc failed at %u/%u, falling back to 4K\n",
				       i, nhp);
				goto fallback_4k;
			}
		}

		buf->hugepages = true;
		buf->nhpages   = nhp;
		buf->npages    = nhp * GB_HPAGES_PER;
		buf->size      = hsize;
		gb_dbg("allocated %u hugepages (%zuMB, 2MB pages)\n", nhp, hsize >> 20);
		goto done;
	}

fallback_4k:
alloc_4k:
	{
		unsigned int np = DIV_ROUND_UP(size, PAGE_SIZE);
		size = (size_t)np * PAGE_SIZE;

		buf->pages = kvcalloc(np, sizeof(struct page *), GFP_KERNEL);
		if (!buf->pages) { kfree(buf); return ERR_PTR(-ENOMEM); }

		for (i = 0; i < np; i++) {
			buf->pages[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
			if (!buf->pages[i]) {
				for (j = 0; j < i; j++)
					__free_page(buf->pages[j]);
				kvfree(buf->pages);
				kfree(buf);
				return ERR_PTR(-ENOMEM);
			}
		}
		buf->hugepages = false;
		buf->npages    = np;
		buf->size      = size;
		gb_dbg("allocated %u pages (%zuMB, 4K %s)\n",
		       np, size >> 20, tier3 ? "NVMe-spillable" : "DDR4");
	}

done:
	buf->id          = 0;
	buf->tier        = tier3 ? GB_TIER3_NVME : GB_TIER2_DDR4;
	buf->alloc_flags = flags;
	buf->alloc_jiffies = jiffies;
	buf->last_jiffies  = jiffies;
	buf->frozen      = (flags & GB_ALLOC_FROZEN) ? 1 : 0;
	INIT_LIST_HEAD(&buf->lru_node);

	if (tier3)
		atomic64_add(buf->size, &gb_dev.nvme_allocated);
	else
		atomic64_add(buf->size, &gb_dev.pool_allocated);
	atomic_inc(&gb_dev.active_bufs);

	spin_lock(&gb_dev.lru_lock);
	list_add_tail(&buf->lru_node, &gb_dev.lru_list);
	spin_unlock(&gb_dev.lru_lock);

	return buf;
}

/* ------------------------------------------------------------------ */
/*  IOCTL                                                               */
/* ------------------------------------------------------------------ */

static long gb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {

	case GB_IOCTL_ALLOC: {
		struct gb_alloc_req req;
		struct gb_buf *buf;
		DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
		struct dma_buf *dmabuf;
		int id, fd;
		unsigned int j;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		if (!req.size ||
		    req.size > (u64)virtual_vram_gb * (1ULL << 30))
			return -EINVAL;

		/* 1. Pin DDR4 pages */
		buf = gb_alloc_buf((size_t)req.size, req.flags);
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		/* 2. Export as DMA-BUF */
		exp_info.ops   = &gb_dma_buf_ops;
		exp_info.size  = buf->size;
		exp_info.flags = O_RDWR | O_CLOEXEC;
		exp_info.priv  = buf;

		dmabuf = dma_buf_export(&exp_info);
		if (IS_ERR(dmabuf)) {
			/* gb_release won't be called — undo manually */
			atomic_dec(&gb_dev.active_bufs);
			atomic64_sub(buf->size, &gb_dev.pool_allocated);
			for (j = 0; j < buf->npages; j++)
				__free_page(buf->pages[j]);
			kvfree(buf->pages);
			kfree(buf);
			return PTR_ERR(dmabuf);
		}
		buf->dmabuf = dmabuf;

		/* 3. Register in IDR */
		mutex_lock(&gb_dev.lock);
		id = idr_alloc(&gb_dev.idr, buf, 1, 0, GFP_KERNEL);
		mutex_unlock(&gb_dev.lock);

		if (id < 0) {
			/* dma_buf_put triggers gb_release; buf->id==0 → no IDR remove */
			dma_buf_put(dmabuf);
			return id;
		}
		buf->id = id;

		/* 4. Install fd in caller's file table */
		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			mutex_lock(&gb_dev.lock);
			idr_remove(&gb_dev.idr, buf->id);
			mutex_unlock(&gb_dev.lock);
			buf->id = 0;
			dma_buf_put(dmabuf); /* triggers gb_release → frees pages */
			return fd;
		}

		req.fd = fd;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT; /* fd already installed, caller must close it */

		pr_info(DRIVER_NAME
			": allocated %zuMB buffer (id=%d fd=%d)\n",
			buf->size >> 20, buf->id, fd);
		return 0;
	}

	case GB_IOCTL_PIN_USER_PTR: {
		struct gb_pin_req req;
		struct gb_buf *buf;
		DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
		struct dma_buf *dmabuf;
		int id, fd;
		unsigned int j;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		if (!req.size || !req.vaddr ||
		    req.size > (u64)virtual_vram_gb * (1ULL << 30))
			return -EINVAL;

		/* 1. Pin user memory */
		buf = gb_pin_user_buf((u64)req.vaddr, (size_t)req.size, req.flags);
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		/* 2. Export as DMA-BUF */
		exp_info.ops   = &gb_dma_buf_ops;
		exp_info.size  = buf->size;
		exp_info.flags = O_RDWR | O_CLOEXEC;
		exp_info.priv  = buf;

		dmabuf = dma_buf_export(&exp_info);
		if (IS_ERR(dmabuf)) {
			atomic_dec(&gb_dev.active_bufs);
			atomic64_sub(buf->size, &gb_dev.pool_allocated);
			for (j = 0; j < buf->npages; j++)
				unpin_user_page(buf->pages[j]);
			kvfree(buf->pages);
			kfree(buf);
			return PTR_ERR(dmabuf);
		}
		buf->dmabuf = dmabuf;

		/* 3. Register in IDR */
		mutex_lock(&gb_dev.lock);
		id = idr_alloc(&gb_dev.idr, buf, 1, 0, GFP_KERNEL);
		mutex_unlock(&gb_dev.lock);

		if (id < 0) {
			dma_buf_put(dmabuf);
			return id;
		}
		buf->id = id;

		/* 4. Install fd in caller's file table */
		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			mutex_lock(&gb_dev.lock);
			idr_remove(&gb_dev.idr, buf->id);
			mutex_unlock(&gb_dev.lock);
			buf->id = 0;
			dma_buf_put(dmabuf);
			return fd;
		}

		req.fd = fd;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;

		pr_info(DRIVER_NAME
			": pinned %zuMB user buffer (id=%d fd=%d)\n",
			buf->size >> 20, buf->id, fd);
		return 0;
	}

	case GB_IOCTL_GET_INFO: {
		struct gb_info info;
		struct sysinfo si;
		u64 free_bytes, reserve_bytes, alloc_bytes;
		u64 swap_total_mb, swap_free_mb, swap_used_mb;

		si_meminfo(&si);
		gb_read_swap_mb(&swap_total_mb, &swap_free_mb);

		free_bytes    = (u64)si.freeram  * si.mem_unit;
		reserve_bytes = (u64)safety_reserve_gb * (1ULL << 30);
		alloc_bytes   = (u64)atomic64_read(&gb_dev.pool_allocated);
		swap_used_mb  = swap_total_mb - swap_free_mb;

		memset(&info, 0, sizeof(info));

		/* Tier 1 */
		info.vram_physical_mb    = (u64)physical_vram_gb * 1024ULL;

		/* Tier 2 */
		info.total_ram_mb        = ((u64)si.totalram * si.mem_unit) >> 20;
		info.free_ram_mb         = free_bytes >> 20;
		info.allocated_mb        = alloc_bytes >> 20;
		info.max_pool_mb         = (u64)virtual_vram_gb * 1024ULL;
		info.safety_reserve_mb   = reserve_bytes >> 20;
		info.available_mb        = (free_bytes > reserve_bytes + alloc_bytes)
			? (free_bytes - reserve_bytes - alloc_bytes) >> 20 : 0;
		info.active_buffers      = (gb_u32)atomic_read(&gb_dev.active_bufs);
		info.oom_active          = (gb_u32)atomic_read(&gb_dev.oom_active);

		/* Tier 3 */
		info.nvme_swap_total_mb  = (u64)nvme_swap_gb * 1024ULL;
		info.nvme_swap_used_mb   = swap_used_mb;
		info.nvme_swap_free_mb   = swap_free_mb;
		info.nvme_t3_allocated_mb =
			(u64)atomic64_read(&gb_dev.nvme_allocated) >> 20;
		info.swap_pressure       = (gb_u32)atomic_read(&gb_dev.swap_pressure);

		/* Combined */
		info.total_combined_mb   = info.vram_physical_mb
					 + info.max_pool_mb
					 + info.nvme_swap_total_mb;

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case GB_IOCTL_RESET:
		pr_info(DRIVER_NAME
			": RESET — close all DMA-BUF fds to release buffers\n");
		return 0;

	case GB_IOCTL_MADVISE: {
		struct gb_madvise_req req;
		struct gb_buf *buf;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		mutex_lock(&gb_dev.lock);
		buf = idr_find(&gb_dev.idr, req.buf_id);
		mutex_unlock(&gb_dev.lock);
		if (!buf)
			return -ENOENT;

		spin_lock(&gb_dev.lru_lock);
		switch (req.advise) {
		case GB_MADVISE_HOT:
			buf->last_jiffies = jiffies;
			list_move(&buf->lru_node, &gb_dev.lru_list);  /* to head */
			break;
		case GB_MADVISE_COLD:
			list_move_tail(&buf->lru_node, &gb_dev.lru_list); /* to tail */
			break;
		case GB_MADVISE_FREEZE:
			buf->frozen = 1;
			break;
		default:
			spin_unlock(&gb_dev.lru_lock);
			return -EINVAL;
		}
		spin_unlock(&gb_dev.lru_lock);
		gb_dbg("madvise buf id=%d advise=%u\n", req.buf_id, req.advise);
		return 0;
	}

	case GB_IOCTL_EVICT: {
		struct gb_evict_req req;
		struct gb_buf *buf;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		mutex_lock(&gb_dev.lock);
		buf = idr_find(&gb_dev.idr, req.buf_id);
		mutex_unlock(&gb_dev.lock);
		if (!buf)
			return -ENOENT;

		/* Move T2 accounting to T3 (kernel reclaim handles actual page-out
		 * for GFP_HIGHUSER 4K pages; hugepage T2 buffers get soft-evicted) */
		if (buf->tier == GB_TIER2_DDR4) {
			atomic64_sub(buf->size, &gb_dev.pool_allocated);
			atomic64_add(buf->size, &gb_dev.nvme_allocated);
			buf->tier = GB_TIER3_NVME;
			/* Remove from LRU — evicted buffers are not candidates */
			spin_lock(&gb_dev.lru_lock);
			list_del_init(&buf->lru_node);
			spin_unlock(&gb_dev.lru_lock);
			gb_dbg("evict buf id=%d: T2→T3 (%zuMB)\n",
			       buf->id, buf->size >> 20);
		}
		return 0;
	}

	case GB_IOCTL_POLL_FD: {
		struct gb_poll_req req;
		struct eventfd_ctx *ctx;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		ctx = eventfd_ctx_fdget(req.efd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);

		/* Replace any previously registered eventfd */
		if (gb_dev.pressure_efd)
			eventfd_ctx_put(gb_dev.pressure_efd);
		gb_dev.pressure_efd = ctx;
		gb_dbg("pressure eventfd registered (efd=%d)\n", req.efd);
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/*  Sysfs attributes                                                    */
/* ------------------------------------------------------------------ */

static ssize_t pool_info_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct sysinfo si;
	u64 ram_total_mb, ram_free_mb, t2_alloc_mb, t2_avail_mb, reserve_mb;
	u64 swap_total_mb, swap_free_mb, swap_used_mb, t3_alloc_mb;
	u64 combined_mb;
	int pressure;
	const char *pressure_str;

	si_meminfo(&si);
	gb_read_swap_mb(&swap_total_mb, &swap_free_mb);

	ram_total_mb = ((u64)si.totalram * si.mem_unit) >> 20;
	ram_free_mb  = ((u64)si.freeram  * si.mem_unit) >> 20;
	t2_alloc_mb  = (u64)atomic64_read(&gb_dev.pool_allocated) >> 20;
	t3_alloc_mb  = (u64)atomic64_read(&gb_dev.nvme_allocated) >> 20;
	reserve_mb   = (u64)safety_reserve_gb * 1024ULL;
	swap_used_mb = swap_total_mb - swap_free_mb;
	combined_mb  = (u64)physical_vram_gb * 1024ULL
		     + (u64)virtual_vram_gb  * 1024ULL
		     + (u64)nvme_swap_gb     * 1024ULL;
	t2_avail_mb  = (ram_free_mb > reserve_mb + t2_alloc_mb)
		       ? ram_free_mb - reserve_mb - t2_alloc_mb : 0;
	pressure     = atomic_read(&gb_dev.swap_pressure);
	pressure_str = (pressure == GB_SWAP_PRESSURE_CRITICAL) ? "CRITICAL (>90%)" :
		       (pressure == GB_SWAP_PRESSURE_WARN)     ? "warn (>75%)"    :
		                                                  "ok";

	return sysfs_emit(buf,
		"=== GreenBoost v2.3 — 3-Tier Pool Info ===\n"
		"\n"
		"Tier 1  RTX 5070 VRAM      : %4d GB   ~336 GB/s  GDDR7 192-bit  [hot layers]\n"
		"Tier 2  DDR4 pool cap      : %4d GB   ~57.6 GB/s dual-ch / ~32 GB/s PCIe DMA  [cold layers]\n"
		"Tier 3  NVMe swap          : %4d GB   ~7.25 GB/s seq / ~1.8 GB/s swap  [frozen pages]\n"
		"        ─────────────────────────────────\n"
		"        Combined model view: %4llu GB\n"
		"\n"
		"── Tier 2 (DDR4) ──────────────────────────\n"
		"  Total RAM                : %llu MB\n"
		"  Free RAM                 : %llu MB\n"
		"  Safety reserve           : %llu MB\n"
		"  T2 allocated             : %llu MB\n"
		"  T2 available             : %llu MB\n"
		"  Active DMA-BUF objects   : %d\n"
		"  OOM guard                : %s\n"
		"  Page mode                : %s\n"
		"\n"
		"── Tier 3 (NVMe swap) ──────────────────────\n"
		"  Swap total               : %llu MB  (%d GB configured)\n"
		"  Swap used                : %llu MB\n"
		"  Swap free                : %llu MB\n"
		"  T3 GreenBoost alloc      : %llu MB\n"
		"  Swap pressure            : %s\n",
		physical_vram_gb,
		virtual_vram_gb,
		nvme_swap_gb,
		combined_mb / 1024ULL,
		ram_total_mb, ram_free_mb, reserve_mb,
		t2_alloc_mb, t2_avail_mb,
		atomic_read(&gb_dev.active_bufs),
		atomic_read(&gb_dev.oom_active) ? "YES" : "no",
		use_hugepages ? "2 MB hugepages (T2) / 4K swappable (T3)"
			      : "4 KB pages",
		swap_total_mb, nvme_swap_gb,
		swap_used_mb, swap_free_mb,
		t3_alloc_mb,
		pressure_str);
}
static DEVICE_ATTR_RO(pool_info);

static ssize_t active_buffers_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&gb_dev.active_bufs));
}
static DEVICE_ATTR_RO(active_buffers);

static ssize_t hw_info_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"=== GreenBoost v2.3 — Hardware Topology ===\n"
		"\n"
		"CPU  i9-14900KF (Raptor Lake, LGA1700)\n"
		"  Logical CPUs      : %d total\n"
		"  P-cores (HT)      : CPU 0-%d  (8 phys × 2 HT, up to 6 GHz)\n"
		"  Golden cores      : CPU %d-%d  (core_id 8,12 — 6 GHz TVB)\n"
		"  E-cores           : CPU %d-end (16 phys × 1, up to 4.4 GHz)\n"
		"  Watchdog pinned   : P-cores (CPU 0-%d) = %s\n"
		"  L3 cache          : 36 MB shared\n"
		"  L2 cache          : 32 MB (2 MB per P-core cluster)\n"
		"\n"
		"RAM  Crucial Ballistix DDR4-3600 (2×32 GB, dual-channel, XMP 2.0)\n"
		"  Configuration     : 2 DIMMs, dual-channel, CL16, 1.35V\n"
		"  Local bandwidth   : ~57.6 GB/s (2×64-bit @ 3600 MT/s)\n"
		"  PCIe 4.0 x16 DMA  : ~32 GB/s (GPU↔DDR4 pool)\n"
		"\n"
		"GPU  ASUS RTX 5070 OC (GB205, 12 GB GDDR7)\n"
		"  VRAM bandwidth    : ~336 GB/s (192-bit bus @ 14 Gbps)\n"
		"  SM boost clock    : 2610 MHz (OC) / boost observed: 3090 MHz\n"
		"  PCIe slot         : Gen 4 x16 (ASRock B760M-ITX/D4)\n"
		"\n"
		"NVMe  Samsung 990 EVO Plus 4 TB (PM9C1a, PCIe 4.0 x4 / 5.0 x2)\n"
		"  Sequential read   : ~7,250 MB/s\n"
		"  Sequential write  : ~6,300 MB/s\n"
		"  Swap random I/O   : ~1.8 GB/s (T3 page-in/out)\n"
		"\n"
		"Motherboard  ASRock B760M-ITX/D4  (5+1+1 power phases)\n"
		"  PCIe slots        : 1× Gen 4 x16 (GPU), 1× M.2 Gen 4 x4 (NVMe)\n"
		"  NUMA nodes        : 1 (node 0, all CPUs)\n"
		"\n"
		"GreenBoost kthread affinity\n"
		"  pcores_only       : %d\n"
		"  pcores_max_cpu    : %d  (CPUs 0-%d = P-cores)\n"
		"  golden_cpu_min/max: %d / %d\n",
		num_online_cpus(),
		pcores_max_cpu,
		golden_cpu_min, golden_cpu_max,
		pcores_max_cpu + 1,
		pcores_max_cpu,
		pcores_only ? "yes" : "no (all CPUs)",
		pcores_only, pcores_max_cpu, pcores_max_cpu,
		golden_cpu_min, golden_cpu_max);
}
static DEVICE_ATTR_RO(hw_info);

static struct attribute *gb_attrs[] = {
	&dev_attr_pool_info.attr,
	&dev_attr_hw_info.attr,
	&dev_attr_active_buffers.attr,
	NULL,
};
ATTRIBUTE_GROUPS(gb);

/* ------------------------------------------------------------------ */
/*  Watchdog kthread — enforces safety reserve                         */
/* ------------------------------------------------------------------ */

static int gb_watchdog(void *unused)
{
	struct sysinfo si;
	u64 free_bytes, reserve_bytes;
	u64 swap_total_mb, swap_free_mb, swap_used_pct;
	int new_pressure, old_pressure;

	pr_info(DRIVER_NAME ": watchdog started (500ms, T2 RAM + T3 NVMe)\n");

	while (!kthread_should_stop()) {
		msleep_interruptible(500);

		/* ── Tier 2: RAM safety reserve ─────────────────────── */
		si_meminfo(&si);
		free_bytes    = (u64)si.freeram * si.mem_unit;
		reserve_bytes = (u64)safety_reserve_gb * (1ULL << 30);

		if (free_bytes < reserve_bytes) {
			if (!atomic_read(&gb_dev.oom_active)) {
				atomic_set(&gb_dev.oom_active, 1);
				pr_warn(DRIVER_NAME
					": T2 OOM guard TRIPPED — "
					"free=%lluMB < reserve=%dGB\n",
					free_bytes >> 20, safety_reserve_gb);
			}
		} else {
			if (atomic_read(&gb_dev.oom_active)) {
				atomic_set(&gb_dev.oom_active, 0);
				pr_info(DRIVER_NAME
					": T2 OOM guard cleared — "
					"free=%lluMB\n",
					free_bytes >> 20);
			}
		}

		/* ── Tier 3: NVMe swap pressure ─────────────────────── */
		gb_read_swap_mb(&swap_total_mb, &swap_free_mb);
		if (swap_total_mb > 0) {
			swap_used_pct = ((swap_total_mb - swap_free_mb) * 100ULL)
					/ swap_total_mb;
			if (swap_used_pct >= 90)
				new_pressure = GB_SWAP_PRESSURE_CRITICAL;
			else if (swap_used_pct >= 75)
				new_pressure = GB_SWAP_PRESSURE_WARN;
			else
				new_pressure = GB_SWAP_PRESSURE_OK;

			old_pressure = atomic_read(&gb_dev.swap_pressure);
			if (new_pressure != old_pressure) {
				atomic_set(&gb_dev.swap_pressure, new_pressure);
				if (gb_dev.pressure_efd)
					eventfd_signal(gb_dev.pressure_efd);
				if (new_pressure == GB_SWAP_PRESSURE_CRITICAL)
					pr_warn(DRIVER_NAME
						": T3 NVMe swap CRITICAL — "
						"%llu%% used (%lluMB/%lluMB)\n",
						swap_used_pct,
						swap_total_mb - swap_free_mb,
						swap_total_mb);
				else if (new_pressure == GB_SWAP_PRESSURE_WARN)
					pr_warn(DRIVER_NAME
						": T3 NVMe swap warn — "
						"%llu%% used\n", swap_used_pct);
				else
					pr_info(DRIVER_NAME
						": T3 NVMe swap pressure cleared\n");
			}
		}
	}

	pr_info(DRIVER_NAME ": watchdog stopped\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/*  File operations                                                     */
/* ------------------------------------------------------------------ */

static int gb_open(struct inode *inode, struct file *file)
{
	gb_dbg("opened\n");
	return 0;
}

static int gb_close(struct inode *inode, struct file *file)
{
	gb_dbg("closed\n");
	return 0;
}

static const struct file_operations gb_fops = {
	.owner          = THIS_MODULE,
	.open           = gb_open,
	.release        = gb_close,
	.unlocked_ioctl = gb_ioctl,
};

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init gb_init(void)
{
	int ret;

	pr_info(DRIVER_NAME ": =====================================================\n");
	pr_info(DRIVER_NAME ": GreenBoost v2.3 — 3-Tier GPU Memory Pool (LRU+eventfd)\n");
	pr_info(DRIVER_NAME ": Author  : Ferran Duarri\n");
	pr_info(DRIVER_NAME ": CPU     : i9-14900KF — P-cores CPU 0-%d (golden %d-%d @ 6GHz)\n",
		pcores_max_cpu, golden_cpu_min, golden_cpu_max);
	pr_info(DRIVER_NAME ": T1 VRAM : RTX 5070 OC   %d GB  ~336 GB/s GDDR7\n",
		physical_vram_gb);
	pr_info(DRIVER_NAME ": T2 DDR4 : pool cap       %d GB  ~57.6 GB/s dual-ch / ~32 GB/s PCIe  (reserve %d GB)\n",
		virtual_vram_gb, safety_reserve_gb);
	pr_info(DRIVER_NAME ": T3 NVMe : 990 EVO Plus   %d GB  ~7.25 GB/s seq / ~1.8 GB/s swap  (cap %d GB)\n",
		nvme_swap_gb, nvme_pool_gb);
	pr_info(DRIVER_NAME ": Combined: %d GB total model capacity\n",
		physical_vram_gb + virtual_vram_gb + nvme_swap_gb);
	pr_info(DRIVER_NAME ": =====================================================\n");

	mutex_init(&gb_dev.lock);
	idr_init(&gb_dev.idr);
	atomic_set(&gb_dev.active_bufs, 0);
	atomic64_set(&gb_dev.pool_allocated, 0);
	atomic64_set(&gb_dev.nvme_allocated, 0);
	atomic_set(&gb_dev.oom_active, 0);
	atomic_set(&gb_dev.swap_pressure, GB_SWAP_PRESSURE_OK);
	INIT_LIST_HEAD(&gb_dev.lru_list);
	spin_lock_init(&gb_dev.lru_lock);
	gb_dev.pressure_efd = NULL;

	/* Allocate character device region */
	ret = alloc_chrdev_region(&gb_dev.devt, 0, 1, DRIVER_NAME);
	if (ret) {
		pr_err(DRIVER_NAME ": alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&gb_dev.cdev, &gb_fops);
	gb_dev.cdev.owner = THIS_MODULE;

	ret = cdev_add(&gb_dev.cdev, gb_dev.devt, 1);
	if (ret) {
		pr_err(DRIVER_NAME ": cdev_add failed: %d\n", ret);
		goto err_chrdev;
	}

	gb_dev.cls = class_create(CLASS_NAME);
	if (IS_ERR(gb_dev.cls)) {
		ret = PTR_ERR(gb_dev.cls);
		pr_err(DRIVER_NAME ": class_create failed: %d\n", ret);
		goto err_cdev;
	}

	gb_dev.dev = device_create_with_groups(gb_dev.cls, NULL,
					       gb_dev.devt, NULL,
					       gb_groups,
					       DEVICE_NAME);
	if (IS_ERR(gb_dev.dev)) {
		ret = PTR_ERR(gb_dev.dev);
		pr_err(DRIVER_NAME ": device_create failed: %d\n", ret);
		goto err_class;
	}

	/* Start watchdog kthread */
	gb_dev.watchdog = kthread_run(gb_watchdog, NULL, "gb_watchdog");
	if (IS_ERR(gb_dev.watchdog)) {
		ret = PTR_ERR(gb_dev.watchdog);
		pr_err(DRIVER_NAME ": kthread_run failed: %d\n", ret);
		gb_dev.watchdog = NULL;
		goto err_device;
	}

	/* Pin watchdog to E-cores to avoid stealing cycles from P-cores.
	 * E-cores (CPUs > pcores_max_cpu) run at 4.4 GHz vs 6 GHz.
	 * We want to keep the Golden P-cores (4-7) free for inference.
	 */
	if (pcores_only) {
		cpumask_var_t emask;
		int cpu;

		if (alloc_cpumask_var(&emask, GFP_KERNEL)) {
			cpumask_clear(emask);
			for (cpu = pcores_max_cpu + 1; cpu < nr_cpu_ids; cpu++) {
				if (cpu_online(cpu))
					cpumask_set_cpu(cpu, emask);
			}
			/* Fallback to all CPUs if no E-cores found */
			if (cpumask_empty(emask)) {
				for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
					if (cpu_online(cpu))
						cpumask_set_cpu(cpu, emask);
				}
			}
			set_cpus_allowed_ptr(gb_dev.watchdog, emask);
			free_cpumask_var(emask);
			pr_info(DRIVER_NAME
				": watchdog pinned away from P-cores (to E-cores %d-%d) "
				"to reserve golden P-cores (%d-%d) for inference\n",
				pcores_max_cpu + 1, nr_cpu_ids - 1,
				golden_cpu_min, golden_cpu_max);
		}
	}

	pr_info(DRIVER_NAME ": ready — /dev/%s\n", DEVICE_NAME);
	pr_info(DRIVER_NAME ": pool info: cat /sys/class/%s/%s/pool_info\n",
		CLASS_NAME, DEVICE_NAME);
	return 0;

err_device:
	device_destroy(gb_dev.cls, gb_dev.devt);
err_class:
	class_destroy(gb_dev.cls);
err_cdev:
	cdev_del(&gb_dev.cdev);
err_chrdev:
	unregister_chrdev_region(gb_dev.devt, 1);
	return ret;
}

static void __exit gb_exit(void)
{
	pr_info(DRIVER_NAME ": unloading GreenBoost v2.3\n");

	if (gb_dev.pressure_efd) {
		eventfd_ctx_put(gb_dev.pressure_efd);
		gb_dev.pressure_efd = NULL;
	}

	if (gb_dev.watchdog) {
		kthread_stop(gb_dev.watchdog);
		gb_dev.watchdog = NULL;
	}

	device_destroy(gb_dev.cls, gb_dev.devt);
	class_destroy(gb_dev.cls);
	cdev_del(&gb_dev.cdev);
	unregister_chrdev_region(gb_dev.devt, 1);
	idr_destroy(&gb_dev.idr);

	pr_info(DRIVER_NAME ": unloaded cleanly\n");
}

module_init(gb_init);
module_exit(gb_exit);
