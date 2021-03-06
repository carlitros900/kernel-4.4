/* Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) "["KBUILD_MODNAME"]" fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/slab.h>

#include "mtk_sd.h"
#include "dbg.h"
#include "include/pmic_api_buck.h"


struct msdc_host *mtk_msdc_host[] = { NULL, NULL, NULL};
EXPORT_SYMBOL(mtk_msdc_host);

int g_dma_debug[HOST_MAX_NUM] = { 0, 0, 0};
u32 latest_int_status[HOST_MAX_NUM] = { 0, 0, 0};

unsigned int msdc_latest_transfer_mode[HOST_MAX_NUM] = {
	/* 0 for PIO; 1 for DMA; 3 for nothing */
	MODE_NONE,
	MODE_NONE,
	MODE_NONE,
};

unsigned int msdc_latest_op[HOST_MAX_NUM] = {
	/* 0 for read; 1 for write; 2 for nothing */
	OPER_TYPE_NUM,
	OPER_TYPE_NUM,
	OPER_TYPE_NUM
};

/* for debug zone */
unsigned int sd_debug_zone[HOST_MAX_NUM] = {
	0,
	0,
	0
};
/* for enable/disable register dump */
unsigned int sd_register_zone[HOST_MAX_NUM] = {
	1,
	1,
	1
};
/* mode select */
u32 dma_size[HOST_MAX_NUM] = {
	512,
	512,
	512
};

u32 drv_mode[HOST_MAX_NUM] = {
	MODE_SIZE_DEP, /* using DMA or not depend on the size */
	MODE_SIZE_DEP,
	MODE_SIZE_DEP
};

int dma_force[HOST_MAX_NUM]; /* used for sd ioctrol */

u8 msdc_clock_src[HOST_MAX_NUM] = {
	0,
	0,
	0
};

/* use for SPM spm_resource_req */
unsigned int msdc_cg_lock_init, msdc_cg_cnt;
spinlock_t msdc_cg_lock;

/**************************************************************/
/* Section 1: Device Tree Global Variables                    */
/**************************************************************/
const struct of_device_id msdc_of_ids[] = {
	{   .compatible = DT_COMPATIBLE_NAME, },
	{ },
};

#if !defined(FPGA_PLATFORM)
static void __iomem *gpio_base;

static void __iomem *pericfg_base;
static void __iomem *topckgen_base;
#endif

void __iomem *msdc_io_cfg_bases[HOST_MAX_NUM];

/**************************************************************/
/* Section 2: Power                                           */
/**************************************************************/
#if !defined(FPGA_PLATFORM)
int msdc_regulator_set_and_enable(struct regulator *reg, int powerVolt)
{
#ifndef CONFIG_MTK_MSDC_BRING_UP_BYPASS
	regulator_set_voltage(reg, powerVolt, powerVolt);
	return regulator_enable(reg);
#else
	return 0;
#endif
}

void msdc_ldo_power(u32 on, struct regulator *reg, int voltage_mv, u32 *status)
{
	int voltage_uv = voltage_mv * 1000;

	if (reg == NULL)
		return;

	if (on) { /* want to power on */
		if (*status == 0) {  /* can power on */
			/*Comment out to reduce log */
			/* pr_notice("msdc power on<%d>\n", voltage_uv); */
			(void)msdc_regulator_set_and_enable(reg, voltage_uv);
			*status = voltage_uv;
		} else if (*status == voltage_uv) {
			pr_notice("msdc power on <%d> again!\n", voltage_uv);
		} else {
			pr_notice("msdc change<%d> to <%d>\n",
				*status, voltage_uv);
			regulator_disable(reg);
			(void)msdc_regulator_set_and_enable(reg, voltage_uv);
			*status = voltage_uv;
		}
	} else {  /* want to power off */
		if (*status != 0) {  /* has been powerred on */
			pr_notice("msdc power off\n");
			(void)regulator_disable(reg);
			*status = 0;
		} else {
			pr_notice("msdc not power on\n");
		}
	}
}

void msdc_dump_ldo_sts(struct msdc_host *host)
{
	u32 ldo_en = 0, ldo_vol = 0, ldo_cal = 0;
	u32 id = host->id;

	switch (id) {
	case 0:
		pmic_read_interface_nolock(REG_VEMC_EN, &ldo_en, MASK_VEMC_EN,
			SHIFT_VEMC_EN);
		pmic_read_interface_nolock(REG_VEMC_VOSEL, &ldo_vol,
			MASK_VEMC_VOSEL, SHIFT_VEMC_VOSEL);
		pmic_read_interface_nolock(REG_VEMC_VOSEL_CAL, &ldo_cal,
			MASK_VEMC_VOSEL_CAL, SHIFT_VEMC_VOSEL_CAL);
		pr_notice(" VEMC_EN=0x%x, VEMC_VOL=0x%x [4b'1011(3V)], VEMC_CAL=0x%x\n",
			ldo_en, ldo_vol, ldo_cal);
		break;
	case 1:
		pmic_read_interface_nolock(REG_VMC_EN, &ldo_en, MASK_VMC_EN,
			SHIFT_VMC_EN);
		pmic_read_interface_nolock(REG_VMC_VOSEL, &ldo_vol,
			MASK_VMC_VOSEL, SHIFT_VMC_VOSEL);
		pmic_read_interface_nolock(REG_VMCH_VOSEL_CAL, &ldo_cal,
			MASK_VMCH_VOSEL_CAL, SHIFT_VMCH_VOSEL_CAL);
		pr_notice(" VMC_EN=0x%x, VMC_VOL=0x%x [4b'0100(1V8),4b'1011(3V)], VMC_CAL=0x%x\n",
			ldo_en, ldo_vol, ldo_cal);

		pmic_read_interface_nolock(REG_VMCH_EN, &ldo_en, MASK_VMCH_EN,
			SHIFT_VMCH_EN);
		pmic_read_interface_nolock(REG_VMCH_VOSEL, &ldo_vol,
			MASK_VMCH_VOSEL, SHIFT_VMCH_VOSEL);
		pmic_read_interface_nolock(REG_VMC_VOSEL_CAL, &ldo_cal,
			MASK_VMC_VOSEL_CAL, SHIFT_VMC_VOSEL_CAL);
		pr_notice(" VMCH_EN=0x%x, VMCH_VOL=0x%x [4b'1011(3V)], VMCH_CAL=0x%x\n",
			ldo_en, ldo_vol, ldo_cal);
		break;
	default:
		break;
	}
}

void msdc_sd_power_switch(struct msdc_host *host, u32 on)
{
	if (host->id == 1) {
		if (on) {
			/* VMC calibration +60mV. According to SA's request. */
			pmic_config_interface(REG_VMC_VOSEL_CAL,
				6,
				MASK_VMC_VOSEL_CAL,
				SHIFT_VMC_VOSEL_CAL);
		}

		msdc_ldo_power(on, host->mmc->supply.vqmmc, VOL_1800,
			&host->power_io);
		msdc_set_tdsel(host, MSDC_TDRDSEL_1V8, 0);
		msdc_set_rdsel(host, MSDC_TDRDSEL_1V8, 0);
		host->hw->driving_applied = &host->hw->driving_sdr50;
		msdc_set_driving(host, host->hw->driving_applied);
	}
}

