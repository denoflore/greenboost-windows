#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xd648ae19, "sg_free_table" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xf1de9e85, "vunmap" },
	{ 0xf46d5bf3, "mutex_init_generic" },
	{ 0x9f222e1e, "alloc_chrdev_region" },
	{ 0xaa9455c8, "cdev_init" },
	{ 0x5ba4a3be, "cdev_add" },
	{ 0x6cd71bc1, "class_create" },
	{ 0xe48c248b, "device_create_with_groups" },
	{ 0x126bc160, "kthread_create_on_node" },
	{ 0x6f001536, "wake_up_process" },
	{ 0xc6b90fc4, "device_destroy" },
	{ 0xf2aabb37, "alloc_cpumask_var_node" },
	{ 0xf296206e, "nr_cpu_ids" },
	{ 0xb5c51982, "__cpu_online_mask" },
	{ 0x494c552b, "_find_first_bit" },
	{ 0x737468de, "set_cpus_allowed_ptr" },
	{ 0x02b7fa5b, "free_cpumask_var" },
	{ 0x69a29051, "class_destroy" },
	{ 0xd1ed87cb, "cdev_del" },
	{ 0x0bc5fb0d, "unregister_chrdev_region" },
	{ 0xe0418251, "eventfd_ctx_put" },
	{ 0x37ef79a4, "kthread_stop" },
	{ 0x255dfd5a, "idr_destroy" },
	{ 0xde338d9a, "_raw_spin_lock" },
	{ 0xde338d9a, "_raw_spin_unlock" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0x07d50c57, "idr_remove" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0x7ab1aa5e, "unpin_user_page" },
	{ 0xf1de9e85, "kvfree" },
	{ 0x2684a94a, "__free_pages" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0xdd1ad581, "__kvmalloc_node_noprof" },
	{ 0x1bdf2bc8, "sme_me_mask" },
	{ 0x5fc55113, "__default_kernel_pte_mask" },
	{ 0x70670de1, "vmap" },
	{ 0x5e865cb8, "pgprot_writecombine" },
	{ 0xb6d1d445, "vm_insert_page" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0x6ba71a8b, "kmalloc_caches" },
	{ 0x7a0b7d1b, "__kmalloc_cache_noprof" },
	{ 0xb09bc67d, "sg_alloc_table" },
	{ 0x804af820, "sg_alloc_table_from_pages_segment" },
	{ 0x2b97a5e9, "dma_map_sgtable" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0x3c0300ea, "eventfd_ctx_fdget" },
	{ 0x8e329d60, "const_current_task" },
	{ 0x16c6a373, "__tracepoint_mmap_lock_start_locking" },
	{ 0xa59da3c0, "down_read" },
	{ 0x16c6a373, "__tracepoint_mmap_lock_acquire_returned" },
	{ 0x076537b2, "pin_user_pages" },
	{ 0x16c6a373, "__tracepoint_mmap_lock_released" },
	{ 0xa59da3c0, "up_read" },
	{ 0x058c185a, "jiffies" },
	{ 0x56fa7025, "dma_buf_export" },
	{ 0xb82edfb3, "idr_alloc" },
	{ 0xacba1b52, "dma_buf_fd" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0x5244a5dc, "idr_find" },
	{ 0x6602c262, "alloc_pages_noprof" },
	{ 0x05ce7807, "dma_buf_put" },
	{ 0xdc9af840, "__mmap_lock_do_trace_released" },
	{ 0xdc9af840, "__mmap_lock_do_trace_start_locking" },
	{ 0x0a83aa13, "__mmap_lock_do_trace_acquire_returned" },
	{ 0xc51eb0e9, "param_ops_int" },
	{ 0xd272d446, "__fentry__" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0x5e505530, "kthread_should_stop" },
	{ 0x5723059f, "msleep_interruptible" },
	{ 0xc7ffe1aa, "si_meminfo" },
	{ 0xf654f750, "nr_swap_pages" },
	{ 0x18ded256, "eventfd_signal_mask" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xdd6830c7, "sysfs_emit" },
	{ 0xfdfc89f8, "dmi_get_system_info" },
	{ 0x2182515b, "__num_online_cpus" },
	{ 0xb1ad3f2f, "boot_cpu_data" },
	{ 0x16402e05, "dma_unmap_sg_attrs" },
	{ 0x5f8848a0, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xd648ae19,
	0xcb8b6ec6,
	0xf1de9e85,
	0xf46d5bf3,
	0x9f222e1e,
	0xaa9455c8,
	0x5ba4a3be,
	0x6cd71bc1,
	0xe48c248b,
	0x126bc160,
	0x6f001536,
	0xc6b90fc4,
	0xf2aabb37,
	0xf296206e,
	0xb5c51982,
	0x494c552b,
	0x737468de,
	0x02b7fa5b,
	0x69a29051,
	0xd1ed87cb,
	0x0bc5fb0d,
	0xe0418251,
	0x37ef79a4,
	0x255dfd5a,
	0xde338d9a,
	0xde338d9a,
	0xf46d5bf3,
	0x07d50c57,
	0xf46d5bf3,
	0x7ab1aa5e,
	0xf1de9e85,
	0x2684a94a,
	0xe4de56b4,
	0xdd1ad581,
	0x1bdf2bc8,
	0x5fc55113,
	0x70670de1,
	0x5e865cb8,
	0xb6d1d445,
	0xbd03ed67,
	0x6ba71a8b,
	0x7a0b7d1b,
	0xb09bc67d,
	0x804af820,
	0x2b97a5e9,
	0x092a35a2,
	0x3c0300ea,
	0x8e329d60,
	0x16c6a373,
	0xa59da3c0,
	0x16c6a373,
	0x076537b2,
	0x16c6a373,
	0xa59da3c0,
	0x058c185a,
	0x56fa7025,
	0xb82edfb3,
	0xacba1b52,
	0x092a35a2,
	0x5244a5dc,
	0x6602c262,
	0x05ce7807,
	0xdc9af840,
	0xdc9af840,
	0x0a83aa13,
	0xc51eb0e9,
	0xd272d446,
	0xd272d446,
	0xe8213e80,
	0xbd03ed67,
	0x5e505530,
	0x5723059f,
	0xc7ffe1aa,
	0xf654f750,
	0x18ded256,
	0xd272d446,
	0xdd6830c7,
	0xfdfc89f8,
	0x2182515b,
	0xb1ad3f2f,
	0x16402e05,
	0x5f8848a0,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"sg_free_table\0"
	"kfree\0"
	"vunmap\0"
	"mutex_init_generic\0"
	"alloc_chrdev_region\0"
	"cdev_init\0"
	"cdev_add\0"
	"class_create\0"
	"device_create_with_groups\0"
	"kthread_create_on_node\0"
	"wake_up_process\0"
	"device_destroy\0"
	"alloc_cpumask_var_node\0"
	"nr_cpu_ids\0"
	"__cpu_online_mask\0"
	"_find_first_bit\0"
	"set_cpus_allowed_ptr\0"
	"free_cpumask_var\0"
	"class_destroy\0"
	"cdev_del\0"
	"unregister_chrdev_region\0"
	"eventfd_ctx_put\0"
	"kthread_stop\0"
	"idr_destroy\0"
	"_raw_spin_lock\0"
	"_raw_spin_unlock\0"
	"mutex_lock\0"
	"idr_remove\0"
	"mutex_unlock\0"
	"unpin_user_page\0"
	"kvfree\0"
	"__free_pages\0"
	"__ubsan_handle_load_invalid_value\0"
	"__kvmalloc_node_noprof\0"
	"sme_me_mask\0"
	"__default_kernel_pte_mask\0"
	"vmap\0"
	"pgprot_writecombine\0"
	"vm_insert_page\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"sg_alloc_table\0"
	"sg_alloc_table_from_pages_segment\0"
	"dma_map_sgtable\0"
	"_copy_from_user\0"
	"eventfd_ctx_fdget\0"
	"const_current_task\0"
	"__tracepoint_mmap_lock_start_locking\0"
	"down_read\0"
	"__tracepoint_mmap_lock_acquire_returned\0"
	"pin_user_pages\0"
	"__tracepoint_mmap_lock_released\0"
	"up_read\0"
	"jiffies\0"
	"dma_buf_export\0"
	"idr_alloc\0"
	"dma_buf_fd\0"
	"_copy_to_user\0"
	"idr_find\0"
	"alloc_pages_noprof\0"
	"dma_buf_put\0"
	"__mmap_lock_do_trace_released\0"
	"__mmap_lock_do_trace_start_locking\0"
	"__mmap_lock_do_trace_acquire_returned\0"
	"param_ops_int\0"
	"__fentry__\0"
	"__x86_return_thunk\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"kthread_should_stop\0"
	"msleep_interruptible\0"
	"si_meminfo\0"
	"nr_swap_pages\0"
	"eventfd_signal_mask\0"
	"__stack_chk_fail\0"
	"sysfs_emit\0"
	"dmi_get_system_info\0"
	"__num_online_cpus\0"
	"boot_cpu_data\0"
	"dma_unmap_sg_attrs\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "DF206547A146A4E1E5506EB");
