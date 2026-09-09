// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for panels based on Himax HX8394 controller, such as:
 *
 * - HannStar HSD060BHW4 5.99" MIPI-DSI panel
 *
 * Copyright (C) 2021 Kamil Trzciński
 *
 * Based on drivers/gpu/drm/panel/panel-sitronix-st7703.c
 * Copyright (C) Purism SPC 2019
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRV_NAME "panel-himax-hx8394"

/* Manufacturer specific commands sent via DSI, listed in HX8394-F datasheet */
#define HX8394_CMD_SETSEQUENCE	  0xb0
#define HX8394_CMD_SETPOWER	  0xb1
#define HX8394_CMD_SETDISP	  0xb2
#define HX8394_CMD_SETCYC	  0xb4
#define HX8394_CMD_SETVCOM	  0xb6
#define HX8394_CMD_SETTE	  0xb7
#define HX8394_CMD_SETSENSOR	  0xb8
#define HX8394_CMD_SETEXTC	  0xb9
#define HX8394_CMD_SETMIPI	  0xba
#define HX8394_CMD_SETOTP	  0xbb
#define HX8394_CMD_SETREGBANK	  0xbd
#define HX8394_CMD_UNKNOWN5	  0xbf
#define HX8394_CMD_UNKNOWN1	  0xc0
#define HX8394_CMD_SETDGCLUT	  0xc1
#define HX8394_CMD_SETID	  0xc3
#define HX8394_CMD_SETDDB	  0xc4
#define HX8394_CMD_UNKNOWN2	  0xc6
#define HX8394_CMD_SETCABC	  0xc9
#define HX8394_CMD_SETCABCGAIN	  0xca
#define HX8394_CMD_SETPANEL	  0xcc
#define HX8394_CMD_SETOFFSET	  0xd2
#define HX8394_CMD_SETGIP0	  0xd3
#define HX8394_CMD_UNKNOWN3	  0xd4
#define HX8394_CMD_SETGIP1	  0xd5
#define HX8394_CMD_SETGIP2	  0xd6
#define HX8394_CMD_SETGPO	  0xd6
#define HX8394_CMD_UNKNOWN4	  0xd8
#define HX8394_CMD_SETSCALING	  0xdd
#define HX8394_CMD_SETIDLE	  0xdf
#define HX8394_CMD_SETGAMMA	  0xe0
#define HX8394_CMD_SETCHEMODE_DYN 0xe4
#define HX8394_CMD_SETCHE	  0xe5
#define HX8394_CMD_SETCESEL	  0xe6
#define HX8394_CMD_SET_SP_CMD	  0xe9
#define HX8394_CMD_SETREADINDEX	  0xfe
#define HX8394_CMD_GETSPIREAD	  0xff

struct hx8394 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *vcc;
	struct regulator *iovcc;
	enum drm_panel_orientation orientation;

	const struct hx8394_panel_desc *desc;
};

struct hx8394_panel_desc {
	const struct drm_display_mode *mode;
	unsigned int lanes;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	void (*init_sequence)(struct mipi_dsi_multi_context *dsi_ctx);
};

static inline struct hx8394 *panel_to_hx8394(struct drm_panel *panel)
{
	return container_of(panel, struct hx8394, panel);
}