void msdc_sdio_power(struct msdc_host *host, u32 on)
{
	if (host->id == 2) {
		host->power_flash = VOL_1800 * 1000;
		host->power_io = VOL_1800 * 1000;
	}
}

void msdc_power_calibration_init(struct msdc_host *host)
{
}

int msdc_oc_check(struct msdc_host *host, u32 en)
{
	u32 val = 0;

	if (host->id == 1 && en) {
		pmic_read_interface(REG_VMCH_OC_STATUS, &val,
			MASK_VMCH_OC_STATUS, SHIFT_VMCH_OC_STATUS);

		if (val) {
			pr_notice("msdc1 OC status = %x\n", val);
			host->power_control(host, 0);
			msdc_set_bad_card_and_remove(host);
			return -1;
		}
	}
	return 0;
}

void msdc_emmc_power(struct msdc_host *host, u32 on)
{
	void __iomem *base = host->base;

	if (on == 0) {
		if ((MSDC_READ32(MSDC_PS) & 0x10000) != 0x10000)
			emmc_sleep_failed = 1;
	} else {
		msdc_set_driving(host, &host->hw->driving);
		msdc_set_tdsel(host, MSDC_TDRDSEL_1V8, 0);
		msdc_set_rdsel(host, MSDC_TDRDSEL_1V8, 0);
	}

	msdc_ldo_power(on, host->mmc->supply.vmmc, VOL_3000, &host->power_flash);

#ifdef MTK_MSDC_BRINGUP_DEBUG
	msdc_dump_ldo_sts(host);
#endif
}

void msdc_sd_power(struct msdc_host *host, u32 on)
{
	u32 card_on = on;

	switch (host->id) {
	case 1:
		msdc_set_driving(host, &host->hw->driving);
		msdc_set_tdsel(host, MSDC_TDRDSEL_3V, 0);
		msdc_set_rdsel(host, MSDC_TDRDSEL_3V, 0);
		if (host->hw->flags & MSDC_SD_NEED_POWER)
			card_on = 1;
		/* VMCH VOLSEL */
		msdc_ldo_power(card_on, host->mmc->supply.vmmc, VOL_3000,
			&host->power_flash);
		/* VMC VOLSEL */
		/* rollback to 0mv in REG_VMC_VOSEL_CAL in case of SD3.0 setting */
		pmic_config_interface(REG_VMC_VOSEL_CAL,
			SD_VOL_ACTUAL - VOL_3000,
			MASK_VMC_VOSEL_CAL, SHIFT_VMC_VOSEL_CAL);
		msdc_ldo_power(on, host->mmc->supply.vqmmc, VOL_3000,
			&host->power_io);

		if (on)
			msdc_set_sr(host, 0, 0, 0, 0, 0);
		break;

	default:
		break;
	}

#ifdef MTK_MSDC_BRINGUP_DEBUG
	msdc_dump_ldo_sts(host);
#endif
}

void msdc_clk_pre_enable(struct msdc_host *host)
{
#ifndef CONFIG_MTK_MSDC_BRING_UP_BYPASS
	unsigned long flags;

	spin_lock_irqsave(&msdc_cg_lock, flags);
	msdc_cg_cnt++;
	if (msdc_cg_cnt > 0)
		spm_resource_req(SPM_RESOURCE_USER_MSDC, SPM_RESOURCE_ALL);
	spin_unlock_irqrestore(&msdc_cg_lock, flags);
#endif
}

void msdc_clk_post_disble(struct msdc_host *host)
{
#ifndef CONFIG_MTK_MSDC_BRING_UP_BYPASS
	unsigned long flags;

	spin_lock_irqsave(&msdc_cg_lock, flags);
	msdc_cg_cnt--;
	if (msdc_cg_cnt == 0)
		spm_resource_req(SPM_RESOURCE_USER_MSDC, SPM_RESOURCE_RELEASE);
	spin_unlock_irqrestore(&msdc_cg_lock, flags);
#endif
}

void msdc_sd_power_off(void)
{
	struct msdc_host *host = mtk_msdc_host[1];

	if (host) {
		pr_notice("Power Off, SD card\n");

		/* power must be on */
		host->power_io = VOL_3000 * 1000;
		host->power_flash = VOL_3000 * 1000;

		host->power_control(host, 0);

		msdc_set_bad_card_and_remove(host);
	}
}
EXPORT_SYMBOL(msdc_sd_power_off);
#endif /*if !defined(FPGA_PLATFORM)*/

void msdc_pmic_force_vcore_pwm(bool enable)
{
#if !defined(FPGA_PLATFORM)
	if (vcore_pmic_set_mode(enable))
		pr_notice("[msdc]error: vcore_pmic_set_mode fail\n");
#endif
}

void msdc_set_host_power_control(struct msdc_host *host)
{
	if (host->hw->host_function == MSDC_EMMC) {
		host->power_control = msdc_emmc_power;
	} else if (host->hw->host_function == MSDC_SD) {
		host->power_control = msdc_sd_power;
		host->power_switch = msdc_sd_power_switch;

		#if SD_POWER_DEFAULT_ON
		/* If SD card power is default on, turn it off so that
		 * removable card slot won't keep power when no card plugged
		 */
		if (!(host->mmc->caps & MMC_CAP_NONREMOVABLE)) {
			/* turn on first to match HW/SW state*/
			msdc_sd_power(host, 1);
			mdelay(10);
			msdc_sd_power(host, 0);
		}
		#endif
	} else if (host->hw->host_function == MSDC_SDIO) {
		host->power_control = msdc_sdio_power;
	}

	if (host->power_control != NULL) {
		msdc_power_calibration_init(host);
	} else {
		ERR_MSG("Host function defination error for msdc%d", host->id);
		WARN_ON(1);
	}
}

#if defined(MSDC_HQA)
/*#define MSDC_HQA_HV*/
/*#define MSDC_HQA_NV*/
/*#define MSDC_HQA_LV*/

