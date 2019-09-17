/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2017 by Chuck Chen <yh_hsieh@realtek.com>
 *
 * Time initialization.
 */
#include <common.h>
#include <asm/io.h>
#include <asm/arch/cpu.h>
#include <asm/arch/fw_info.h>
#include <asm/arch/sys_proto.h>

DECLARE_GLOBAL_DATA_PTR;

const struct rtd1395_sysinfo sysinfo = {
	"Board: Realtek QA Board\n"
};

/**
 * @brief checkboard
 *
 * @return 0
 */
int checkboard(void)
{
	printf(sysinfo.board_string);
	return 0;
}

/**
 * @brief board_init
 *
 * @return 0
 */
int board_init(void)
{

	__raw_writel(0x00000000, AARCH_REGISTER); // Clear the status of aarch register
#ifdef CONFIG_RTK_ARM32
	/* 0x1 for bl31 goto aarch32 resume flow */
	__raw_writel(__raw_readl(AARCH_REGISTER) | (0x1 << 0), AARCH_REGISTER);
#else
	/* 0x0 for bl31 goto aarch64 resume flow */
	__raw_writel(__raw_readl(AARCH_REGISTER) | (0x0 << 0), AARCH_REGISTER);
#endif

	return 0;
}

/**
 * @brief dram_init_banksize
 *
 * @return 0
 */
/*void dram_init_banksize(void)
{
	// Bank 1
	gd->bd->bi_dram[0].start = CONFIG_SYS_SDRAM_BASE;
	gd->bd->bi_dram[0].size = CONFIG_SYS_RAM_DCU1_SIZE;

#if (CONFIG_NR_DRAM_BANKS > 1)
	// Bank 2
	gd->bd->bi_dram[1].start = CONFIG_SYS_SDRAM_DCU2_BASE;
	gd->bd->bi_dram[1].size = CONFIG_SYS_RAM_DCU2_SIZE;
#endif

#if (CONFIG_NR_DRAM_BANKS > 2)
	// Bank 3
#if defined(CONFIG_SYS_SDRAM_DCU_OPT_BASE) && defined(CONFIG_SYS_RAM_DCU_OPT_SIZE)
	gd->bd->bi_dram[2].start = CONFIG_SYS_SDRAM_DCU_OPT_BASE;
	gd->bd->bi_dram[2].size = CONFIG_SYS_RAM_DCU_OPT_SIZE;
#endif
#endif

}*/

int board_eth_init(bd_t *bis)
{
	return 0;
}

/**
 * @brief misc_init_r - Configure Panda board specific configurations
 * such as power configurations, ethernet initialization as phase2 of
 * boot sequence
 *
 * @return 0
 */
int misc_init_r(void)
{
	return 0;
}

/*
 * get_board_rev() - get board revision
 */
u32 get_board_rev(void)
{
	uint revision = 0;

	revision = (uint)simple_strtoul(CONFIG_VERSION, NULL, 16);

	return revision;
}
