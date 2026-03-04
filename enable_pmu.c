// enable_pmu.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/smp.h>

static void enable_pmu_userspace(void *info) {
    // Set bit 0 (EN) of PMUSERENR_EL0 to allow user-space access
    asm volatile("msr pmuserenr_el0, %0" :: "r"(0xf));
}

static int __init pmu_init(void) {
    on_each_cpu(enable_pmu_userspace, NULL, 1);
    pr_info("PMU user-space access enabled on all cores\n");
    return 0;
}

static void __exit pmu_exit(void) {
    pr_info("PMU module unloaded\n");
}

module_init(pmu_init);
module_exit(pmu_exit);
MODULE_LICENSE("GPL");