void msdc_HQA_set_voltage(struct msdc_host *host)
{
#if defined(MSDC_HQA_HV) || defined(MSDC_HQA_LV)
	static int vcore_orig = -1, vufs18_orig = -1;
	u32 vcore, vufs18 = 0, vufs18_cal = 0, val_delta;
#endif
#if defined(MSDC_HQA_NV)
	static int vcore_orig = -1, vufs18_orig = -1;
	u32 vufs18_cal;
#endif

	if (host->is_autok_done == 1)
		return;

	if (vcore_orig < 0)
		pmic_read_interface(0x1060, &vcore_orig, 0x7F, 0);
	if (vufs18_orig < 0)
		pmic_read_interface(0x1A60, &vufs18_orig, 0xF, 8);
	pmic_read_interface(0x1A60, &vufs18_cal, 0xF, 0);
	pr_info("[MSDC%d HQA] orig Vcore 0x%x, Vufs18 0x%x, Vufs18_cal 0x%x\n",
		host->id, vcore_orig, vufs18_orig, vufs18_cal);

#if defined(MSDC_HQA_HV) || defined(MSDC_HQA_LV)
	val_delta = (406250 + vcore_orig * 6250) / 20 / 6250;

#ifdef MSDC_HQA_HV
	vcore = vcore_orig + val_delta;
	vufs18_cal = 0xa;
#endif

#ifdef MSDC_HQA_LV
	vcore = vcore_orig - val_delta;
	vufs18_cal = 0;
	vufs18 = vufs18_orig - 1;
#endif

	pmic_config_interface(0x1060, vcore, 0x7F, 0);

	if (vufs18_cal)
		pmic_config_interface(0x1A60, vufs18_cal, 0xF, 0);
	else
		pmic_config_interface(0x1A60, vufs18, 0xF, 8);

	pr_info("[MSDC%d HQA] adj Vcore 0x%x, Vufs18 0x%x, Vufs18_cal 0x%x\n",
		host->id, vcore, vufs18, vufs18_cal);
#endif
}
#endif

/**************************************************************/
/* Section 3: Clock                                           */
/**************************************************************/
#if !defined(FPGA_PLATFORM)
u32 hclks_msdc0[] = { MSDC0_SRC_0, MSDC0_SRC_1};

/* msdc1/2 clock source reference value is 200M */
u32 hclks_msdc1[] = { MSDC1_SRC_0, MSDC1_SRC_1, MSDC1_SRC_2};

u32 hclks_msdc3[] = { MSDC3_SRC_0, MSDC3_SRC_1, MSDC3_SRC_2, MSDC3_SRC_3,
		      MSDC3_SRC_4, MSDC3_SRC_5, MSDC3_SRC_6, MSDC3_SRC_7};

u32 *hclks_msdc_all[] = {
	hclks_msdc0,
	hclks_msdc1,
	hclks_msdc3,
};
u32 *hclks_msdc;

int msdc_get_ccf_clk_pointer(struct platform_device *pdev,
	struct msdc_host *host)
{
	static char const * const clk_names[] = {
		MSDC0_CLK_NAME, MSDC1_CLK_NAME, MSDC3_CLK_NAME
	};
	static char const * const hclk_names[] = {
		MSDC0_HCLK_NAME, MSDC1_HCLK_NAME, MSDC3_HCLK_NAME
	};

	host->clk_ctl = devm_clk_get(&pdev->dev, clk_names[pdev->id]);
	if  (hclk_names[pdev->id])
		host->hclk_ctl = devm_clk_get(&pdev->dev, hclk_names[pdev->id]);

	if (IS_ERR(host->clk_ctl)) {
		pr_notice("[msdc%d] can not get clock control\n", pdev->id);
		return 1;
	}
	if (clk_prepare(host->clk_ctl)) {
		pr_notice("[msdc%d] can not prepare clock control\n", pdev->id);
		return 1;
	}

	if (hclk_names[pdev->id] && IS_ERR(host->hclk_ctl)) {
		pr_notice("[msdc%d] can not get clock control\n", pdev->id);
		return 1;
	}
	if (hclk_names[pdev->id] && clk_prepare(host->hclk_ctl)) {
		pr_notice("[msdc%d] can not prepare hclock control\n", pdev->id);
		return 1;
	}

#ifdef CONFIG_MTK_HW_FDE
	if (pdev->id == 0) {
		host->aes_clk_ctl = devm_clk_get(&pdev->dev, MSDC0_AES_CLK_NAME);
		if (IS_ERR(host->aes_clk_ctl)) {
			pr_notice("[msdc%d] can not get aes clock control\n", pdev->id);
			WARN_ON(1);
			return 1;
		}
		if (clk_prepare(host->aes_clk_ctl)) {
			pr_notice("[msdc%d] can not prepare aes clock control\n", pdev->id);
			WARN_ON(1);
			return 1;
		}
	}
#endif

	return 0;
}

void msdc_select_clksrc(struct msdc_host *host, int clksrc)
{
	if (host->id != 0) {
		pr_notice("[msdc%d] NOT Support switch pll souce[%s]%d\n",
			host->id, __func__, __LINE__);
		return;
	}

	host->hclk = msdc_get_hclk(host->id, clksrc);
	host->hw->clk_src = clksrc;

	pr_notice("[%s]: msdc%d select clk_src as %d(%dKHz)\n", __func__,
		host->id, clksrc, host->hclk/1000);
}

#include <linux/seq_file.h>
static void msdc_dump_clock_sts_core(struct msdc_host *host, struct seq_file *m)
{
	char buffer[1024];
	char *buf_ptr = buffer;

	if (topckgen_base) {
		buf_ptr += sprintf(buf_ptr,
		" topckgen [0x%p]=0x%x(should bit[18:16]=001b, bit[23]=0, bit[26:24]=001b, bit[31]=0)\n",
			topckgen_base + 0x140, MSDC_READ32(topckgen_base + 0x140));
		buf_ptr += sprintf(buf_ptr,
		" topckgen [0x%p]=0x%x(should bit[2:0]=101b, bit[7]=0)\n",
			topckgen_base + 0x150, MSDC_READ32(topckgen_base + 0x150));
	}
	if (pericfg_base) {
		buf_ptr += sprintf(buf_ptr,
		" pericfg [0x%p]=0x%x(should bit[1:0]=00b)\n",
			pericfg_base + 0x290, MSDC_READ32(pericfg_base + 0x290));
	}

	*buf_ptr = '\0';
	if (!m)
		pr_notice("%s", buffer);
	else
		seq_printf(m, "%s", buffer);
}

void msdc_dump_clock_sts(struct msdc_host *host)
{
	msdc_dump_clock_sts_core(host, NULL);
}

/* FIX ME, consider to remove it */
void dbg_msdc_dump_clock_sts(struct seq_file *m, struct msdc_host *host)
{
	msdc_dump_clock_sts_core(host, m);
}

void msdc_clksrc_onoff(struct msdc_host *host, u32 on)
{
	void __iomem *base = host->base;
	u32 div, mode, hs400_div_dis;
	u32 val;

	if ((on) && (host->core_clkon == 0)) {

		msdc_clk_enable(host);

		host->core_clkon = 1;
		udelay(10);

		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_SDMMC);

		val = MSDC_READ32(MSDC_CFG);
		GET_FIELD(val, CFG_CKDIV_SHIFT, CFG_CKDIV_MASK, div);
		GET_FIELD(val, CFG_CKMOD_SHIFT, CFG_CKMOD_MASK, mode);
		GET_FIELD(val, CFG_CKMOD_HS400_SHIFT, CFG_CKMOD_HS400_MASK,
			hs400_div_dis);
		msdc_clk_stable(host, mode, div, hs400_div_dis);

	} else if ((!on) && (host->core_clkon == 1)) {
		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_MS);

		msdc_clk_disable(host);

		host->core_clkon = 0;
	}
}


