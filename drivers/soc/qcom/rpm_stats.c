/* Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <asm/arch_timer.h>

#define RPM_STATS_NUM_REC	2
#define MSM_ARCH_TIMER_FREQ	19200000

#define GET_PDATA_OF_ATTR(attr) \
	(container_of(attr, struct msm_rpmstats_kobj_attr, ka)->pd)

struct msm_rpmstats_record {
	char name[32];
	u32 id;
	u32 val;
};

struct msm_rpmstats_platform_data {
	phys_addr_t phys_addr_base;
	u32 phys_size;
};

struct msm_rpmstats_private_data {
	void __iomem *reg_base;
	u32 num_records;
	u32 read_idx;
	u32 len;
	char buf[320];
	struct msm_rpmstats_platform_data *platform_data;
};

struct msm_rpm_stats_data {
	u32 stat_type;
	u32 count;
	u64 last_entered_at;
	u64 last_exited_at;
	u64 accumulated;
#if defined(CONFIG_MSM_RPM_SMD)
	u32 client_votes;
	u32 reserved[3];
#endif

};

struct msm_aop_ss_state {
	u32 info;
};

struct msm_rpmstats_kobj_attr {
	struct kobj_attribute ka;
	struct msm_rpmstats_platform_data *pd;
};

static inline u64 get_time_in_sec(u64 counter)
{
	do_div(counter, MSM_ARCH_TIMER_FREQ);

	return counter;
}

static inline u64 get_time_in_msec(u64 counter)
{
	do_div(counter, MSM_ARCH_TIMER_FREQ);
	counter *= MSEC_PER_SEC;

	return counter;
}

static inline int msm_rpmstats_append_data_to_buf(char *buf,
		struct msm_rpm_stats_data *data, int buflength)
{
	char stat_type[5];
	u64 time_in_last_mode;
	u64 time_since_last_mode;
	u64 actual_last_sleep;

	stat_type[4] = 0;
	memcpy(stat_type, &data->stat_type, sizeof(u32));

	time_in_last_mode = data->last_exited_at - data->last_entered_at;
	time_in_last_mode = get_time_in_msec(time_in_last_mode);
	time_since_last_mode = arch_counter_get_cntvct() - data->last_exited_at;
	time_since_last_mode = get_time_in_sec(time_since_last_mode);
	actual_last_sleep = get_time_in_msec(data->accumulated);

#if defined(CONFIG_MSM_RPM_SMD)
	return snprintf(buf, buflength,
		"RPM Mode:%s\n\t count:%d\ntime in last mode(msec):%llu\n"
		"time since last mode(sec):%llu\nactual last sleep(msec):%llu\n"
		"client votes: %#010x\n\n",
		stat_type, data->count, time_in_last_mode,
		time_since_last_mode, actual_last_sleep,
		data->client_votes);
#else
	return snprintf(buf, buflength,
		"RPM Mode:%s\n\t count:%d\ntime in last mode(msec):%llu\n"
		"time since last mode(sec):%llu\nactual last sleep(msec):%llu\n\n",
		stat_type, data->count, time_in_last_mode,
		time_since_last_mode, actual_last_sleep);
#endif
}

static inline u32 msm_rpmstats_read_long_register(void __iomem *regbase,
		int index, int offset)
{
	return readl_relaxed(regbase + offset +
			index * sizeof(struct msm_rpm_stats_data));
}

static inline u32 msm_aop_ss_stats_read_long_register(void __iomem *regbase,
		int index, int offset)
{
	return readl_relaxed(regbase + offset +
			index * sizeof(struct msm_aop_ss_state));
}

static inline u64 msm_rpmstats_read_quad_register(void __iomem *regbase,
		int index, int offset)
{
	u64 dst;

	memcpy_fromio(&dst,
		regbase + offset + index * sizeof(struct msm_rpm_stats_data),
		8);
	return dst;
}

static inline int msm_rpmstats_copy_stats(
			struct msm_rpmstats_private_data *prvdata)
{
	void __iomem *reg;
	struct msm_rpm_stats_data data;
	int i, length;

	reg = prvdata->reg_base;

	for (i = 0, length = 0; i < prvdata->num_records; i++) {
		data.stat_type = msm_rpmstats_read_long_register(reg, i,
				offsetof(struct msm_rpm_stats_data,
					stat_type));
		data.count = msm_rpmstats_read_long_register(reg, i,
				offsetof(struct msm_rpm_stats_data, count));
		data.last_entered_at = msm_rpmstats_read_quad_register(reg,
				i, offsetof(struct msm_rpm_stats_data,
					last_entered_at));
		data.last_exited_at = msm_rpmstats_read_quad_register(reg,
				i, offsetof(struct msm_rpm_stats_data,
					last_exited_at));
		data.accumulated = msm_rpmstats_read_quad_register(reg,
				i, offsetof(struct msm_rpm_stats_data,
					accumulated));
#if defined(CONFIG_MSM_RPM_SMD)
		data.client_votes = msm_rpmstats_read_long_register(reg,
				i, offsetof(struct msm_rpm_stats_data,
					client_votes));
#endif

		length += msm_rpmstats_append_data_to_buf(prvdata->buf + length,
				&data, sizeof(prvdata->buf) - length);
		prvdata->read_idx++;
	}

	return length;
}

#ifdef CONFIG_HTC_POWER_DEBUG
enum aop_sub_system_name_v2 {
	APPS_SOC,
	SP_SOC,
	AUDIO_SOC,
	SENSOR_SOC,
	AOP_SOC,
	DEBUG_SOC,
	GPU_SOC,
	DISPLAY_SOC,
	COMPUTE_SOC,
	MODEM_SOC,
};

struct aop_sub_system {
	char	*name;
};

static struct aop_sub_system sdm845_v2_aop_ss[] = {
	[APPS_SOC]	= {
		.name	= "ap",
	},
	[SP_SOC]	= {
		.name	= "sp",
	},
	[AUDIO_SOC]	= {
		.name	= "aud",
	},
	[SENSOR_SOC]		= {
		.name	= "ssc",
	},
	[AOP_SOC]		= {
		.name	= "aop",
	},
	[DEBUG_SOC]	= {
		.name	= "dbg",
	},
	[GPU_SOC]	= {
		.name	= "gpu",
	},
	[DISPLAY_SOC]	= {
		.name	= "disp",
	},
	[COMPUTE_SOC]	= {
		.name	= "cdsp",
	},
	[MODEM_SOC]	= {
		.name	= "mdm",
	},
};

struct msm_rpmstats_platform_data *htc_pdata;

struct htc_sleep_state_base {
	void __iomem *system_state_base;
	void __iomem *sleep_state_base;
	void __iomem *sleep_state_cnt_base;
};

struct htc_sleep_state_base htc_base;

#define SLEEP_STATS_BUF  1024
#define SLEEP_STATS_PIECE  64

/* In order to remap the ss_sleep_state pointer*/
/* Below offset need to correspond to AOP code */
/* i.e. aop_info_0_t structure in msg_ram_layout.h*/
#define REVERSED 210
#define WDOG_COOKIE 4
#define AOP_SUBSYSTEM_NUM 10
#define SS_STATE_CHANGE_CNT_OFFSET 4
#define SS_SLEEP_STATE_OFFSET (sizeof(struct msm_rpm_stats_data) * RPM_STATS_NUM_REC) + WDOG_COOKIE + (REVERSED * 4)

