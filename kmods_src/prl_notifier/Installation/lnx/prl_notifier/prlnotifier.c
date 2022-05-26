/*
 * Copyright (C) 1999-2021 Parallels International GmbH. All Rights Reserved.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/arm-smccc.h>
#include <linux/highmem.h>
#include <linux/mm.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
#define page_cache_get(x) get_page(x)
#define page_cache_release(x) put_page(x)
#endif

static int prl_smc_send_simple_otg_cmd(unsigned int cmd_id, unsigned int sub_cmd_id)
{
	int rc;
	struct arm_smccc_res regs;
	struct page *req_page;
	// OTG request structure
	struct {
		unsigned int cmd_id;
		unsigned int flags;
		unsigned int sub_cmd_id;
		unsigned int small_data;
	} *send_request;

	arm_smccc_smc(0x8300ff01, 0,0,0,0,0,0,0, &regs);
	if (regs.a0 != 0x623974c5 || regs.a1 != 0x104cc8fc || regs.a2 != 0xf46f86ad || regs.a3 != 0xc53e9b86)
		return -EIO;

	req_page = alloc_page(GFP_KERNEL);
	if (!req_page)
		return -ENOMEM;

	page_cache_get(req_page);

	send_request = kmap(req_page);

	send_request->cmd_id = cmd_id;
	send_request->sub_cmd_id = sub_cmd_id;

	arm_smccc_smc(0xc3000003,
			page_to_pfn(req_page) << PAGE_SHIFT,
			sizeof(*send_request),
			0,
			0,0,0,0, &regs);
	rc = regs.a0 < 0 ? -EIO : 0;

	kunmap(req_page);
	page_cache_release(req_page);
	__free_page(req_page);

	return rc;
}

static int __init prl_notifier_init_module(void)
{
	return prl_smc_send_simple_otg_cmd(10, 3);
}

static void __exit prl_notifier_cleanup_module(void) { }

module_init(prl_notifier_init_module);
module_exit(prl_notifier_cleanup_module);

MODULE_AUTHOR("Parallels International GmbH");
MODULE_DESCRIPTION("Parallels PTfL notification driver");
MODULE_LICENSE("Parallels");
MODULE_VERSION(DRV_VERSION);
MODULE_INFO(supported, "external");