#endif /*if !defined(FPGA_PLATFORM)*/

/**************************************************************/
/* Section 4: GPIO and Pad                                    */
/**************************************************************/
#if !defined(FPGA_PLATFORM)
void msdc_dump_vcore(void)
{
#ifndef CONFIG_MTK_MSDC_BRING_UP_BYPASS
	pr_notice("%s: Vcore %d\n", __func__, vcorefs_get_hw_opp());
#endif
}

/*****************************************************************************/
/* obtain dump api interface */
/*****************************************************************************/
void msdc_dump_dvfs_reg(struct msdc_host *host)
{
}

/*
 * Power off card on the 2 bad card conditions:
 * 1. if dat pins keep high when pulled low or
 * 2. dat pins alway keeps high
 */
int msdc_io_check(struct msdc_host *host)
{
	void __iomem *base = host->base;
	unsigned long polling_tmo = 0;
	u32 orig_pupd, orig_r0, orig_r1;

	if (host->id != 1)
		return 0;

	if (host->block_bad_card)
		goto SET_BAD_CARD;

	/* backup the orignal value */
	MSDC_GET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
	MSDC_GET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
	MSDC_GET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);

	/* Switch MSDC1_DAT0 to 50K ohm PD */
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x1 << 4, 1);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x1 << 4, 0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x1 << 4, 1);

	polling_tmo = jiffies + POLLING_PINS;
	while ((MSDC_READ32(MSDC_PS) & 0xF0000) != 0xE0000) {
		if (time_after(jiffies, polling_tmo)) {
			/* gpio cannot pull down pin if sd card
			 * is good but has a high pull-up resistance
			 */
			if ((MSDC_READ32(MSDC_PS) & 0xF0000) == 0xF0000)
				break;
			pr_notice("msdc%d DAT0 pin get wrong, ps = 0x%x!\n",
					host->id, MSDC_READ32(MSDC_PS));
			/* restore */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
			MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);
			goto SET_BAD_CARD;
		}
	}
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);

	/* Switch MSDC1_DAT1 to 50K ohm PD */
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x1 << 5, 1);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x1 << 5, 0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x1 << 5, 1);

	polling_tmo = jiffies + POLLING_PINS;
	while ((MSDC_READ32(MSDC_PS) & 0xF0000) != 0xD0000) {
		if (time_after(jiffies, polling_tmo)) {
			if ((MSDC_READ32(MSDC_PS) & 0xF0000) == 0xF0000)
				break;
			pr_notice("msdc%d DAT1 pin get wrong, ps = 0x%x!\n",
				host->id, MSDC_READ32(MSDC_PS));
			/* restore */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
			MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);
			goto SET_BAD_CARD;
		}
	}
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);

	/* Switch MSDC1_DAT2 to 50K ohm PD */
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x1 << 6, 1);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x1 << 6, 0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x1 << 6, 1);

	polling_tmo = jiffies + POLLING_PINS;
	while ((MSDC_READ32(MSDC_PS) & 0xF0000) != 0xB0000) {
		if (time_after(jiffies, polling_tmo)) {
			if ((MSDC_READ32(MSDC_PS) & 0xF0000) == 0xF0000)
				break;
			pr_notice("msdc%d DAT2 pin get wrong, ps = 0x%x!\n",
				host->id, MSDC_READ32(MSDC_PS));
			/* restore */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
			MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);
			goto SET_BAD_CARD;
		}
	}
	MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, orig_pupd);
	MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, orig_r0);
	MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, orig_r1);

	return 0;

SET_BAD_CARD:
	msdc_set_bad_card_and_remove(host);
	return 1;
}

void msdc_dump_padctl_by_id(u32 id)
{
	if (!gpio_base || !msdc_io_cfg_bases[id]) {
		pr_notice("err: gpio_base=%p, msdc_io_cfg_bases[%d]=%p\n",
			gpio_base, id, msdc_io_cfg_bases[id]);
		return;
	}

	if (id == 0) {
		pr_notice(
		"MSDC0 GPIO0 [0x%p] =0x%8x\tshould:32'b.001.001 .001.001 ........ ........\n",
			MSDC0_GPIO_MODE0, MSDC_READ32(MSDC0_GPIO_MODE0));
		pr_notice(
		"MSDC0 GPIO1 [0x%p] =0x%8x\tshould:32'b.001.001 .001.001 .001.001 .001.001\n",
			MSDC0_GPIO_MODE1, MSDC_READ32(MSDC0_GPIO_MODE1));

		pr_notice(
		"MSDC0 SMT [0x%p] =0x%8x\tshould:32'b........ ........ .....111 11......\n",
			MSDC0_GPIO_SMT, MSDC_READ32(MSDC0_GPIO_SMT));
		pr_notice(
		"MSDC0 IES [0x%p] =0x%8x\tshould:32'b.....111 11111111 1....... ........\n",
			MSDC0_GPIO_IES, MSDC_READ32(MSDC0_GPIO_IES));

		pr_notice(
		"MSDC0 PUPD [0x%p] =0x%8x\tshould:32'b........ ........ ....0100 00000001\n",
			MSDC0_GPIO_PUPD, MSDC_READ32(MSDC0_GPIO_PUPD));
		pr_notice(
		"MSDC0 R0 [0x%p] =0x%8x\tshould:32'b........ ........ ....1011 11111110\n",
			MSDC0_GPIO_R0, MSDC_READ32(MSDC0_GPIO_R0));
		pr_notice(
		"MSDC0 R1 [0x%p] =0x%8x\tshould:32'b........ ........ ....0100 00000001\n",
			MSDC0_GPIO_R1, MSDC_READ32(MSDC0_GPIO_R1));

		pr_notice(
		"MSDC0 TDSEL [0x%p] =0x%8x\tshould:32'b00000000 ........ ........ ........\n",
			MSDC0_GPIO_TDSEL, MSDC_READ32(MSDC0_GPIO_TDSEL));
		pr_notice(
		"MSDC0 TDSEL1 [0x%p] =0x%8x\tshould:32'b........ ........ ....0000 00000000\n",
			MSDC0_GPIO_TDSEL1, MSDC_READ32(MSDC0_GPIO_TDSEL1));

		pr_notice(
		"MSDC0 RDSEL [0x%p] =0x%8x\tshould:32'b..000000 00000000 0000.... ........\n",
			MSDC0_GPIO_RDSEL, MSDC_READ32(MSDC0_GPIO_RDSEL));
		pr_notice(
		"MSDC0 RDSEL1 [0x%p] =0x%8x\tshould:32'b........ ........ ....0000 00000000\n",
			MSDC0_GPIO_RDSEL1, MSDC_READ32(MSDC0_GPIO_RDSEL1));

		pr_notice(
		"MSDC0 DRV   [0x%p] =0x%8x\tshould:32'b..011011 011011.. ........ ........\n",
			MSDC0_GPIO_DRV, MSDC_READ32(MSDC0_GPIO_DRV));
		pr_notice(
		"MSDC0 DRV1   [0x%p] =0x%8x\tshould:32'b........ ........ ........ ......011\n",
			MSDC0_GPIO_DRV1, MSDC_READ32(MSDC0_GPIO_DRV1));
	} else if (id == 1) {
		pr_notice(
		"MSDC1 MODE0 [0x%p] =0x%8x\tshould:32'b........ .001.001 .001.001 .001.001\n",
			MSDC1_GPIO_MODE0, MSDC_READ32(MSDC1_GPIO_MODE0));

		pr_notice(
		"MSDC1 SMT    [0x%p] =0x%8x\tshould:32'b........ ........ ........ 111.....\n",
			MSDC1_GPIO_SMT, MSDC_READ32(MSDC1_GPIO_SMT));
		pr_notice(
		"MSDC1 IES    [0x%p] =0x%8x\tshould:32'b........ ........ 111111.. ........\n",
			MSDC1_GPIO_IES, MSDC_READ32(MSDC1_GPIO_IES));

		pr_notice(
		"MSDC1 PUPD   [0x%p] =0x%8x\tshould: 32'b........ ........ ........ 000001..\n",
			MSDC1_GPIO_PUPD, MSDC_READ32(MSDC1_GPIO_PUPD));
		pr_notice(
		"MSDC1 R0   [0x%p] =0x%8x\tshould: 32'b........ ........ ........ 111110..\n",
			MSDC1_GPIO_R0, MSDC_READ32(MSDC1_GPIO_R0));
		pr_notice(
		"MSDC1 R1   [0x%p] =0x%8x\tshould: 32'b........ ........ ........ 000001..\n",
			MSDC1_GPIO_R1, MSDC_READ32(MSDC1_GPIO_R1));

		pr_notice("MSDC1 TDSEL  [0x%p] =0x%8x\n",
			MSDC1_GPIO_TDSEL, MSDC_READ32(MSDC1_GPIO_TDSEL));
		pr_notice("should 1.8v & sleep & awake: 32'b00000000 0000.... ........ ........\n");

		pr_notice("MSDC1 RDSEL0  [0x%p] =0x%8x\n",
			MSDC1_GPIO_RDSEL, MSDC_READ32(MSDC1_GPIO_RDSEL));
		pr_notice("should 1.8v & sleep & awake: 32'b00000000 00000000 00...... ........\n");

		pr_notice(
		"MSDC1 DRV    [0x%p] =0x%8x\tshould: 32'b........ 01001000 1....... ........\n",
			MSDC1_GPIO_DRV, MSDC_READ32(MSDC1_GPIO_DRV));

		pr_notice(
		"MSDC1 SR    [0x%p] =0x%8x\tshould: 32'b........ ........ ........ 000000..\n",
			MSDC1_GPIO_SR, MSDC_READ32(MSDC1_GPIO_SR));
	} else if (id == 2) {
	}
}

