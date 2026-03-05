/* SPDX-License-Identifier: GPL-2.0
 * GreenBoost v2.3 — Shared IOCTL definitions (kernel + userspace)
 *
 * Works with both #include <linux/ioctl.h> (kernel) and <sys/ioctl.h> (user).
 *
 * Author  : Ferran Duarri
 * License : GPL v2
 */
#ifndef GREENBOOST_IOCTL_H
#define GREENBOOST_IOCTL_H

#ifdef __KERNEL__
# include <linux/ioctl.h>
# include <linux/types.h>
  typedef __u64 gb_u64;
  typedef __u32 gb_u32;
  typedef __s32 gb_s32;
#else
# include <sys/ioctl.h>
# include <stdint.h>
  typedef uint64_t gb_u64;
  typedef uint32_t gb_u32;
  typedef int32_t  gb_s32;
#endif

/* Allocation flags — stored in gb_alloc_req.flags */
#define GB_ALLOC_WEIGHTS     (1u << 0)  /* model weight tensor              */
#define GB_ALLOC_KV_CACHE    (1u << 1)  /* KV cache page                    */
#define GB_ALLOC_ACTIVATIONS (1u << 2)  /* ephemeral activation buffer      */
#define GB_ALLOC_FROZEN      (1u << 3)  /* never evict from T2              */
#define GB_ALLOC_NO_HUGEPAGE (1u << 4)  /* force 4K (for T3-spillable)      */

/* Allocate a pinned DDR4 buffer; returns a DMA-BUF fd the GPU can import */
struct gb_alloc_req {
	gb_u64 size;    /* bytes to allocate          (in)  */
	gb_s32 fd;      /* DMA-BUF fd returned        (out) */
	gb_u32 flags;   /* GB_ALLOC_* flags           (in)  */
};

/* Pool statistics — three-tier memory hierarchy */
struct gb_info {
	/* Tier 1 — GPU VRAM (physical, managed by NVIDIA driver) */
	gb_u64 vram_physical_mb;   /* RTX 5070 physical VRAM             */

	/* Tier 2 — DDR4 RAM pool (pinned pages, DMA-BUF exported) */
	gb_u64 total_ram_mb;       /* total system RAM                   */
	gb_u64 free_ram_mb;        /* currently free RAM                 */
	gb_u64 allocated_mb;       /* bytes pinned by GreenBoost (T2)    */
	gb_u64 max_pool_mb;        /* virtual_vram_gb cap (T2)           */
	gb_u64 safety_reserve_mb;  /* always kept free (safety_reserve_gb) */
	gb_u64 available_mb;       /* free_ram - reserve - allocated     */
	gb_u32 active_buffers;     /* live DMA-BUF objects               */
	gb_u32 oom_active;         /* 1 if safety guard is triggered     */

	/* Tier 3 — NVMe swap (kernel-managed, model page overflow) */
	gb_u64 nvme_swap_total_mb; /* configured NVMe swap capacity      */
	gb_u64 nvme_swap_used_mb;  /* swap currently in use              */
	gb_u64 nvme_swap_free_mb;  /* swap available for model pages     */
	gb_u64 nvme_t3_allocated_mb; /* GreenBoost T3 allocations        */
	gb_u32 swap_pressure;      /* 0=ok 1=warn(>75%) 2=critical(>90%) */
	gb_u32 _pad;               /* alignment                          */

	/* Combined view */
	gb_u64 total_combined_mb;  /* VRAM + DDR4 pool + NVMe swap       */
};

/* Madvise request — advise the kernel on buffer eviction priority */
struct gb_madvise_req {
	gb_s32 buf_id;  /* buffer id from gb_alloc_req.fd → IDR lookup */
	gb_u32 advise;  /* GB_MADVISE_* constant                       */
};
#define GB_MADVISE_COLD   0   /* demote in LRU (evict sooner)        */
#define GB_MADVISE_HOT    1   /* promote to LRU head (evict later)   */
#define GB_MADVISE_FREEZE 2   /* pin — never evict while frozen      */

/* Evict request — push a T2 buffer to T3 (NVMe swap) immediately */
struct gb_evict_req {
	gb_s32 buf_id;
	gb_u32 _pad;
};

/* Poll-fd request — register a userspace eventfd to receive pressure events.
 * Userspace creates the eventfd: efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
 * then passes it here.  Kernel signals it whenever swap pressure changes.
 */
struct gb_poll_req {
	gb_s32 efd;     /* eventfd fd (in — created by caller)         */
	gb_u32 _pad;
};

/* Pin an existing user-space virtual buffer (allocated by shim) */
struct gb_pin_req {
	gb_u64 vaddr;   /* user space pointer (in)    */
	gb_u64 size;    /* bytes to pin       (in)    */
	gb_s32 fd;      /* DMA-BUF fd returned (out)  */
	gb_u32 flags;   /* GB_ALLOC_* flags   (in)    */
};

#define GB_IOCTL_MAGIC      'G'
#define GB_IOCTL_ALLOC      _IOWR(GB_IOCTL_MAGIC, 1, struct gb_alloc_req)
#define GB_IOCTL_GET_INFO   _IOR( GB_IOCTL_MAGIC, 2, struct gb_info)
#define GB_IOCTL_RESET      _IO(  GB_IOCTL_MAGIC, 3)
#define GB_IOCTL_MADVISE    _IOW( GB_IOCTL_MAGIC, 4, struct gb_madvise_req)
#define GB_IOCTL_EVICT      _IOW( GB_IOCTL_MAGIC, 5, struct gb_evict_req)
#define GB_IOCTL_POLL_FD    _IOW( GB_IOCTL_MAGIC, 7, struct gb_poll_req)
#define GB_IOCTL_PIN_USER_PTR _IOWR(GB_IOCTL_MAGIC, 8, struct gb_pin_req)

/* Swap pressure thresholds */
#define GB_SWAP_PRESSURE_OK       0
#define GB_SWAP_PRESSURE_WARN     1   /* >75% swap used */
#define GB_SWAP_PRESSURE_CRITICAL 2   /* >90% swap used */

#endif /* GREENBOOST_IOCTL_H */
