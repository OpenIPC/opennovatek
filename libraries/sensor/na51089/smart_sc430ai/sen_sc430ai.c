/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Open-source SmartSens SC430AI MIPI CMOS sensor driver for Novatek NT9856x
 * (2022 HDAL ABI, kflow_videocapture).
 *
 * Supported modes:
 *   0: 2688x1520 @ 30fps, 10-bit linear
 *   1: 2688x1520 @ 60fps, 10-bit linear
 *   2: 2688x1520 @ 30fps, 10-bit 2-frame Stagger HDR
 *
 * Unlike the SDK drivers in this tree, this one is self-contained: it has its
 * own I2C access and cfg parser instead of ../common. The HDAL interface
 * types come from the SDK headers (kflow_videocapture/ctl_sen.h, isp_api.h).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/gpio/consumer.h>

#include "kwrap/error_no.h"
#include "kwrap/type.h"
#include "kflow_videocapture/ctl_sen.h"
#include "isp_api.h"

#include "sc430ai_regs.h"

#define SEN_MAX_MODE		3

/* The cfg file's [PRESET]: exposure and gain until the ISP sends its own. */
struct sen_preset_cfg {
	u32 expt_time;
	u32 gain_ratio;
};

struct sen_power_cfg {
	u32 mclk;
	u32 pwdn_pin;
	u32 rst_pin;
	u32 rst_time;
	u32 stable_time;
};

struct sen_i2c_cfg {
	u32 id;
	u32 addr;
};

static char *sen_cfg_path = "null";
module_param(sen_cfg_path, charp, 0644);
MODULE_PARM_DESC(sen_cfg_path, "Path of cfg file");

static int sen_debug_level;
module_param(sen_debug_level, int, 0644);
MODULE_PARM_DESC(sen_debug_level, "Debug message level");

static const CTL_SENDRV_GET_ATTR_BASIC_PARAM basic_param = {
	.name = "sen_sc430ai",
	.vendor = CTL_SEN_VENDOR_OTHERS,
	.max_senmode = SEN_MAX_MODE,
	.property = CTL_SEN_SUPPORT_PROPERTY_MIRROR | CTL_SEN_SUPPORT_PROPERTY_FLIP |
		    CTL_SEN_SUPPORT_PROPERTY_CHGFPS,
	.sync_timing = 0,
};

static const CTL_SENDRV_GET_ATTR_SIGNAL_PARAM signal_param = {
	.type = CTL_SEN_SIGNAL_MASTER,
	.info = { CTL_SEN_ACTIVE_HIGH, CTL_SEN_ACTIVE_HIGH, CTL_SEN_PHASE_RISING,
		  CTL_SEN_PHASE_RISING, CTL_SEN_PHASE_RISING },
};

static const CTL_SENDRV_GET_MODE_BASIC_PARAM mode_basic_param[SEN_MAX_MODE] = {
	{
		.mode = CTL_SEN_MODE_1,
		.if_type = CTL_SEN_IF_TYPE_MIPI,
		.data_fmt = CTL_SEN_DATA_FMT_RGB,
		.mode_type = CTL_SEN_MODE_LINEAR,
		.dft_fps = 3000,
		.frame_num = 1,
		.stpix = CTL_SEN_STPIX_R,
		.pixel_depth = CTL_SEN_PIXDEPTH_10BIT,
		.fmt = CTL_SEN_FMT_POGRESSIVE,
		.valid_size = { 2688, 1522 },
		.act_size = { { 0, 2, 2688, 1520 } },
		.crp_size = { 2688, 1520 },
		.signal_info = { 0, 1600, 0, 1650 },
		.ratio_h_v = CTL_SEN_RATIO(16, 9),
		.gain = { 1000, 301178 },
		.bining_ratio = 100,
	},
	{
		.mode = CTL_SEN_MODE_2,
		.if_type = CTL_SEN_IF_TYPE_MIPI,
		.data_fmt = CTL_SEN_DATA_FMT_RGB,
		.mode_type = CTL_SEN_MODE_LINEAR,
		.dft_fps = 6000,
		.frame_num = 1,
		.stpix = CTL_SEN_STPIX_R,
		.pixel_depth = CTL_SEN_PIXDEPTH_10BIT,
		.fmt = CTL_SEN_FMT_POGRESSIVE,
		.valid_size = { 2688, 1522 },
		.act_size = { { 0, 2, 2688, 1520 } },
		.crp_size = { 2688, 1520 },
		.signal_info = { 0, 1600, 0, 1650 },
		.ratio_h_v = CTL_SEN_RATIO(16, 9),
		.gain = { 1000, 301178 },
		.bining_ratio = 100,
	},
	{
		.mode = CTL_SEN_MODE_3,
		.if_type = CTL_SEN_IF_TYPE_MIPI,
		.data_fmt = CTL_SEN_DATA_FMT_RGB,
		.mode_type = CTL_SEN_MODE_STAGGER_HDR,
		.dft_fps = 3000,
		.frame_num = 2,
		.stpix = CTL_SEN_STPIX_R,
		.pixel_depth = CTL_SEN_PIXDEPTH_10BIT,
		.fmt = CTL_SEN_FMT_POGRESSIVE,
		.valid_size = { 2688, 1522 },
		.act_size = { { 0, 2, 2688, 1520 }, { 0, 2, 2688, 1520 } },
		.crp_size = { 2688, 1520 },
		.signal_info = { 0, 1600, 0, 3300 },
		.ratio_h_v = CTL_SEN_RATIO(16, 9),
		.gain = { 1000, 301178 },
		.bining_ratio = 100,
	},
};