void msdc_set_pin_mode(struct msdc_host *host)
{

}

void msdc_set_ies_by_id(u32 id, int set_ies)
{

}

void msdc_set_smt_by_id(u32 id, int set_smt)
{
	if (id == 0) {
		/*
		* 1. enable SMT
		*/
		MSDC_SET_FIELD(MSDC0_GPIO_SMT, 0x1F << 6, (set_smt ? 0x1F : 0));
	} else if (id == 1) {
		MSDC_SET_FIELD(MSDC1_GPIO_SMT, 0x7 << 5, (set_smt ? 0x7 : 0));
	} else if (id == 3) {
	}
}

void msdc_set_tdsel_by_id(u32 id, u32 flag, u32 value)
{
	u32 cust_val;

	if (id == 0) {
		if (flag == MSDC_TDRDSEL_CUST)
			cust_val = value;
		else
			cust_val = 0;
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL, 0xF << 24,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL, 0xF << 28,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL1, 0xF << 0,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL1, 0xF << 4,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_TDSEL1, 0xF << 8,
			cust_val);
	} else if (id == 1) {
		if (flag == MSDC_TDRDSEL_CUST)
			cust_val = value;
		else
			cust_val = 0;
		MSDC_SET_FIELD(MSDC1_GPIO_TDSEL, 0xF << 20,
			cust_val);
		MSDC_SET_FIELD(MSDC1_GPIO_TDSEL, 0xF << 24,
			cust_val);
		MSDC_SET_FIELD(MSDC1_GPIO_TDSEL, 0xF << 28,
			cust_val);
	} else if (id == 2) {
	}
}

void msdc_set_rdsel_by_id(u32 id, u32 flag, u32 value)
{
	u32 cust_val;

	if (id == 0) {
		if (flag == MSDC_TDRDSEL_CUST)
			cust_val = value;
		else
			cust_val = 0;
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL, 0x3F << 12,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL, 0x3F << 18,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL, 0x3F << 24,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL1, 0x3F << 0,
			cust_val);
		MSDC_SET_FIELD(MSDC0_GPIO_RDSEL1, 0x3F << 6,
			cust_val);
	} else if (id == 1) {
		if (flag == MSDC_TDRDSEL_CUST)
			cust_val = value;
		else
			cust_val = 0;
		MSDC_SET_FIELD(MSDC1_GPIO_RDSEL, 0x3F << 14,
			cust_val);
		MSDC_SET_FIELD(MSDC1_GPIO_RDSEL, 0x3F << 20,
			cust_val);
		MSDC_SET_FIELD(MSDC1_GPIO_RDSEL, 0x3F << 26,
			cust_val);
	} else if (id == 2) {
	}
}

void msdc_get_tdsel_by_id(u32 id, u32 *value)
{
	if (id == 0) {
		MSDC_GET_FIELD(MSDC0_GPIO_TDSEL, 0xF << 24,
			*value);
	} else if (id == 1) {
		MSDC_GET_FIELD(MSDC1_GPIO_TDSEL, 0xF << 20,
			*value);
	} else if (id == 2) {
	}
}

void msdc_get_rdsel_by_id(u32 id, u32 *value)
{
	if (id == 0) {
		MSDC_GET_FIELD(MSDC0_GPIO_RDSEL, 0x3F << 12,
			*value);
	} else if (id == 1) {
		MSDC_GET_FIELD(MSDC1_GPIO_RDSEL, 0x3F << 14,
			*value);
	} else if (id == 2) {
	}
}

void msdc_set_sr_by_id(u32 id, int clk, int cmd, int dat, int rst, int ds)
{
	if (id == 0) {
		/* do nothing since 10nm does not have SR control for 1.8V */
	} else if (id == 1) {
		MSDC_SET_FIELD(MSDC1_GPIO_SR, 0x1 << 3,
			cmd ? 1 : 0);
		MSDC_SET_FIELD(MSDC1_GPIO_SR, 0x1 << 2,
			clk ? 1 : 0);
		MSDC_SET_FIELD(MSDC1_GPIO_SR, 0xF << 4,
			dat ? 0xF : 0);
	} else if (id == 2) {
		/* do nothing since 10nm does not have SR control for 1.8V */
	}
}

