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
	{ 0x62b72b0d, "mutex_unlock" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0x14d020cf, "my_mutex" },
	{ 0x48a0f939, "mutex_lock_interruptible" },
	{ 0x27e1a049, "printk" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=lab1_mutex1";


MODULE_INFO(srcversion, "FC196832AD1CFDE1A8D4583");