static const CTL_SENDRV_GET_SPEED_PARAM speed_param[SEN_MAX_MODE] = {
	{ CTL_SEN_MODE_1, CTL_SEN_SIEMCLK_SRC_DFT, 27000000, 158400000, 79200000 },
	{ CTL_SEN_MODE_2, CTL_SEN_SIEMCLK_SRC_DFT, 27000000, 316800000, 158400000 },
	{ CTL_SEN_MODE_3, CTL_SEN_SIEMCLK_SRC_DFT, 27000000, 316800000, 158400000 },
};

static const CTL_SENDRV_GET_MODE_MIPI_PARAM mipi_param[SEN_MAX_MODE] = {
	{
		.mode = CTL_SEN_MODE_1,
		.clk_lane = CTL_SEN_CLKLANE_1,
		.data_lane = CTL_SEN_DATALANE_4,
		.sel_bit_ofs = SEN_BIT_OFS_NONE,
	},
	{
		.mode = CTL_SEN_MODE_2,
		.clk_lane = CTL_SEN_CLKLANE_1,
		.data_lane = CTL_SEN_DATALANE_4,
		.sel_bit_ofs = SEN_BIT_OFS_NONE,
	},
	{
		.mode = CTL_SEN_MODE_3,
		.clk_lane = CTL_SEN_CLKLANE_1,
		.data_lane = CTL_SEN_DATALANE_4,
		.sel_frm_id = { 1, 0, 0, 0 },
		.sel_bit_ofs = SEN_BIT_OFS_0 | SEN_BIT_OFS_1,
	},
};

static const CTL_SENDRV_I2C i2c_table_default = {
	.w_addr_info = {
		{ CTL_SEN_I2C_W_ADDR_DFT,     0x60 },
		{ CTL_SEN_I2C_W_ADDR_OPTION1, 0xff },
		{ CTL_SEN_I2C_W_ADDR_OPTION2, 0xff },
		{ CTL_SEN_I2C_W_ADDR_OPTION3, 0xff },
		{ CTL_SEN_I2C_W_ADDR_OPTION4, 0xff },
		{ CTL_SEN_I2C_W_ADDR_OPTION5, 0xff },
	},
};

struct sc430ai_state {
	struct i2c_adapter *adap;
	u32 cur_mode;
	u32 cur_fps;
	u32 chg_fps;
	u32 direction;
	u32 compensation_gain[2];
	ISP_SENSOR_CTRL ctrl;
	struct sen_preset_cfg preset;
	ISP_SENSOR_PRESET_CTRL preset_ctrl;
	struct sen_power_cfg power;
	struct sen_i2c_cfg i2c;
};

static struct sc430ai_state g_sen[CTL_SEN_ID_MAX];