void msdc_set_driving_by_id(u32 id, struct msdc_hw_driving *driving)
{
#ifndef CONFIG_MTK_MSDC_BRING_UP_BYPASS
	pr_notice("msdc%d set driving: clk_drv=%d, cmd_drv=%d, dat_drv=%d, rst_drv=%d, ds_drv=%d\n",
		id,
		driving->clk_drv,
		driving->cmd_drv,
		driving->dat_drv,
		driving->rst_drv,
		driving->ds_drv);
#endif

	if (id == 0) {
		MSDC_SET_FIELD(MSDC0_GPIO_DRV, 0x7 << 27,
			driving->ds_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV1, 0x7 << 0,
			driving->rst_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV, 0x7 << 21,
			driving->cmd_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV, 0x7 << 18,
			driving->clk_drv);
		MSDC_SET_FIELD(MSDC0_GPIO_DRV, 0x7 << 24,
			driving->dat_drv);
	} else if (id == 1) {
		MSDC_SET_FIELD(MSDC1_GPIO_DRV, 0x7 << 18,
			driving->cmd_drv);
		MSDC_SET_FIELD(MSDC1_GPIO_DRV, 0x7 << 15,
			driving->clk_drv);
		MSDC_SET_FIELD(MSDC1_GPIO_DRV, 0x7 << 21,
			driving->dat_drv);
	} else if (id == 2) {
	}
}

void msdc_get_driving_by_id(u32 id, struct msdc_hw_driving *driving)
{
	if (id == 0) {
		MSDC_GET_FIELD(MSDC0_GPIO_DRV, 0x7 << 27,
			driving->ds_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV1, 0x7 << 0,
			driving->rst_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV, 0x7 << 21,
			driving->cmd_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV, 0x7 << 18,
			driving->clk_drv);
		MSDC_GET_FIELD(MSDC0_GPIO_DRV, 0x7 << 24,
			driving->dat_drv);
	} else if (id == 1) {
		MSDC_GET_FIELD(MSDC1_GPIO_DRV, 0x7 << 18,
			driving->cmd_drv);
		MSDC_GET_FIELD(MSDC1_GPIO_DRV, 0x7 << 15,
			driving->clk_drv);
		MSDC_GET_FIELD(MSDC1_GPIO_DRV, 0x7 << 21,
			driving->dat_drv);
	} else if (id == 2) {
	}
}

/* msdc pin config
 * MSDC0
 * PUPD/R1/R0
 * 0/0/0: High-Z
 * 0/1/0: Pull-up with 50Kohm
 * 0/0/1: Pull-up with 10Kohm
 * 0/1/1: Pull-up with 50Kohm//10Kohm
 * 1/0/0: High-Z
 * 1/1/0: Pull-down with 50Kohm
 * 1/0/1: Pull-down with 10Kohm
 * 1/1/1: Pull-down with 50Kohm//10Kohm
 */
void msdc_pin_config_by_id(u32 id, u32 mode)
{
	if (id == 0) {
		/* 1. don't pull CLK high;
		 * 2. Don't toggle RST to prevent from entering boot mode
		 */
		if (mode == MSDC_PIN_PULL_NONE) {
		} else if (mode == MSDC_PIN_PULL_DOWN) {
			/* Switch MSDC0_* to 50K ohm PD */
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD, 0xFFF, 0x7FF);
			MSDC_SET_FIELD(MSDC0_GPIO_R0, 0xFFF, 0);
			MSDC_SET_FIELD(MSDC0_GPIO_R1, 0xFFF, 0xFFF);
		} else if (mode == MSDC_PIN_PULL_UP) {
			/* Switch MSDC0_CLK to 50K ohm PD,
			 * MSDC0_CMD/MSDC0_DAT* to 10K ohm PU,
			 * MSDC0_DSL to 50K ohm PD,
			 * MSDC0_RSTB to 10K ohm PU
			 */
			MSDC_SET_FIELD(MSDC0_GPIO_PUPD, 0xFFF, 0x401);
			MSDC_SET_FIELD(MSDC0_GPIO_R0, 0xFFF, 0xBFE);
			MSDC_SET_FIELD(MSDC0_GPIO_R1, 0xFFF, 0x401);
		}
	} else if (id == 1) {
		if (mode == MSDC_PIN_PULL_NONE) {
		} else if (mode == MSDC_PIN_PULL_DOWN) {
			/* Switch MSDC1_* to 50K ohm PD */
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, 0x3F);
			MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, 0);
			MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, 0x3F);
		} else if (mode == MSDC_PIN_PULL_UP) {
			/* Switch MSDC1_CLK to 50K ohm PD,
			* MSDC1_CMD/MSDC1_DAT* to 10K ohm PU
			*/
			MSDC_SET_FIELD(MSDC1_GPIO_PUPD, 0x3F << 2, 0x1);
			MSDC_SET_FIELD(MSDC1_GPIO_R0, 0x3F << 2, 0x3E);
			MSDC_SET_FIELD(MSDC1_GPIO_R1, 0x3F << 2, 0x1);
		}
	} else if (id == 2) {
	}

}
#endif /*if !defined(FPGA_PLATFORM)*/


/**************************************************************/
/* Section 5: Device Tree Init function                       */
/*            This function is placed here so that all	      */
/*            functions and variables used by it has already  */
/*            been declared                                   */
/**************************************************************/
/*
 * parse pinctl settings
 * Driver strength
 */
#if !defined(FPGA_PLATFORM)
static int msdc_get_pinctl_settings(struct msdc_host *host,
	struct device_node *np)
{
	struct device_node *pinctl_node, *pins_node;
	static char const * const pinctl_names[] = {
		"pinctl", "pinctl_sdr104", "pinctl_sdr50", "pinctl_ddr50"
	};

	/* sequence shall be the same as sequence in msdc_hw_driving */
	static char const * const pins_names[] = {
		"pins_cmd", "pins_dat", "pins_clk", "pins_rst", "pins_ds"
	};
	unsigned char *pin_drv;
	int i, j;

	host->hw->driving_applied = &host->hw->driving;
	for (i = 0; i < ARRAY_SIZE(pinctl_names); i++) {
		pinctl_node = of_parse_phandle(np, pinctl_names[i], 0);

		if (strcmp(pinctl_names[i], "pinctl") == 0)
			pin_drv = (unsigned char *)&host->hw->driving;
		else if (strcmp(pinctl_names[i], "pinctl_sdr104") == 0)
			pin_drv = (unsigned char *)&host->hw->driving_sdr104;
		else if (strcmp(pinctl_names[i], "pinctl_sdr50") == 0)
			pin_drv = (unsigned char *)&host->hw->driving_sdr50;
		else if (strcmp(pinctl_names[i], "pinctl_ddr50") == 0)
			pin_drv = (unsigned char *)&host->hw->driving_ddr50;
		else
			continue;

		for (j = 0; j < ARRAY_SIZE(pins_names); j++) {
			pins_node = of_get_child_by_name(pinctl_node,
				pins_names[j]);

			if (pins_node)
				of_property_read_u8(pins_node,
					"drive-strength", pin_drv);
			pin_drv++;
		}
	}

