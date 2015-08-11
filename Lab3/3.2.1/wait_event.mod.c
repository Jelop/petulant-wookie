#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xda9e78e9, "module_layout" },
	{ 0xeed38436, "malloc_sizes" },
	{ 0xdb7e82df, "misc_register" },
	{ 0xd197d610, "kmem_cache_alloc" },
	{ 0x37a0cba, "kfree" },
	{ 0xff58149e, "misc_deregister" },
	{ 0xc8b57c27, "autoremove_wake_function" },
	{ 0xc06ec2c8, "abort_exclusive_wait" },
	{ 0x67c2fa54, "__copy_to_user" },
	{ 0x8893fa5d, "finish_wait" },
	{ 0xb77a7c47, "prepare_to_wait_exclusive" },
	{ 0x1000e51, "schedule" },
	{ 0xfbc74f64, "__copy_from_user" },
	{ 0xfa2a45e, "__memzero" },
	{ 0xb9e52429, "__wake_up" },
	{ 0x2e5810c6, "__aeabi_unwind_cpp_pr1" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0xf623f316, "module_refcount" },
	{ 0x27e1a049, "printk" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "4BFF9A2BD1685F99454B865");