int htc_show_aop_ss_sleep_state(char *buf, char *piece)
{
	struct msm_aop_ss_state sleep_state;
	void __iomem *reg;
	int index;

	reg = htc_base.sleep_state_base;

        if (!reg)
		return -ENOMEM;

	sleep_state.info = msm_aop_ss_stats_read_long_register(reg, 0,
			offsetof(struct msm_aop_ss_state, info));

	snprintf(piece, SLEEP_STATS_PIECE, "(sleep_st, %X),", sleep_state.info);

	if(strlen(buf) + strlen(piece) < SLEEP_STATS_BUF)
		strncat(buf, piece, SLEEP_STATS_BUF - strlen(buf) - 1);

	reg = htc_base.sleep_state_cnt_base;

        if (!reg)
                return -ENOMEM;

	for (index = 0; index < AOP_SUBSYSTEM_NUM; index++) {
		sleep_state.info = msm_aop_ss_stats_read_long_register(reg, index,
                      offsetof(struct msm_aop_ss_state, info));

		snprintf(piece, SLEEP_STATS_PIECE,
				"(%s, %d),",
				sdm845_v2_aop_ss[index].name, sleep_state.info);

		if(strlen(buf) + strlen(piece) < SLEEP_STATS_BUF)
			strncat(buf, piece, SLEEP_STATS_BUF - strlen(buf) - 1);
	}

	buf[strlen(buf) - 1] = '\0';
	printk("[K] system_sleep_stats: %s\n", buf);

	return 0;
}