	return 0;
}
#endif

/* Get msdc register settings
 * 1. internal data delay for tuning, FIXME: can be removed when use data tune?
 * 2. sample edge
 */
static int msdc_get_register_settings(struct msdc_host *host, struct device_node *np)
{
	struct device_node *register_setting_node = NULL;

	/* parse hw property settings */
	register_setting_node = of_parse_phandle(np, "register_setting", 0);
	if (register_setting_node) {
		of_property_read_u8(register_setting_node, "cmd_edge",
				&host->hw->cmd_edge);
		of_property_read_u8(register_setting_node, "rdata_edge",
				&host->hw->rdata_edge);
		of_property_read_u8(register_setting_node, "wdata_edge",
				&host->hw->wdata_edge);
	} else {
		pr_notice("[msdc%d] register_setting is not found in DT\n",
			host->id);
	}

	return 0;
}

/*
 *	msdc_of_parse() - parse host's device-tree node
 *	@host: host whose node should be parsed.
 *
 */
int msdc_of_parse(struct platform_device *pdev, struct mmc_host *mmc)
{
	struct device_node *np;
	struct msdc_host *host = mmc_priv(mmc);
	int ret = 0;
	int len = 0;
	u8 id;

	np = mmc->parent->of_node; /* mmcx node in project dts */

	if (of_property_read_u8(np, "index", &id)) {
		pr_notice("[%s] host index not specified in device tree\n",
			pdev->dev.of_node->name);
		return -1;
	}
	host->id = id;
	pdev->id = id;

	pr_notice("DT probe %s%d!\n", pdev->dev.of_node->name, id);

	ret = mmc_of_parse(mmc);
	if (ret) {
		pr_notice("%s: mmc of parse error!!: %d\n", __func__, ret);
		return ret;
	}

	host->mmc = mmc;
	host->hw = kzalloc(sizeof(struct msdc_hw), GFP_KERNEL);

	/* iomap register */
	host->base = of_iomap(np, 0);
	if (!host->base) {
		pr_notice("[msdc%d] of_iomap failed\n", mmc->index);
		return -ENOMEM;
	}

	/* get irq # */
	host->irq = irq_of_parse_and_map(np, 0);
	pr_notice("[msdc%d] get irq # %d\n", host->id, host->irq);
	WARN_ON(host->irq < 0);

#if !defined(FPGA_PLATFORM)
	/* get clk_src */
	if (of_property_read_u8(np, "clk_src", &host->hw->clk_src)) {
		pr_notice("[msdc%d] error: clk_src isn't found in device tree.\n",
			host->id);
		WARN_ON(1);
	}
#endif

	/* get msdc flag(caps)*/
	if (of_find_property(np, "msdc-sys-suspend", &len))
		host->hw->flags |= MSDC_SYS_SUSPEND;

	if (of_find_property(np, "sd-uhs-ddr208", &len))
		host->hw->flags |= MSDC_SDIO_DDR208;

	/* Returns 0 on success, -EINVAL if the property does not exist,
	 * -ENODATA if property does not have a value, and -EOVERFLOW if the
	 * property data isn't large enough.
	 */
	if (of_property_read_u8(np, "host_function", &host->hw->host_function))
		pr_notice("[msdc%d] host_function isn't found in device tree\n",
			host->id);

	/* get cd_gpio and cd_level */
#ifdef CONFIG_MTK_GPIOLIB_STAND
	cd_gpio = of_get_named_gpio(np, "cd-gpios", 0);
#else
	of_property_read_u32_index(np, "cd-gpios", 1, &cd_gpio);
#endif
	if (of_property_read_u8(np, "cd_level", &host->hw->cd_level))
		pr_notice("[msdc%d] cd_level isn't found in device tree\n", host->id);

	msdc_get_register_settings(host, np);

#if !defined(FPGA_PLATFORM)
	msdc_get_pinctl_settings(host, np);

	mmc->supply.vmmc = regulator_get(mmc_dev(mmc), "vmmc");
	mmc->supply.vqmmc = regulator_get(mmc_dev(mmc), "vqmmc");
#else
	msdc_fpga_pwr_init();
#endif

#if !defined(FPGA_PLATFORM)
	if (host->hw->host_function == MSDC_EMMC) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,msdc_top");
		host->base_top = of_iomap(np, 0);
		pr_debug("of_iomap for MSDC%d TOP base @ 0x%p\n",
			host->id, host->base_top);
	}
#endif

#if defined(CFG_DEV_MSDC3)
	if (host->hw->host_function == MSDC_SDIO) {
		host->hw->flags |= MSDC_EXT_SDIO_IRQ;
		host->hw->request_sdio_eirq = mt_sdio_ops[3].sdio_request_eirq;
		host->hw->enable_sdio_eirq = mt_sdio_ops[3].sdio_enable_eirq;
		host->hw->disable_sdio_eirq = mt_sdio_ops[3].sdio_disable_eirq;
		host->hw->register_pm = mt_sdio_ops[3].sdio_register_pm;
	}
#endif

	/* init spinlock for SPM */
	if (msdc_cg_lock_init == 0) {
		msdc_cg_lock_init = 1;
		spin_lock_init(&msdc_cg_lock);
		msdc_cg_cnt = 0;
	}

	/* device rename */
	if ((host->id == 0) && !device_rename(mmc->parent, "bootdevice"))
		pr_notice("[msdc%d] device renamed to bootdevice.\n", host->id);
	else if ((host->id == 1) && !device_rename(mmc->parent, "externdevice"))
		pr_notice("[msdc%d] device renamed to externdevice.\n", host->id);
	else if ((host->id == 0) || (host->id == 1))
		pr_notice("[msdc%d] error: device renamed failed.\n", host->id);

	return host->id;
}

int msdc_dt_init(struct platform_device *pdev, struct mmc_host *mmc)
{
	int id;

#ifndef FPGA_PLATFORM
	static char const * const ioconfig_names[] = {
		MSDC0_IOCFG_NAME, MSDC1_IOCFG_NAME
	};
	struct device_node *np;
#endif

	id = msdc_of_parse(pdev, mmc);
	if (id < 0) {
		pr_notice("%s: msdc_of_parse error!!: %d\n", __func__, id);
		return id;
	}

#ifndef FPGA_PLATFORM
	if (gpio_base == NULL) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,gpio");
		gpio_base = of_iomap(np, 0);
		pr_debug("of_iomap for gpio base @ 0x%p\n", gpio_base);
	}

	if (msdc_io_cfg_bases[id] == NULL) {
		np = of_find_compatible_node(NULL, NULL, ioconfig_names[id]);
		msdc_io_cfg_bases[id] = of_iomap(np, 0);
		pr_debug("of_iomap for MSDC%d IOCFG base @ 0x%p\n",
			id, msdc_io_cfg_bases[id]);
	}

	if (topckgen_base == NULL) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,topckgen");
		topckgen_base = of_iomap(np, 0);
		pr_debug("of_iomap for topckgen base @ 0x%p\n",
			topckgen_base);
	}

	if (pericfg_base == NULL) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,pericfg");
		pericfg_base = of_iomap(np, 0);
		pr_debug("of_iomap for pericfg base @ 0x%p\n",
			pericfg_base);
	}