static int sc430ai_i2c_write(u32 id, u16 reg, u8 val)
{
	struct i2c_adapter *adap = g_sen[id].adap;
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = {
		.addr = g_sen[id].i2c.addr,
		.flags = 0,
		.len = 3,
		.buf = buf,
	};
	int ret;

	if (!adap)
		return -ENODEV;
	ret = i2c_transfer(adap, &msg, 1);
	return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

static int sc430ai_i2c_read(u32 id, u16 reg, u8 *val)
{
	struct i2c_adapter *adap = g_sen[id].adap;
	u8 rbuf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = g_sen[id].i2c.addr, .flags = 0, .len = 2, .buf = rbuf },
		{ .addr = g_sen[id].i2c.addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret;

	if (!adap)
		return -ENODEV;
	ret = i2c_transfer(adap, msgs, 2);
	return (ret == 2) ? 0 : (ret < 0 ? ret : -EIO);
}

/*
 * Time per exposure step in 0.1 us. The SC430AI exposure registers count
 * half-lines (2 * VTS per frame), and hd_period / pclk is a half-line here:
 * 1600 / 158.4 MHz = 10.1 us, and 3300 half-lines of it make 33.3 ms.
 */
static u32 sc430ai_calc_rowtime_10x(u32 id, u32 mode)
{
	u32 pclk_100k;

	if (mode >= SEN_MAX_MODE)
		mode = g_sen[id].cur_mode;
	pclk_100k = speed_param[mode].pclk / 100000U;
	if (!pclk_100k)
		return 1;
	return (100U * mode_basic_param[mode].signal_info.hd_period) / pclk_100k;
}

static u32 sc430ai_calc_vd(u32 id, u32 fps)
{
	u32 mode = g_sen[id].cur_mode;
	u32 dft_fps = mode_basic_param[mode].dft_fps;
	u32 dft_vd = mode_basic_param[mode].signal_info.vd_period;
	u32 vd;

	if (!fps || fps > dft_fps)
		fps = dft_fps;
	vd = (dft_fps * dft_vd) / fps;
	if (vd < dft_vd)
		vd = dft_vd;
	if (vd > 0x7fff)
		vd = 0x7fff;
	g_sen[id].cur_fps = fps;
	return vd;
}

static int sc430ai_set_flip(u32 id, u32 dir)
{
	u8 val = 0;
	int ret;

	ret = sc430ai_i2c_read(id, 0x3221, &val);
	if (ret)
		return ret;
	if (dir & CTL_SEN_FLIP_H)
		val |= 0x06;
	else
		val &= ~0x06;
	if (dir & CTL_SEN_FLIP_V)
		val |= 0x60;
	else
		val &= ~0x60;
	g_sen[id].direction = dir & CTL_SEN_FLIP_H_V;
	return sc430ai_i2c_write(id, 0x3221, val);
}

static u32 sc430ai_align_hdr_line(u32 line)
{
	if (line & 3) {
		line &= ~3U;
		if (line <= 4)
			line = 5;
	}
	return line;
}

static int sc430ai_set_expt(u32 id, const ISP_SENSOR_CTRL *ctrl)
{
	u32 mode = g_sen[id].cur_mode;
	const CTL_SENDRV_GET_MODE_BASIC_PARAM *mb = &mode_basic_param[mode];
	u32 rowtime = sc430ai_calc_rowtime_10x(id, mode);
	u32 line[2] = { 0, 0 }, req_line[2] = { 0, 0 };
	u32 total_lines, fps, vd, max_long;
	u32 i, n = mb->frame_num ? mb->frame_num : 1;
	int ret;

	g_sen[id].ctrl.exp_time[0] = ctrl->exp_time[0];
	g_sen[id].ctrl.exp_time[1] = ctrl->exp_time[1];

	for (i = 0; i < n && i < 2; i++) {
		line[i] = (ctrl->exp_time[i] * 10U) / rowtime;
		if (mb->mode_type == CTL_SEN_MODE_LINEAR) {
			if (line[i] < 3)
				line[i] = 3;
		} else {
			if (line[i] < 5)
				line[i] = 5;
		}
		req_line[i] = line[i];
	}

	if (mb->mode_type == CTL_SEN_MODE_STAGGER_HDR && line[1] > 0x180)
		line[1] = 0x180;

	total_lines = (mb->mode_type == CTL_SEN_MODE_STAGGER_HDR) ?
		      (line[0] + line[1]) : line[0];
	fps = (total_lines * rowtime) / 10U;
	fps = fps ? (100000000U / fps) : mb->dft_fps;
	if (fps > g_sen[id].chg_fps)
		fps = g_sen[id].chg_fps;
	if (!fps)
		fps = mb->dft_fps;
	vd = (mb->dft_fps * mb->signal_info.vd_period) / fps;
	if (vd < mb->signal_info.vd_period)
		vd = mb->signal_info.vd_period;
	if (vd > 0x7fff)
		vd = 0x7fff;
	g_sen[id].cur_fps = fps;

	ret = sc430ai_i2c_write(id, 0x320e, (vd >> 8) & 0x7f);
	ret |= sc430ai_i2c_write(id, 0x320f, vd & 0xff);

	if (mb->mode_type == CTL_SEN_MODE_LINEAR) {
		max_long = (vd - 5) * 2;
		if (line[0] > 0xffff)
			line[0] = 0xffff;
		if (line[0] > max_long)
			line[0] = max_long;
		g_sen[id].compensation_gain[0] = (100U * req_line[0]) / line[0];

		ret |= sc430ai_i2c_write(id, 0x3e00, (line[0] >> 12) & 0x0f);
		ret |= sc430ai_i2c_write(id, 0x3e01, (line[0] >> 4) & 0xff);
		ret |= sc430ai_i2c_write(id, 0x3e02, (line[0] << 4) & 0xf0);
	} else {
		if (line[0] > 0xffff)
			line[0] = 0xffff;
		if (line[1] > 0x180)
			line[1] = 0x180;
		max_long = (vd > 0xd0) ? ((vd - 0xd0) * 2) : 5;
		if (line[0] > max_long)
			line[0] = max_long;
		line[0] = sc430ai_align_hdr_line(line[0]);
		line[1] = sc430ai_align_hdr_line(line[1]);
		g_sen[id].compensation_gain[0] = (100U * req_line[0]) / line[0];
		g_sen[id].compensation_gain[1] = (100U * req_line[1]) / line[1];

		ret |= sc430ai_i2c_write(id, 0x3e00, (line[0] >> 12) & 0x0f);
		ret |= sc430ai_i2c_write(id, 0x3e01, (line[0] >> 4) & 0xff);
		ret |= sc430ai_i2c_write(id, 0x3e02, (line[0] << 4) & 0xf0);
		ret |= sc430ai_i2c_write(id, 0x3e22, (line[1] >> 12) & 0x0f);
		ret |= sc430ai_i2c_write(id, 0x3e04, (line[1] >> 4) & 0xff);
		ret |= sc430ai_i2c_write(id, 0x3e05, (line[1] << 4) & 0xf0);
	}
	return ret;
}

static void sc430ai_calc_gain(u32 gain, u32 comp, u32 min_g, u32 max_g,
			      u8 *again_reg, u8 *dgain_coarse, u8 *dgain_fine)
{
	u32 dgain_1000, fine;

	if (comp >= 100)
		gain = (gain * comp) / 100U;
	if (gain < min_g)
		gain = min_g;
	if (gain > max_g)
		gain = max_g;

	*dgain_coarse = 0;

	/* SmartSens SC430AI DCG analog gain steps (2.39x DCG jump at 0x40) */
	if (gain < 2000) {
		*again_reg = 0x00;
		dgain_1000 = gain;
	} else if (gain <= 2389) {
		*again_reg = 0x01;
		dgain_1000 = gain / 2U;
	} else if (gain <= 4779) {
		*again_reg = 0x40;
		dgain_1000 = (gain * 1000U) / 2390U;
	} else if (gain <= 9559) {
		*again_reg = 0x48;
		dgain_1000 = (gain * 1000U) / 4780U;
	} else if (gain <= 19119) {
		*again_reg = 0x49;
		dgain_1000 = (gain * 1000U) / 9560U;
	} else if (gain <= 38239) {
		*again_reg = 0x4b;
		dgain_1000 = (gain * 1000U) / 19120U;
	} else if (gain <= 76479) {
		*again_reg = 0x4f;
		dgain_1000 = (gain * 1000U) / 38240U;
	} else {
		*again_reg = 0x5f;
		dgain_1000 = (gain * 1000U) / 76480U;
		if (gain > 152959) {
			*dgain_coarse = 0x01;
			dgain_1000 /= 2U;
		}
	}

	fine = (dgain_1000 * 128U) / 1000U;
	if (fine & 3)
		fine = ((fine - 0x80) & ~3U) + 0x80;
	if (fine < 0x80)
		fine = 0x80;
	if (fine > 0xfc)
		fine = 0xfc;
	*dgain_fine = (u8)fine;
}

static int sc430ai_set_gain(u32 id, const ISP_SENSOR_CTRL *ctrl)
{
	u32 mode = g_sen[id].cur_mode;
	const CTL_SENDRV_GET_MODE_BASIC_PARAM *mb = &mode_basic_param[mode];
	u8 ag[2] = { 0, 0 }, dc[2] = { 0, 0 }, df[2] = { 0x80, 0x80 };
	u32 i, n = mb->frame_num ? mb->frame_num : 1;
	int ret;

	g_sen[id].ctrl.gain_ratio[0] = ctrl->gain_ratio[0];
	g_sen[id].ctrl.gain_ratio[1] = ctrl->gain_ratio[1];

	for (i = 0; i < n && i < 2; i++)
		sc430ai_calc_gain(ctrl->gain_ratio[i], g_sen[id].compensation_gain[i],
				  mb->gain.min, mb->gain.max, &ag[i], &dc[i], &df[i]);

	ret = sc430ai_i2c_write(id, 0x3e09, ag[0]);
	ret |= sc430ai_i2c_write(id, 0x3e06, dc[0]);
	ret |= sc430ai_i2c_write(id, 0x3e07, df[0]);
	if (mb->mode_type == CTL_SEN_MODE_STAGGER_HDR) {
		ret |= sc430ai_i2c_write(id, 0x3e13, ag[1]);
		ret |= sc430ai_i2c_write(id, 0x3e10, dc[1]);
		ret |= sc430ai_i2c_write(id, 0x3e11, df[1]);
	}
	return ret;
}

static void sc430ai_pwr_ctrl(CTL_SEN_ID id, CTL_SEN_PWR_CTRL_FLAG flag, CTL_SEN_CLK_CB clk_cb)
{
	struct sen_power_cfg *p;
	struct gpio_desc *gd;

	if (id >= CTL_SEN_ID_MAX)
		return;
	p = &g_sen[id].power;

	if (flag == CTL_SEN_PWR_CTRL_TURN_ON) {
		if (p->pwdn_pin != CTL_SEN_IGNORE) {
			gd = gpio_to_desc(p->pwdn_pin);
			if (gd) {
				gpiod_direction_output_raw(gd, 0);
				gpiod_set_raw_value(gd, 0);
				gpiod_set_raw_value(gd, 1);
			}
		}
		if (clk_cb && p->mclk != CTL_SEN_IGNORE)
			clk_cb(p->mclk, TRUE);
		if (p->rst_pin != CTL_SEN_IGNORE) {
			msleep(p->stable_time);
			gd = gpio_to_desc(p->rst_pin);
			if (gd) {
				gpiod_direction_output_raw(gd, 0);
				gpiod_set_raw_value(gd, 0);
				msleep(p->rst_time);
				gpiod_set_raw_value(gd, 1);
				msleep(p->stable_time);
			}
		}
	} else {
		if (p->rst_pin != CTL_SEN_IGNORE) {
			gd = gpio_to_desc(p->rst_pin);
			if (gd)
				gpiod_set_raw_value(gd, 0);
		}
		if (clk_cb && p->mclk != CTL_SEN_IGNORE)
			clk_cb(p->mclk, FALSE);
		if (p->pwdn_pin != CTL_SEN_IGNORE) {
			gd = gpio_to_desc(p->pwdn_pin);
			if (gd)
				gpiod_set_raw_value(gd, 0);
		}
	}
}

static ER sc430ai_open(CTL_SEN_ID id)
{
	if (id >= CTL_SEN_ID_MAX)
		return -EINVAL;
	g_sen[id].preset_ctrl.mode = ISP_SENSOR_PRESET_DEFAULT;
	if (g_sen[id].adap)
		return 0;
	g_sen[id].adap = i2c_get_adapter(g_sen[id].i2c.id);
	if (!g_sen[id].adap) {
		pr_err("nvt_sen_sc430ai: i2c adapter %u not found\n", g_sen[id].i2c.id);
		return -ENODEV;
	}
	return 0;
}

static ER sc430ai_close(CTL_SEN_ID id)
{
	if (id >= CTL_SEN_ID_MAX)
		return -EINVAL;
	if (g_sen[id].adap) {
		i2c_put_adapter(g_sen[id].adap);
		g_sen[id].adap = NULL;
	}
	return 0;
}

static ER sc430ai_sleep(CTL_SEN_ID id)
{
	return 0;
}

static ER sc430ai_wakeup(CTL_SEN_ID id)
{
	return 0;
}

static ER sc430ai_write_reg(CTL_SEN_ID id, CTL_SEN_CMD *cmd)
{
	if (id >= CTL_SEN_ID_MAX || !cmd)
		return -EINVAL;
	return sc430ai_i2c_write(id, (u16)cmd->addr, (u8)cmd->data[0]);
}

static ER sc430ai_read_reg(CTL_SEN_ID id, CTL_SEN_CMD *cmd)
{
	u8 val = 0;
	int ret;

	if (id >= CTL_SEN_ID_MAX || !cmd)
		return -EINVAL;
	ret = sc430ai_i2c_read(id, (u16)cmd->addr, &val);
	if (!ret)
		cmd->data[0] = val;
	return ret;
}

static ER sc430ai_chg_fps(CTL_SEN_ID id, UINT32 fps)
{
	u32 vd;
	int ret;

	if (id >= CTL_SEN_ID_MAX)
		return -EINVAL;
	g_sen[id].chg_fps = fps;
	vd = sc430ai_calc_vd(id, fps);
	ret = sc430ai_i2c_write(id, 0x320e, (vd >> 8) & 0x7f);
	ret |= sc430ai_i2c_write(id, 0x320f, vd & 0xff);
	return ret;
}

/*
 * Exposure and gain to start a mode with, as the SDK drivers choose them:
 * the cfg preset on the first mode change after open (the short frame of
 * HDR at 1/8 of it), what was last set on later ones, or what the ISP asked
 * for through CTL_SENDRV_CFGID_USER_DEFINE1.
 */
static void sc430ai_get_preset(CTL_SEN_ID id, ISP_SENSOR_CTRL *ctrl)
{
	const ISP_SENSOR_PRESET_CTRL *pc = &g_sen[id].preset_ctrl;

	memset(ctrl, 0, sizeof(*ctrl));
	switch (pc->mode) {
	case ISP_SENSOR_PRESET_CHGMODE:
		*ctrl = g_sen[id].ctrl;
		break;
	case ISP_SENSOR_PRESET_AE:
		ctrl->exp_time[0] = pc->exp_time[0];
		ctrl->exp_time[1] = pc->exp_time[1];
		ctrl->gain_ratio[0] = pc->gain_ratio[0];
		ctrl->gain_ratio[1] = pc->gain_ratio[1];
		break;
	case ISP_SENSOR_PRESET_DEFAULT:
	default:
		ctrl->exp_time[0] = g_sen[id].preset.expt_time;
		ctrl->gain_ratio[0] = g_sen[id].preset.gain_ratio;
		if (mode_basic_param[g_sen[id].cur_mode].frame_num == 2) {
			ctrl->exp_time[1] = g_sen[id].preset.expt_time >> 3;
			ctrl->gain_ratio[1] = g_sen[id].preset.gain_ratio;
		}
		break;
	}
}

static ER sc430ai_chg_mode(CTL_SEN_ID id, CTL_SENDRV_CHGMODE_OBJ obj)
{
	const struct sc430ai_reg *tbl;
	size_t i, n;
	ISP_SENSOR_CTRL ctrl;
	int ret = 0;

	if (id >= CTL_SEN_ID_MAX)
		return -EINVAL;
	if (obj.mode >= SEN_MAX_MODE)
		obj.mode = CTL_SEN_MODE_1;

	g_sen[id].cur_mode = obj.mode;
	g_sen[id].chg_fps = obj.frame_rate ? obj.frame_rate : mode_basic_param[obj.mode].dft_fps;
	g_sen[id].cur_fps = g_sen[id].chg_fps;

	if (obj.mode == CTL_SEN_MODE_2) {
		tbl = sc430ai_mode_2;
		n = ARRAY_SIZE(sc430ai_mode_2);
	} else if (obj.mode == CTL_SEN_MODE_3) {
		tbl = sc430ai_mode_3;
		n = ARRAY_SIZE(sc430ai_mode_3);
	} else {
		tbl = sc430ai_mode_1;
		n = ARRAY_SIZE(sc430ai_mode_1);
	}

	for (i = 0; i < n; i++) {
		if (tbl[i].addr == SEN_CMD_DELAY) {
			msleep(tbl[i].val);
		} else if (tbl[i].addr == SEN_CMD_SETVD) {
			ret |= sc430ai_chg_fps(id, g_sen[id].chg_fps);
		} else if (tbl[i].addr == SEN_CMD_PRESET) {
			sc430ai_get_preset(id, &ctrl);
			ret |= sc430ai_set_expt(id, &ctrl);
			ret |= sc430ai_set_gain(id, &ctrl);
		} else if (tbl[i].addr == SEN_CMD_DIRECTION) {
			ret |= sc430ai_set_flip(id, g_sen[id].direction);
		} else {
			ret |= sc430ai_i2c_write(id, (u16)tbl[i].addr, tbl[i].val);
		}
	}

	/* A later mode change keeps the exposure AE has reached by then. */
	g_sen[id].preset_ctrl.mode = ISP_SENSOR_PRESET_CHGMODE;

	return ret;
}

static ER sc430ai_set_info(CTL_SEN_ID id, CTL_SENDRV_CFGID cfg_id, void *data)
{
	if (id >= CTL_SEN_ID_MAX || !data)
		return -EINVAL;

	switch (cfg_id) {
	case CTL_SENDRV_CFGID_SET_EXPT:
		return sc430ai_set_expt(id, data);
	case CTL_SENDRV_CFGID_SET_GAIN:
		return sc430ai_set_gain(id, data);
	case CTL_SENDRV_CFGID_FLIP_TYPE:
		return sc430ai_set_flip(id, *(CTL_SEN_FLIP *)data);
	case CTL_SENDRV_CFGID_USER_DEFINE1:
		memcpy(&g_sen[id].preset_ctrl, data, sizeof(g_sen[id].preset_ctrl));
		return 0;
	default:
		return E_NOSPT;
	}
}

static ER sc430ai_get_info(CTL_SEN_ID id, CTL_SENDRV_CFGID cfg_id, void *data)
{
	u32 mode;

	if (id >= CTL_SEN_ID_MAX || !data)
		return -EINVAL;

	switch (cfg_id) {
	case CTL_SENDRV_CFGID_GET_EXPT: {
		ISP_SENSOR_CTRL *c = data;

		c->exp_time[0] = g_sen[id].ctrl.exp_time[0];
		c->exp_time[1] = g_sen[id].ctrl.exp_time[1];
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_GAIN: {
		ISP_SENSOR_CTRL *c = data;

		c->gain_ratio[0] = g_sen[id].ctrl.gain_ratio[0];
		c->gain_ratio[1] = g_sen[id].ctrl.gain_ratio[1];
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_ATTR_BASIC:
		memcpy(data, &basic_param, sizeof(basic_param));
		return 0;
	case CTL_SENDRV_CFGID_GET_ATTR_SIGNAL:
		memcpy(data, &signal_param, sizeof(signal_param));
		return 0;
	case CTL_SENDRV_CFGID_GET_ATTR_CMDIF: {
		CTL_SENDRV_GET_ATTR_CMDIF_PARAM *out = data;

		out->type = CTL_SEN_CMDIF_TYPE_I2C;
		memcpy(&out->info.i2c, &i2c_table_default, sizeof(out->info.i2c));
		out->info.i2c.w_addr_info[0].w_addr = g_sen[id].i2c.addr;
		out->info.i2c.cur_w_addr_info.w_addr_sel = out->info.i2c.w_addr_info[0].w_addr_sel;
		out->info.i2c.cur_w_addr_info.w_addr = g_sen[id].i2c.addr;
		out->info.i2c.ch = g_sen[id].i2c.id;
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_ATTR_IF:
		return (((CTL_SENDRV_GET_ATTR_IF_PARAM *)data)->type == CTL_SEN_IF_TYPE_MIPI) ?
		       0 : E_NOSPT;
	case CTL_SENDRV_CFGID_GET_FPS: {
		CTL_SENDRV_GET_FPS_PARAM *out = data;

		out->chg_fps = g_sen[id].chg_fps;
		out->cur_fps = g_sen[id].cur_fps;
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_SPEED: {
		CTL_SENDRV_GET_SPEED_PARAM *out = data;

		mode = out->mode < SEN_MAX_MODE ? out->mode : 0;
		*out = speed_param[mode];
		if (g_sen[id].power.mclk <= CTL_SEN_CLK_SEL_SIEMCLK3)
			out->mclk_src = g_sen[id].power.mclk + CTL_SEN_SIEMCLK_SRC_MCLK;
		else
			out->mclk_src = g_sen[id].power.mclk;
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_MODESEL: {
		CTL_SENDRV_GET_MODESEL_PARAM *p = data;
		u32 fps = p->frame_rate, w = p->size.w, h = p->size.h;

		if (p->if_type != CTL_SEN_IF_TYPE_MIPI || p->data_fmt != CTL_SEN_DATA_FMT_RGB)
			return 0;
		if (p->frame_num == 2 && w <= 2688 && h <= 1520 && fps <= 3000)
			p->mode = CTL_SEN_MODE_3; /* 2-frame Stagger HDR */
		else if (w <= 2688 && h <= 1520 && fps <= 3000)
			p->mode = CTL_SEN_MODE_1; /* 30fps linear */
		else if (w <= 2688 && h <= 1520 && fps <= 6000)
			p->mode = CTL_SEN_MODE_2; /* 60fps linear */
		else
			p->mode = CTL_SEN_MODE_1;
		return 0;
	}
	case CTL_SENDRV_CFGID_GET_MODE_BASIC:
		mode = *(CTL_SEN_MODE *)data;
		if (mode >= SEN_MAX_MODE)
			mode = 0;
		memcpy(data, &mode_basic_param[mode], sizeof(mode_basic_param[0]));
		return 0;
	case CTL_SENDRV_CFGID_GET_MODE_MIPI:
		mode = *(CTL_SEN_MODE *)data;
		if (mode >= SEN_MAX_MODE)
			mode = 0;
		memcpy(data, &mipi_param[mode], sizeof(mipi_param[0]));
		return 0;
	case CTL_SENDRV_CFGID_GET_MODE_ROWTIME: {
		CTL_SENDRV_GET_MODE_ROWTIME_PARAM *out = data;
		u32 rt;

		mode = out->mode < SEN_MAX_MODE ? out->mode : g_sen[id].cur_mode;
		rt = sc430ai_calc_rowtime_10x(id, mode);
		out->row_time_step = (mode_basic_param[mode].mode_type == CTL_SEN_MODE_LINEAR) ? 2 : 4;
		out->row_time = out->row_time_step * rt;
		return 0;
	}
	case CTL_SENDRV_CFGID_FLIP_TYPE: {
		u8 val = 0;
		u32 dir = CTL_SEN_FLIP_NONE;

		if (sc430ai_i2c_read(id, 0x3221, &val) == 0) {
			if (val & 0x06)
				dir |= CTL_SEN_FLIP_H;
			if (val & 0x60)
				dir |= CTL_SEN_FLIP_V;
		} else {
			dir = g_sen[id].direction;
		}
		*(CTL_SEN_FLIP *)data = dir;
		return 0;
	}
	case CTL_SENDRV_CFGID_USER_DEFINE2:
		/* minimum exposure time, us */
		*(u32 *)data = sc430ai_calc_rowtime_10x(id, g_sen[id].cur_mode) / 10U + 1;
		return 0;
	default:
		return E_NOSPT;
	}
}

static CTL_SEN_DRV_TAB sc430ai_drv_tab = {
	.open = sc430ai_open,
	.close = sc430ai_close,
	.sleep = sc430ai_sleep,
	.wakeup = sc430ai_wakeup,
	.write = sc430ai_write_reg,
	.read = sc430ai_read_reg,
	.chgmode = sc430ai_chg_mode,
	.chgfps = sc430ai_chg_fps,
	.set_cfg = sc430ai_set_info,
	.get_cfg = sc430ai_get_info,
};

static void sc430ai_parse_cfg(const char *path)
{
	struct file *f;
	char *buf, *p, *line;
	loff_t pos = 0;
	ssize_t len;

	if (!path || !path[0] || !strcmp(path, "null") || !strcmp(path, "NULL"))
		return;
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return;
	buf = kzalloc(4096, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return;
	}
	len = kernel_read(f, buf, 4095, &pos);
	filp_close(f, NULL);
	if (len <= 0) {
		kfree(buf);
		return;
	}

	p = buf;
	while ((line = strsep(&p, "\r\n")) != NULL) {
		char *eq, *k, *v, *hash;
		unsigned long val;
		u32 id = 0;

		hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		k = strim(line);
		v = strim(eq + 1);
		if (strncmp(k, "id_", 3) != 0 || k[3] < '0' || k[3] >= '0' + CTL_SEN_ID_MAX || k[4] != '_')
			continue;
		id = k[3] - '0';
		k += 5;
		if (kstrtoul(v, 0, &val))
			continue;

		if (!strcmp(k, "i2c_id"))
			g_sen[id].i2c.id = val;
		else if (!strcmp(k, "i2c_addr"))
			g_sen[id].i2c.addr = val;
		else if (!strcmp(k, "mclk"))
			g_sen[id].power.mclk = val;
		else if (!strcmp(k, "pwdn_pin"))
			g_sen[id].power.pwdn_pin = val;
		else if (!strcmp(k, "rst_pin"))
			g_sen[id].power.rst_pin = val;
		else if (!strcmp(k, "rst_time"))
			g_sen[id].power.rst_time = val;
		else if (!strcmp(k, "stable_time"))
			g_sen[id].power.stable_time = val;
		else if (!strcmp(k, "mirror"))
			g_sen[id].direction = (g_sen[id].direction & ~CTL_SEN_FLIP_H) |
					      (val ? CTL_SEN_FLIP_H : 0U);
		else if (!strcmp(k, "flip"))
			g_sen[id].direction = (g_sen[id].direction & ~CTL_SEN_FLIP_V) |
					      (val ? CTL_SEN_FLIP_V : 0U);
		else if (!strcmp(k, "expt_time"))
			g_sen[id].preset.expt_time = val;
		else if (!strcmp(k, "gain_ratio"))
			g_sen[id].preset.gain_ratio = val;
	}
	kfree(buf);
}

static int __init sc430ai_init(void)
{
	CTL_SEN_REG_OBJ reg = {
		.pwr_ctrl = sc430ai_pwr_ctrl,
		.det_plug_in = NULL,
		.drv_tab = &sc430ai_drv_tab,
	};
	int i, ret;

	for (i = 0; i < CTL_SEN_ID_MAX; i++) {
		g_sen[i].cur_fps = 3000;
		g_sen[i].chg_fps = 3000;
		g_sen[i].compensation_gain[0] = 100;
		g_sen[i].compensation_gain[1] = 100;
		g_sen[i].preset.expt_time = 10000;
		g_sen[i].preset.gain_ratio = 1000;
		g_sen[i].power.mclk = CTL_SEN_CLK_SEL_SIEMCLK;
		g_sen[i].power.pwdn_pin = CTL_SEN_IGNORE;
		g_sen[i].power.rst_pin = 0x41; /* S_GPIO_1 */
		g_sen[i].power.rst_time = 1;
		g_sen[i].power.stable_time = 1;
		g_sen[i].i2c.id = 0;
		g_sen[i].i2c.addr = 0x30;
	}

	sc430ai_parse_cfg(sen_cfg_path);

	ret = ctl_sen_reg_sendrv("nvt_sen_sc430ai", &reg);
	if (ret)
		pr_err("nvt_sen_sc430ai: ctl_sen_reg_sendrv failed (%d)\n", ret);
	else
		pr_info("nvt_sen_sc430ai: registered (i2c=%u@0x%02x rst=0x%x, 3 modes)\n",
			g_sen[0].i2c.id, g_sen[0].i2c.addr, g_sen[0].power.rst_pin);
	return ret;
}

static void __exit sc430ai_exit(void)
{
	int i;

	ctl_sen_unreg_sendrv("nvt_sen_sc430ai");
	for (i = 0; i < CTL_SEN_ID_MAX; i++)
		sc430ai_close(i);
}

module_init(sc430ai_init);
module_exit(sc430ai_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenIPC");
MODULE_DESCRIPTION("SmartSens SC430AI sensor driver (2022 HDAL ABI)");
MODULE_VERSION("1.0.0");