int htc_show_system_stats(void)
{
	struct msm_rpm_stats_data data;
	void __iomem *reg;
	char stat_type[5];
	u64 time_in_last_mode;
	u64 time_since_last_mode;
	u64 actual_last_sleep;
	int i;
	char buf[SLEEP_STATS_BUF];
	char piece[SLEEP_STATS_PIECE];

	if(!htc_pdata)
	{
		pr_err("[K] msm_rpmstats_platform_data is NULL\n");
		return -ENOMEM;
	}

	reg = htc_base.system_state_base;

	if (!reg) {
		pr_err("%s: ERROR could not ioremap base=%pa, size=%u\n",
			__func__, &htc_pdata->phys_addr_base,
			htc_pdata->phys_size);
		return -ENOMEM;
	}

	memset(buf, 0, SLEEP_STATS_BUF);
	memset(piece, 0, SLEEP_STATS_PIECE);

	for (i = 0; i < RPM_STATS_NUM_REC; i++) {
                data.stat_type = msm_rpmstats_read_long_register(reg, i,
                                offsetof(struct msm_rpm_stats_data,
                                        stat_type));
                data.count = msm_rpmstats_read_long_register(reg, i,
                                offsetof(struct msm_rpm_stats_data, count));
                data.last_entered_at = msm_rpmstats_read_quad_register(reg,
                                i, offsetof(struct msm_rpm_stats_data,
                                        last_entered_at));
                data.last_exited_at = msm_rpmstats_read_quad_register(reg,
                                i, offsetof(struct msm_rpm_stats_data,
                                        last_exited_at));
                data.accumulated = msm_rpmstats_read_quad_register(reg,
                                i, offsetof(struct msm_rpm_stats_data,
                                        accumulated));

		stat_type[4] = 0;
		memcpy(stat_type, &data.stat_type, sizeof(u32));

		time_in_last_mode = data.last_exited_at - data.last_entered_at;
		time_in_last_mode = get_time_in_msec(time_in_last_mode);
		time_since_last_mode = arch_counter_get_cntvct() - data.last_exited_at;
		time_since_last_mode = get_time_in_sec(time_since_last_mode);
		actual_last_sleep = get_time_in_msec(data.accumulated);

		snprintf(piece, SLEEP_STATS_PIECE,
				"(%s, %d, %llu, %llu, %llu),",
				stat_type, data.count, time_in_last_mode,
				time_since_last_mode, actual_last_sleep);
		if(strlen(buf) + strlen(piece) <= SLEEP_STATS_BUF)
			strncat(buf, piece, sizeof(buf) - strlen(buf) - 1);
	}
	htc_show_aop_ss_sleep_state(buf, piece);

	return 0;
}
#endif

static ssize_t rpmstats_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	struct msm_rpmstats_private_data prvdata;
	struct msm_rpmstats_platform_data *pdata = NULL;

	pdata = GET_PDATA_OF_ATTR(attr);

	prvdata.reg_base = ioremap_nocache(pdata->phys_addr_base,
					pdata->phys_size);
	if (!prvdata.reg_base) {
		pr_err("%s: ERROR could not ioremap start=%pa, len=%u\n",
			__func__, &pdata->phys_addr_base,
			pdata->phys_size);
		return -EBUSY;
	}

	prvdata.read_idx = prvdata.len = 0;
	prvdata.platform_data = pdata;
	prvdata.num_records = RPM_STATS_NUM_REC;

	if (prvdata.read_idx < prvdata.num_records)
		prvdata.len = msm_rpmstats_copy_stats(&prvdata);

	return snprintf(buf, prvdata.len, "%s", prvdata.buf);
}