#endif

	return 0;
}

/**************************************************************/
/* Section 7: For msdc register dump                          */
/**************************************************************/
u16 msdc_offsets[] = {
	OFFSET_MSDC_CFG,
	OFFSET_MSDC_IOCON,
	OFFSET_MSDC_PS,
	OFFSET_MSDC_INT,
	OFFSET_MSDC_INTEN,
	OFFSET_MSDC_FIFOCS,
	OFFSET_SDC_CFG,
	OFFSET_SDC_CMD,
	OFFSET_SDC_ARG,
	OFFSET_SDC_STS,
	OFFSET_SDC_RESP0,
	OFFSET_SDC_RESP1,
	OFFSET_SDC_RESP2,
	OFFSET_SDC_RESP3,
	OFFSET_SDC_BLK_NUM,
	OFFSET_SDC_VOL_CHG,
	OFFSET_SDC_CSTS,
	OFFSET_SDC_CSTS_EN,
	OFFSET_SDC_DCRC_STS,
	OFFSET_SDC_ADV_CFG0,
	OFFSET_EMMC_CFG0,
	OFFSET_EMMC_CFG1,
	OFFSET_EMMC_STS,
	OFFSET_EMMC_IOCON,
	OFFSET_SDC_ACMD_RESP,
	OFFSET_MSDC_DMA_SA_HIGH,
	OFFSET_MSDC_DMA_SA,
	OFFSET_MSDC_DMA_CA,
	OFFSET_MSDC_DMA_CTRL,
	OFFSET_MSDC_DMA_CFG,
	OFFSET_MSDC_DMA_LEN,
	OFFSET_MSDC_DBG_SEL,
	OFFSET_MSDC_DBG_OUT,
	OFFSET_MSDC_PATCH_BIT0,
	OFFSET_MSDC_PATCH_BIT1,
	OFFSET_MSDC_PATCH_BIT2,
	OFFSET_MSDC_PAD_TUNE0,
	OFFSET_MSDC_PAD_TUNE1,
	OFFSET_MSDC_HW_DBG,
	OFFSET_MSDC_VERSION,

	OFFSET_EMMC50_PAD_DS_TUNE,
	OFFSET_EMMC50_PAD_CMD_TUNE,
	OFFSET_EMMC50_PAD_DAT01_TUNE,
	OFFSET_EMMC50_PAD_DAT23_TUNE,
	OFFSET_EMMC50_PAD_DAT45_TUNE,
	OFFSET_EMMC50_PAD_DAT67_TUNE,
	OFFSET_EMMC51_CFG0,
	OFFSET_EMMC50_CFG0,
	OFFSET_EMMC50_CFG1,
	OFFSET_EMMC50_CFG2,
	OFFSET_EMMC50_CFG3,
	OFFSET_EMMC50_CFG4,
	OFFSET_SDC_FIFO_CFG,
	OFFSET_MSDC_AES_SEL,

#ifdef CONFIG_MTK_HW_FDE
	OFFSET_EMMC52_AES_EN,
	OFFSET_EMMC52_AES_CFG_GP0,
	OFFSET_EMMC52_AES_IV0_GP0,
	OFFSET_EMMC52_AES_IV1_GP0,
	OFFSET_EMMC52_AES_IV2_GP0,
	OFFSET_EMMC52_AES_IV3_GP0,
	OFFSET_EMMC52_AES_CTR0_GP0,
	OFFSET_EMMC52_AES_CTR1_GP0,
	OFFSET_EMMC52_AES_CTR2_GP0,
	OFFSET_EMMC52_AES_CTR3_GP0,
	OFFSET_EMMC52_AES_KEY0_GP0,
	OFFSET_EMMC52_AES_KEY1_GP0,
	OFFSET_EMMC52_AES_KEY2_GP0,
	OFFSET_EMMC52_AES_KEY3_GP0,
	OFFSET_EMMC52_AES_KEY4_GP0,
	OFFSET_EMMC52_AES_KEY5_GP0,
	OFFSET_EMMC52_AES_KEY6_GP0,
	OFFSET_EMMC52_AES_KEY7_GP0,
	OFFSET_EMMC52_AES_TKEY0_GP0,
	OFFSET_EMMC52_AES_TKEY1_GP0,
	OFFSET_EMMC52_AES_TKEY2_GP0,
	OFFSET_EMMC52_AES_TKEY3_GP0,
	OFFSET_EMMC52_AES_TKEY4_GP0,
	OFFSET_EMMC52_AES_TKEY5_GP0,
	OFFSET_EMMC52_AES_TKEY6_GP0,
	OFFSET_EMMC52_AES_TKEY7_GP0,
	OFFSET_EMMC52_AES_SWST,
	OFFSET_EMMC52_AES_CFG_GP1,
	OFFSET_EMMC52_AES_IV0_GP1,
	OFFSET_EMMC52_AES_IV1_GP1,
	OFFSET_EMMC52_AES_IV2_GP1,
	OFFSET_EMMC52_AES_IV3_GP1,
	OFFSET_EMMC52_AES_CTR0_GP1,
	OFFSET_EMMC52_AES_CTR1_GP1,
	OFFSET_EMMC52_AES_CTR2_GP1,
	OFFSET_EMMC52_AES_CTR3_GP1,
	OFFSET_EMMC52_AES_KEY0_GP1,
	OFFSET_EMMC52_AES_KEY1_GP1,
	OFFSET_EMMC52_AES_KEY2_GP1,
	OFFSET_EMMC52_AES_KEY3_GP1,
	OFFSET_EMMC52_AES_KEY4_GP1,
	OFFSET_EMMC52_AES_KEY5_GP1,
	OFFSET_EMMC52_AES_KEY6_GP1,
	OFFSET_EMMC52_AES_KEY7_GP1,
	OFFSET_EMMC52_AES_TKEY0_GP1,
	OFFSET_EMMC52_AES_TKEY1_GP1,
	OFFSET_EMMC52_AES_TKEY2_GP1,
	OFFSET_EMMC52_AES_TKEY3_GP1,
	OFFSET_EMMC52_AES_TKEY4_GP1,
	OFFSET_EMMC52_AES_TKEY5_GP1,
	OFFSET_EMMC52_AES_TKEY6_GP1,
	OFFSET_EMMC52_AES_TKEY7_GP1,
#endif

	0xFFFF /*as mark of end */
};

u16 msdc_offsets_top[] = {
	OFFSET_EMMC_TOP_CONTROL,
	OFFSET_EMMC_TOP_CMD,
	OFFSET_TOP_EMMC50_PAD_CTL0,
	OFFSET_TOP_EMMC50_PAD_DS_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT0_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT1_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT2_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT3_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT4_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT5_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT6_TUNE,
	OFFSET_TOP_EMMC50_PAD_DAT7_TUNE,

	0xFFFF /*as mark of end */
};