static void hsd060bhw4_init_sequence(struct mipi_dsi_multi_context *ctx)
{
    struct mipi_dsi_device *dsi = ctx->dsi;
    int ret;
	dev_info(&dsi->dev, "HX8394: Starting init sequence\n");
    /* 0xB9: Enable extension command */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){0xb9, 0xff, 0x83, 0x79}, 4);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xB9: %d\n", ret);
    msleep(10);

    /* 0xB1: Set power */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xb1, 0x44, 0x18, 0x18, 0x31, 0x51, 0x50,
        0xd0, 0xd8, 0x58, 0x80, 0x38, 0x38, 0xf8, 0x33, 0x32, 0x22
    }, 17);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xB1: %d\n", ret);
    msleep(10);

    /* 0xB2: Set display */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xb2, 0x80, 0x3c, 0x0a, 0x03, 0x70, 0x50, 0x11, 0x42, 0x1d
    }, 10);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xB2: %d\n", ret);

    /* 0xB4: Set cycle */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xb4, 0x02, 0x7c, 0x02, 0x7c, 0x02, 0x7c, 0x22, 0x86, 0x23, 0x86
    }, 11);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xB4: %d\n", ret);

    /* 0xC7 */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){0xc7, 0x00, 0x00, 0x00, 0xc0}, 5);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xC7: %d\n", ret);

    /* 0xCC (short write) */
    ret = mipi_dsi_dcs_write(dsi, 0xcc, (u8[]){0x02}, 1);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xCC: %d\n", ret);

    /* 0xD2 (short write) */
    ret = mipi_dsi_dcs_write(dsi, 0xd2, (u8[]){0x77}, 1);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xD2: %d\n", ret);

    /* 0xD3 */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xd3, 0x00, 0x07, 0x00, 0x00, 0x00, 0x08, 0x08, 0x32, 0x10,
        0x01, 0x00, 0x01, 0x03, 0x72, 0x03, 0x72, 0x00, 0x08, 0x00,
        0x08, 0x33, 0x33, 0x05, 0x05, 0x37, 0x05, 0x05, 0x37, 0x08,
        0x00, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x01, 0x0f
    }, 38);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xD3: %d\n", ret);

    /* 0xD5 */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xd5, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x07, 0x06, 0x05,
        0x04, 0x03, 0x02, 0x01, 0x00, 0x18, 0x18, 0x21, 0x20, 0x18,
        0x18, 0x19, 0x19, 0x23, 0x22, 0x38, 0x38, 0x78, 0x78, 0x18,
        0x18, 0x18, 0x18, 0x00, 0x00
    }, 35);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xD5: %d\n", ret);

    /* 0xD6 */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xd6, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x18, 0x18, 0x22, 0x23, 0x19,
        0x19, 0x18, 0x18, 0x20, 0x21, 0x38, 0x38, 0x38, 0x38, 0x18,
        0x18, 0x18, 0x18
    }, 33);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xD6: %d\n", ret);

    /* 0xE0: Gamma */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){
        0xe0, 0x00, 0x01, 0x04, 0x20, 0x24, 0x3f, 0x11, 0x33, 0x09,
        0x0a, 0x0c, 0x17, 0x0f, 0x12, 0x15, 0x13, 0x14, 0x0a, 0x15,
        0x16, 0x18, 0x00, 0x01, 0x04, 0x20, 0x24, 0x3f, 0x11, 0x33,
        0x09, 0x0b, 0x0c, 0x17, 0x03, 0x11, 0x14, 0x13, 0x14, 0x0a,
        0x15, 0x16, 0x18
    }, 43);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xE0: %d\n", ret);

    /* 0xB6 */
    ret = mipi_dsi_dcs_write_buffer(dsi, (u8[]){0xb6, 0x5e, 0x5e}, 3);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0xB6: %d\n", ret);

    /* Exit Sleep (0x11) */
    ret = mipi_dsi_dcs_write(dsi, 0x11, NULL, 0);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0x11 (Exit Sleep): %d\n", ret);
    msleep(500);

    /* Display On (0x29) */
    ret = mipi_dsi_dcs_write(dsi, 0x29, NULL, 0);
    if (ret < 0)
        dev_err(&dsi->dev, "Failed to send cmd 0x29 (Display On): %d\n", ret);
    msleep(500);

	dev_info(&dsi->dev, "HX8394: All init commands sent successfully\n");
}

static const struct drm_display_mode hsd060bhw4_mode_30fps = {
    .hdisplay    = 480,
    .hsync_start = 480 + 10,
    .hsync_end   = 480 + 10 + 10,
    .htotal      = 480 + 10 + 10 + 20,
    .vdisplay    = 800,
    .vsync_start = 800 + 10,
    .vsync_end   = 800 + 10 + 10,
    .vtotal      = 800 + 10 + 10 + 10,
    .clock       = 12948,  // 12.948 MHz (30fps)
    .flags       = 0,
    .width_mm    = 68,
    .height_mm   = 136,
};