static int msm_rpmstats_create_sysfs(struct msm_rpmstats_platform_data *pd)
{
	struct kobject *rpmstats_kobj = NULL;
	struct msm_rpmstats_kobj_attr *rpms_ka = NULL;
	int ret = 0;

	rpmstats_kobj = kobject_create_and_add("system_sleep", power_kobj);
	if (!rpmstats_kobj) {
		pr_err("%s: Cannot create rpmstats kobject\n", __func__);
		ret = -ENOMEM;
		goto fail;
	}

	rpms_ka = kzalloc(sizeof(*rpms_ka), GFP_KERNEL);
	if (!rpms_ka) {
		kobject_put(rpmstats_kobj);
		ret = -ENOMEM;
		goto fail;
	}

	sysfs_attr_init(&rpms_ka->ka.attr);
	rpms_ka->pd = pd;
	rpms_ka->ka.attr.mode = 0444;
	rpms_ka->ka.attr.name = "stats";
	rpms_ka->ka.show = rpmstats_show;
	rpms_ka->ka.store = NULL;

	ret = sysfs_create_file(rpmstats_kobj, &rpms_ka->ka.attr);

fail:
	return ret;
}

static int msm_rpmstats_probe(struct platform_device *pdev)
{
	struct msm_rpmstats_platform_data *pdata;
	struct msm_rpmstats_platform_data *pd;
	struct resource *res = NULL, *offset = NULL;
	u32 offset_addr = 0;
	void __iomem *phys_ptr = NULL;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"phys_addr_base");
	if (!res)
		return -EINVAL;

	offset = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"offset_addr");
	if (offset) {
		/* Remap the rpm-stats pointer */
		phys_ptr = ioremap_nocache(offset->start, SZ_4);
		if (!phys_ptr) {
			pr_err("%s: Failed to ioremap address: %x\n",
					__func__, offset_addr);
			return -ENODEV;
		}
		offset_addr = readl_relaxed(phys_ptr);
		iounmap(phys_ptr);
	}

	pdata->phys_addr_base  = res->start + offset_addr;
	pdata->phys_size = resource_size(res);

	if (pdev->dev.platform_data)
		pd = pdev->dev.platform_data;

	msm_rpmstats_create_sysfs(pdata);
#ifdef CONFIG_HTC_POWER_DEBUG
	htc_pdata = pdata;

        htc_base.system_state_base = ioremap_nocache(
                        htc_pdata->phys_addr_base,
                        htc_pdata->phys_size);

        htc_base.sleep_state_base = ioremap_nocache(
                        htc_pdata->phys_addr_base + SS_SLEEP_STATE_OFFSET,
                        htc_pdata->phys_size);

        htc_base.sleep_state_cnt_base = ioremap_nocache(
                        htc_pdata->phys_addr_base + SS_SLEEP_STATE_OFFSET + SS_STATE_CHANGE_CNT_OFFSET,
                        htc_pdata->phys_size);

        if (!htc_base.system_state_base||!htc_base.sleep_state_base||!htc_base.sleep_state_cnt_base) {
                pr_err("%s: ERROR could not ioremap base=%pa, size=%u\n",
                        __func__, &htc_pdata->phys_addr_base,
                        htc_pdata->phys_size);
        }
#endif

	return 0;
}

static const struct of_device_id rpm_stats_table[] = {
	{ .compatible = "qcom,rpm-stats" },
	{ },
};

static struct platform_driver msm_rpmstats_driver = {
	.probe = msm_rpmstats_probe,
	.driver = {
		.name = "msm_rpm_stat",
		.owner = THIS_MODULE,
		.of_match_table = rpm_stats_table,
	},
};
builtin_platform_driver(msm_rpmstats_driver);