static const struct drm_display_mode hsd060bhw4_mode_45fps = {
    .hdisplay    = 480,
    .hsync_start = 480 + 10,   // 490
    .hsync_end   = 480 + 10 + 10, // 500
    .htotal      = 480 + 10 + 10 + 20, // 520
    .vdisplay    = 800,
    .vsync_start = 800 + 10,   // 810
    .vsync_end   = 800 + 10 + 10, // 820
    .vtotal      = 800 + 10 + 10 + 10, // 830
    .clock       = 19422,  // 19.422 MHz (45fps)
    .flags       = 0,
    .width_mm    = 68,
    .height_mm   = 136,
};


static const struct hx8394_panel_desc hsd060bhw4_desc_30fps = {
	.mode = &hsd060bhw4_mode_30fps,
	.lanes = 2,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.init_sequence = hsd060bhw4_init_sequence,
};


static const struct hx8394_panel_desc hsd060bhw4_desc_45fps = {
	.mode = &hsd060bhw4_mode_45fps,
	.lanes = 2,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.init_sequence = hsd060bhw4_init_sequence,
};

static int hx8394_enable(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret;

	ctx->desc->init_sequence(&dsi_ctx);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;
	/* Panel is operational 120 msec after reset */
	msleep(120);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	if (dsi_ctx.accum_err)
		goto sleep_in;

	return 0;

sleep_in:
	ret = dsi_ctx.accum_err;
	dsi_ctx.accum_err = 0;

	/* This will probably fail, but let's try orderly power off anyway. */
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);

	return ret;
}

static int hx8394_disable(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50); /* about 3 frames */

	return dsi_ctx.accum_err;
}

static int hx8394_unprepare(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->iovcc);
	regulator_disable(ctx->vcc);

	return 0;
}

static int hx8394_prepare(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	int ret;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	ret = regulator_enable(ctx->vcc);
	if (ret) {
		dev_err(ctx->dev, "Failed to enable vcc supply: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->iovcc);
	if (ret) {
		dev_err(ctx->dev, "Failed to enable iovcc supply: %d\n", ret);
		goto disable_vcc;
	}

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);

	msleep(180);

	return 0;

disable_vcc:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->vcc);
	return ret;
}

static int hx8394_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(ctx->dev, "Failed to add mode %ux%u@%u\n",
			ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static enum drm_panel_orientation hx8394_get_orientation(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs hx8394_drm_funcs = {
	.disable   = hx8394_disable,
	.unprepare = hx8394_unprepare,
	.prepare   = hx8394_prepare,
	.enable	   = hx8394_enable,
	.get_modes = hx8394_get_modes,
	.get_orientation = hx8394_get_orientation,
};

static int hx8394_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx8394 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct hx8394, panel,
				   &hx8394_drm_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset gpio\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->desc = of_device_get_match_data(dev);

	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->format = ctx->desc->format;
	dsi->lanes = ctx->desc->lanes;

	ctx->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ctx->vcc))
		return dev_err_probe(dev, PTR_ERR(ctx->vcc),
				     "Failed to request vcc regulator\n");

	ctx->iovcc = devm_regulator_get(dev, "iovcc");
	if (IS_ERR(ctx->iovcc))
		return dev_err_probe(dev, PTR_ERR(ctx->iovcc),
				     "Failed to request iovcc regulator\n");

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err_probe(dev, ret, "mipi_dsi_attach failed\n");
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	dev_dbg(dev, "%ux%u@%u %ubpp dsi %udl - ready\n",
		ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
		drm_mode_vrefresh(ctx->desc->mode),
		mipi_dsi_pixel_format_to_bpp(dsi->format), dsi->lanes);

	return 0;
}

static void hx8394_remove(struct mipi_dsi_device *dsi)
{
	struct hx8394 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hx8394_of_match[] = {
	{ .compatible = "hannstar,hsd060bhw4-30fps", .data = &hsd060bhw4_desc_30fps },
	{ .compatible = "hannstar,hsd060bhw4-45fps", .data = &hsd060bhw4_desc_45fps },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hx8394_of_match);

static struct mipi_dsi_driver hx8394_driver = {
	.probe	= hx8394_probe,
	.remove = hx8394_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = hx8394_of_match,
	},
};
module_mipi_dsi_driver(hx8394_driver);

MODULE_AUTHOR("Kamil Trzciński <ayufan@ayufan.eu>");
MODULE_DESCRIPTION("DRM driver for Himax HX8394 based MIPI DSI panels");
MODULE_LICENSE("GPL");
