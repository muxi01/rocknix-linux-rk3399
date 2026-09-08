/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_of.h>

/**
 * struct panel_desc - Describes a simple panel.
 */
struct panel_desc {
	/**
	 * @modes: Pointer to array of fixed modes appropriate for this panel.
	 *
	 * If only one mode then this can just be the address of the mode.
	 * NOTE: cannot be used with "timings" and also if this is specified
	 * then you cannot override the mode in the device tree.
	 */
	const struct drm_display_mode *modes;

	/** @num_modes: Number of elements in modes array. */
	unsigned int num_modes;

	/**
	 * @timings: Pointer to array of display timings
	 *
	 * NOTE: cannot be used with "modes" and also these will be used to
	 * validate a device tree override if one is present.
	 */
	const struct display_timing *timings;

	/** @num_timings: Number of elements in timings array. */
	unsigned int num_timings;

	/** @bpc: Bits per color. */
	unsigned int bpc;

	/** @size: Structure containing the physical size of this panel. */
	struct {
		/**
		 * @size.width: Width (in mm) of the active display area.
		 */
		unsigned int width;

		/**
		 * @size.height: Height (in mm) of the active display area.
		 */
		unsigned int height;
	} size;

	/** @delay: Structure containing various delay values for this panel. */
	struct {
		/**
		 * @delay.prepare: Time for the panel to become ready.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * become ready and start receiving video data
		 */
		unsigned int prepare;
		
		/**
		 * @delay.init: Time after sending initialization commands.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * process initialization commands.
		 */
		unsigned int init;

		/**
		 * @delay.enable: Time for the panel to display a valid frame.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * display the first valid frame after starting to receive
		 * video data.
		 */
		unsigned int enable;

		/**
		 * @delay.disable: Time for the panel to turn the display off.
		 *
		 * The time (in milliseconds) that it takes for the panel to
		 * turn the display off (no content is visible).
		 */
		unsigned int disable;

		/**
		 * @delay.unprepare: Time to power down completely.
		 *
		 * The time (in milliseconds) that it takes for the panel
		 * to power itself down completely.
		 *
		 * This time is used to prevent a future "prepare" from
		 * starting until at least this many milliseconds has passed.
		 * If at prepare time less time has passed since unprepare
		 * finished, the driver waits for the remaining time.
		 */
		unsigned int unprepare;
	} delay;

	/** @bus_format: See MEDIA_BUS_FMT_... defines. */
	u32 bus_format;

	/** @bus_flags: See DRM_BUS_FLAG_... defines. */
	u32 bus_flags;

	/** @connector_type: LVDS, eDP, DSI, DPI, etc. */
	int connector_type;
};

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	/* Panel initialization sequence */
	const u8 *init_seq;
	unsigned int init_seq_len;

	/* Panel enable sequence (sent after DSI video mode is configured) */
	const u8 *enable_seq;
	unsigned int enable_seq_len;

	/* Panel exit sequence */
	const u8 *exit_seq;
	unsigned int exit_seq_len;
};

struct panel_simple {
	struct drm_panel base;

	ktime_t unprepared_time;

	const struct panel_desc *desc;

	struct regulator *supply;
	struct i2c_adapter *ddc;

	struct gpio_desc *enable_gpio;

	const struct drm_edid *drm_edid;

	struct drm_display_mode override_mode;

	enum drm_panel_orientation orientation;

	/* DSI device pointer for DSI panels */
	struct mipi_dsi_device *dsi;

	bool prepared;
	bool enabled;
};

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static unsigned int panel_simple_get_timings_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];
		struct videomode vm;

		videomode_from_timing(dt, &vm);
		mode = drm_mode_create(connector->dev);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u\n",
				dt->hactive.typ, dt->vactive.typ);
			continue;
		}

		drm_display_mode_from_videomode(&vm, mode);

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_timings == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static unsigned int panel_simple_get_display_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay,
				drm_mode_vrefresh(m));
			continue;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static int panel_simple_get_non_edid_modes(struct panel_simple *panel,
					   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	bool has_override = panel->override_mode.type;
	unsigned int num = 0;

	if (!panel->desc)
		return 0;

	if (has_override) {
		mode = drm_mode_duplicate(connector->dev,
					  &panel->override_mode);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			num = 1;
		} else {
			dev_err(panel->base.dev, "failed to add override mode\n");
		}
	}

	/* Only add timings if override was not there or failed to validate */
	if (num == 0 && panel->desc->num_timings)
		num = panel_simple_get_timings_modes(panel, connector);

	/*
	 * Only add fixed modes if timings/override added no mode.
	 *
	 * We should only ever have either the display timings specified
	 * or a fixed mode. Anything else is rather bogus.
	 */
	WARN_ON(panel->desc->num_timings && panel->desc->num_modes);
	if (num == 0)
		num = panel_simple_get_display_modes(panel, connector);

	connector->display_info.bpc = panel->desc->bpc;
	connector->display_info.width_mm = panel->desc->size.width;
	connector->display_info.height_mm = panel->desc->size.height;
	if (panel->desc->bus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &panel->desc->bus_format, 1);
	connector->display_info.bus_flags = panel->desc->bus_flags;

	return num;
}

static void panel_simple_wait(ktime_t start_ktime, unsigned int min_ms)
{
	ktime_t now_ktime, min_ktime;

	if (!min_ms)
		return;

	min_ktime = ktime_add(start_ktime, ms_to_ktime(min_ms));
	now_ktime = ktime_get_boottime();

	if (ktime_before(now_ktime, min_ktime))
		msleep(ktime_to_ms(ktime_sub(min_ktime, now_ktime)) + 1);
}

/* Forward declaration */
static int panel_simple_dsi_send_sequence(struct mipi_dsi_device *dsi,
					  const u8 *seq, unsigned int len);

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->desc->delay.disable)
		msleep(p->desc->delay.disable);

	return 0;
}

static int panel_simple_suspend(struct device *dev)
{
	struct panel_simple *p = dev_get_drvdata(dev);

	gpiod_set_value_cansleep(p->enable_gpio, 0);
	regulator_disable(p->supply);
	p->unprepared_time = ktime_get_boottime();

	drm_edid_free(p->drm_edid);
	p->drm_edid = NULL;

	return 0;
}

static int panel_simple_unprepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	const struct panel_desc_dsi *desc;
	int ret;

	/* Send panel exit sequence if defined */
	desc = container_of(p->desc, struct panel_desc_dsi, desc);
	if (p->dsi && desc->exit_seq && desc->exit_seq_len) {
		ret = panel_simple_dsi_send_sequence(p->dsi, desc->exit_seq,
						     desc->exit_seq_len);
		if (ret < 0)
			dev_err(panel->dev, "failed to send exit sequence: %d\n", ret);
	}	

	pm_runtime_mark_last_busy(panel->dev);
	ret = pm_runtime_put_autosuspend(panel->dev);
	if (ret < 0)
		return ret;

	return 0;
}

static int panel_simple_resume(struct device *dev)
{
	struct panel_simple *p = dev_get_drvdata(dev);
	int err;

	panel_simple_wait(p->unprepared_time, p->desc->delay.unprepare);

	err = regulator_enable(p->supply);
	if (err < 0) {
		dev_err(dev, "failed to enable supply: %d\n", err);
		return err;
	}

	gpiod_set_value_cansleep(p->enable_gpio, 1);

	if (p->desc->delay.prepare)
		msleep(p->desc->delay.prepare);

	return 0;
}

static int panel_simple_prepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	const struct panel_desc_dsi *desc;
	int ret;

	dev_info(panel->dev, "panel_simple_prepare called\n");

	ret = pm_runtime_get_sync(panel->dev);
	if (ret < 0) {
		pm_runtime_put_autosuspend(panel->dev);
		return ret;
	}

	ret = regulator_enable(p->supply);
	if (ret < 0) return ret;
	msleep(20);

	gpiod_set_value_cansleep(p->enable_gpio, 1);
    msleep(10);
	
	gpiod_set_value_cansleep(p->enable_gpio, 0);

	/* Wait for panel power to stabilize */
	if (p->desc->delay.prepare)
		msleep(p->desc->delay.prepare);

	gpiod_set_value_cansleep(p->enable_gpio, 1);
	if (p->desc->delay.init)
		msleep(p->desc->delay.init);

	/* Send panel initialization sequence if defined */
	desc = container_of(p->desc, struct panel_desc_dsi, desc);
	dev_info(panel->dev, "init_seq=%p init_seq_len=%u dsi=%p\n",
		 desc->init_seq, desc->init_seq_len, p->dsi);
	if (p->dsi && desc->init_seq && desc->init_seq_len) {
		ret = panel_simple_dsi_send_sequence(p->dsi, desc->init_seq,
						     desc->init_seq_len);
		if (ret < 0) {
			dev_err(panel->dev, "failed to send init sequence: %d\n", ret);
			pm_runtime_put_autosuspend(panel->dev);
			return ret;
		}
		dev_info(panel->dev, "init sequence sent successfully\n");
	}

	p->prepared = true;

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->enabled)
		return 0;

	if (p->desc->delay.enable)
		msleep(p->desc->delay.enable);

	p->enabled = true;

	return 0;
}

static int panel_simple_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct panel_simple *p = to_panel_simple(panel);
	int num = 0;

	/* probe EDID if a DDC bus is available */
	if (p->ddc) {
		pm_runtime_get_sync(panel->dev);

		if (!p->drm_edid)
			p->drm_edid = drm_edid_read_ddc(connector, p->ddc);

		drm_edid_connector_update(connector, p->drm_edid);

		num += drm_edid_connector_add_modes(connector);

		pm_runtime_mark_last_busy(panel->dev);
		pm_runtime_put_autosuspend(panel->dev);
	}

	/* add hard-coded panel modes */
	num += panel_simple_get_non_edid_modes(p, connector);

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, p->orientation);

	return num;
}

static int panel_simple_get_timings(struct drm_panel *panel,
				    unsigned int num_timings,
				    struct display_timing *timings)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int i;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static enum drm_panel_orientation panel_simple_get_orientation(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	return p->orientation;
}

static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
	.get_orientation = panel_simple_get_orientation,
	.get_timings = panel_simple_get_timings,
};

static struct panel_desc *panel_dpi_probe(struct device *dev)
{
	struct display_timing *timing;
	const struct device_node *np;
	struct panel_desc *desc;
	unsigned int bus_flags;
	struct videomode vm;
	int ret;

	np = dev->of_node;
	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	timing = devm_kzalloc(dev, sizeof(*timing), GFP_KERNEL);
	if (!timing)
		return ERR_PTR(-ENOMEM);

	ret = of_get_display_timing(np, "panel-timing", timing);
	if (ret < 0) {
		dev_err(dev, "%pOF: no panel-timing node found for \"panel-dpi\" binding\n",
			np);
		return ERR_PTR(ret);
	}

	desc->timings = timing;
	desc->num_timings = 1;

	of_property_read_u32(np, "width-mm", &desc->size.width);
	of_property_read_u32(np, "height-mm", &desc->size.height);

	/* Extract bus_flags from display_timing */
	bus_flags = 0;
	vm.flags = timing->flags;
	drm_bus_flags_from_videomode(&vm, &bus_flags);
	desc->bus_flags = bus_flags;

	/* We do not know the connector for the DT node, so guess it */
	desc->connector_type = DRM_MODE_CONNECTOR_DPI;

	return desc;
}

#define PANEL_SIMPLE_BOUNDS_CHECK(to_check, bounds, field) \
	(to_check->field.typ >= bounds->field.min && \
	 to_check->field.typ <= bounds->field.max)
static void panel_simple_parse_panel_timing_node(struct device *dev,
						 struct panel_simple *panel,
						 const struct display_timing *ot)
{
	const struct panel_desc *desc = panel->desc;
	struct videomode vm;
	unsigned int i;

	if (WARN_ON(desc->num_modes)) {
		dev_err(dev, "Reject override mode: panel has a fixed mode\n");
		return;
	}
	if (WARN_ON(!desc->num_timings)) {
		dev_err(dev, "Reject override mode: no timings specified\n");
		return;
	}

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];

		if (!PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hsync_len) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vsync_len))
			continue;

		if (ot->flags != dt->flags)
			continue;

		videomode_from_timing(ot, &vm);
		drm_display_mode_from_videomode(&vm, &panel->override_mode);
		panel->override_mode.type |= DRM_MODE_TYPE_DRIVER |
					     DRM_MODE_TYPE_PREFERRED;
		break;
	}

	if (WARN_ON(!panel->override_mode.type))
		dev_err(dev, "Reject override mode: No display_timing found\n");
}

static int panel_simple_override_nondefault_lvds_datamapping(struct device *dev,
							     struct panel_simple *panel)
{
	int ret, bpc;

	ret = drm_of_lvds_get_data_mapping(dev->of_node);
	if (ret < 0) {
		if (ret == -EINVAL)
			dev_warn(dev, "Ignore invalid data-mapping property\n");

		/*
		 * Ignore non-existing or malformatted property, fallback to
		 * default data-mapping, and return 0.
		 */
		return 0;
	}

	switch (ret) {
	default:
		WARN_ON(1);
		fallthrough;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		fallthrough;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		bpc = 8;
		break;
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		bpc = 6;
	}

	if (panel->desc->bpc != bpc || panel->desc->bus_format != ret) {
		struct panel_desc *override_desc;

		override_desc = devm_kmemdup(dev, panel->desc, sizeof(*panel->desc), GFP_KERNEL);
		if (!override_desc)
			return -ENOMEM;

		override_desc->bus_format = ret;
		override_desc->bpc = bpc;
		panel->desc = override_desc;
	}

	return 0;
}

static const struct panel_desc *panel_simple_get_desc(struct device *dev)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI) &&
	    dev_is_mipi_dsi(dev)) {
		const struct panel_desc_dsi *dsi_desc;

		dsi_desc = of_device_get_match_data(dev);
		if (!dsi_desc)
			return ERR_PTR(-ENODEV);

		return &dsi_desc->desc;
	}

	if (dev_is_platform(dev)) {
		const struct panel_desc *desc;

		desc = of_device_get_match_data(dev);
		if (!desc) {
			/*
			 * panel-dpi probes without a descriptor and
			 * panel_dpi_probe() will initialize one for us
			 * based on the device tree.
			 */
			if (of_device_is_compatible(dev->of_node, "panel-dpi"))
				return panel_dpi_probe(dev);
			else
				return ERR_PTR(-ENODEV);
		}

		return desc;
	}

	return ERR_PTR(-ENODEV);
}

static struct panel_simple *panel_simple_probe(struct device *dev)
{
	const struct panel_desc *desc;
	struct panel_simple *panel;
	struct display_timing dt;
	struct device_node *ddc;
	int connector_type;
	u32 bus_flags;
	int err;

	desc = panel_simple_get_desc(dev);
	if (IS_ERR(desc))
		return ERR_CAST(desc);

	connector_type = desc->connector_type;
	/* Catch common mistakes for panels. */
	switch (connector_type) {
	case 0:
		dev_warn(dev, "Specify missing connector_type\n");
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	case DRM_MODE_CONNECTOR_LVDS:
		WARN_ON(desc->bus_flags &
			~(DRM_BUS_FLAG_DE_LOW |
			  DRM_BUS_FLAG_DE_HIGH |
			  DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			  DRM_BUS_FLAG_DATA_LSB_TO_MSB));
		WARN_ON(desc->bus_format != MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA);
		WARN_ON(desc->bus_format == MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bpc != 6);
		WARN_ON((desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_SPWG ||
			 desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA) &&
			desc->bpc != 8);
		break;
	case DRM_MODE_CONNECTOR_eDP:
		dev_warn(dev, "eDP panels moved to panel-edp\n");
		return ERR_PTR(-EINVAL);
	case DRM_MODE_CONNECTOR_DSI:
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	case DRM_MODE_CONNECTOR_DPI:
		bus_flags = DRM_BUS_FLAG_DE_LOW |
			    DRM_BUS_FLAG_DE_HIGH |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
			    DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			    DRM_BUS_FLAG_DATA_LSB_TO_MSB |
			    DRM_BUS_FLAG_SYNC_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE;
		if (desc->bus_flags & ~bus_flags)
			dev_warn(dev, "Unexpected bus_flags(%d)\n", desc->bus_flags & ~bus_flags);
		if (!(desc->bus_flags & bus_flags))
			dev_warn(dev, "Specify missing bus_flags\n");
		if (desc->bus_format == 0)
			dev_warn(dev, "Specify missing bus_format\n");
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	default:
		dev_warn(dev, "Specify a valid connector_type: %d\n", desc->connector_type);
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	}

	panel = devm_drm_panel_alloc(dev, struct panel_simple, base,
				     &panel_simple_funcs, connector_type);
	if (IS_ERR(panel))
		return ERR_CAST(panel);

	panel->desc = desc;

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return ERR_CAST(panel->supply);

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(panel->enable_gpio))
		return dev_err_cast_probe(dev, panel->enable_gpio,
					  "failed to request GPIO\n");

	err = of_drm_get_panel_orientation(dev->of_node, &panel->orientation);
	if (err) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, err);
		return ERR_PTR(err);
	}

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		panel->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (!panel->ddc)
			return ERR_PTR(-EPROBE_DEFER);
	}

	if (!of_device_is_compatible(dev->of_node, "panel-dpi") &&
	    !of_get_display_timing(dev->of_node, "panel-timing", &dt))
		panel_simple_parse_panel_timing_node(dev, panel, &dt);

	if (desc->connector_type == DRM_MODE_CONNECTOR_LVDS) {
		/* Optional data-mapping property for overriding bus format */
		err = panel_simple_override_nondefault_lvds_datamapping(dev, panel);
		if (err)
			goto free_ddc;
	}

	dev_set_drvdata(dev, panel);

	/*
	 * We use runtime PM for prepare / unprepare since those power the panel
	 * on and off and those can be very slow operations. This is important
	 * to optimize powering the panel on briefly to read the EDID before
	 * fully enabling the panel.
	 */
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	err = drm_panel_of_backlight(&panel->base);
	if (err) {
		dev_err_probe(dev, err, "Could not find backlight\n");
		goto disable_pm_runtime;
	}

	drm_panel_add(&panel->base);

	return panel;

disable_pm_runtime:
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
free_ddc:
	if (panel->ddc)
		put_device(&panel->ddc->dev);

	return ERR_PTR(err);
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	/*
	 * NOTE: the following two calls don't really belong here. It is the
	 * responsibility of a correctly written DRM modeset driver to call
	 * drm_atomic_helper_shutdown() at shutdown time and that should
	 * cause the panel to be disabled / unprepared if needed. For now,
	 * however, we'll keep these calls due to the sheer number of
	 * different DRM modeset drivers used with panel-simple. Once we've
	 * confirmed that all DRM modeset drivers using this panel properly
	 * call drm_atomic_helper_shutdown() we can simply delete the two
	 * calls below.
	 *
	 * TO BE EXPLICIT: THE CALLS BELOW SHOULDN'T BE COPIED TO ANY NEW
	 * PANEL DRIVERS.
	 *
	 * FIXME: If we're still haven't figured out if all DRM modeset
	 * drivers properly call drm_atomic_helper_shutdown() but we _have_
	 * managed to make sure that DRM modeset drivers get their shutdown()
	 * callback before the panel's shutdown() callback (perhaps using
	 * device link), we could add a WARN_ON here to help move forward.
	 */
	if (panel->base.enabled)
		drm_panel_disable(&panel->base);
	if (panel->base.prepared)
		drm_panel_unprepare(&panel->base);
}

static void panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);
	panel_simple_shutdown(dev);

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	if (panel->ddc)
		put_device(&panel->ddc->dev);
}

static const struct drm_display_mode mchp_ac69t88a_mode = {
	.clock = 25000,
	.hdisplay = 800,
	.hsync_start = 800 + 88,
	.hsync_end = 800 + 88 + 5,
	.htotal = 800 + 88 + 5 + 40,
	.vdisplay = 480,
	.vsync_start = 480 + 23,
	.vsync_end = 480 + 23 + 5,
	.vtotal = 480 + 23 + 5 + 1,
};

static const struct panel_desc mchp_ac69t88a = {
	.modes = &mchp_ac69t88a_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.bus_flags = DRM_BUS_FLAG_DE_HIGH,
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
	.connector_type = DRM_MODE_CONNECTOR_LVDS,
};


static const struct of_device_id platform_of_match[] = {
	{
		.compatible = "microchip,ac69t88a",
		.data = &mchp_ac69t88a,
	}, {
		/* Must be the last entry */
		.compatible = "panel-dpi",

		/*
		 * Explicitly NULL, the panel_desc structure will be
		 * allocated by panel_dpi_probe().
		 */
		.data = NULL,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, platform_of_match);

static int panel_simple_platform_probe(struct platform_device *pdev)
{
	struct panel_simple *panel;

	panel = panel_simple_probe(&pdev->dev);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	return 0;
}

static void panel_simple_platform_remove(struct platform_device *pdev)
{
	panel_simple_remove(&pdev->dev);
}

static void panel_simple_platform_shutdown(struct platform_device *pdev)
{
	panel_simple_shutdown(&pdev->dev);
}

static const struct dev_pm_ops panel_simple_pm_ops = {
	SET_RUNTIME_PM_OPS(panel_simple_suspend, panel_simple_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver panel_simple_platform_driver = {
	.driver = {
		.name = "panel-simple",
		.of_match_table = platform_of_match,
		.pm = &panel_simple_pm_ops,
	},
	.probe = panel_simple_platform_probe,
	.remove = panel_simple_platform_remove,
	.shutdown = panel_simple_platform_shutdown,
};

static const struct drm_display_mode osd101t2045_53ts_mode = {
	.clock = 154500,
	.hdisplay = 1920,
	.hsync_start = 1920 + 112,
	.hsync_end = 1920 + 112 + 16,
	.htotal = 1920 + 112 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 16,
	.vsync_end = 1200 + 16 + 2,
	.vtotal = 1200 + 16 + 2 + 16,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc_dsi osd101t2045_53ts = {
	.desc = {
		.modes = &osd101t2045_53ts_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		 MIPI_DSI_MODE_NO_EOT_PACKET,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};


static const struct drm_display_mode sogou_translater_rk3399_mode = {
	.clock = 19422,
	.hdisplay = 480,
	.hsync_start = 480 + 10,
	.hsync_end = 480 + 10 + 10,
	.htotal = 480 + 10 + 10 + 20,
	.vdisplay = 800,
	.vsync_start = 800 + 10,
	.vsync_end = 800 + 10 + 10,
	.vtotal = 800 + 10 + 10 + 10,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};


/*
 * Panel initialization sequence
 * Format: [data_type] [delay_ms] [payload_length] [payload...]
 * data_type: 0x39 = DCS long write, 0x15 = DCS short write, 0x05 = DCS command
 */
static const u8 sogou_translater_init_seq[] = {
	/* 39 00 04 b9 ff 83 79 - Enable extension command */
	0x39, 0x00, 0x04, 0xb9, 0xff, 0x83, 0x79,
	
	/* 39 00 11 b1 44 18 18 31 51 50 d0 d8 58 80 38 38 f8 33 32 22 */
	0x39, 0x00, 0x11, 0xb1, 0x44, 0x18, 0x18, 0x31, 0x51, 0x50,
	0xd0, 0xd8, 0x58, 0x80, 0x38, 0x38, 0xf8, 0x33, 0x32, 0x22,
	
	/* 39 00 0a b2 80 3c 0a 03 70 50 11 42 1d */
	0x39, 0x00, 0x0a, 0xb2, 0x80, 0x3c, 0x0a, 0x03, 0x70, 0x50,
	0x11, 0x42, 0x1d,
	
	/* 39 00 0b b4 02 7c 02 7c 02 7c 22 86 23 86 */
	0x39, 0x00, 0x0b, 0xb4, 0x02, 0x7c, 0x02, 0x7c, 0x02, 0x7c,
	0x22, 0x86, 0x23, 0x86,
	
	/* 39 00 05 c7 00 00 00 c0 */
	0x39, 0x00, 0x05, 0xc7, 0x00, 0x00, 0x00, 0xc0,
	
	/* 15 00 02 cc 02 */
	0x15, 0x00, 0x02, 0xcc, 0x02,
	
	/* 15 00 02 d2 77 */
	0x15, 0x00, 0x02, 0xd2, 0x77,
	
	/* 39 00 26 d3 ... (38 bytes) */
	0x39, 0x00, 0x26, 0xd3, 0x00, 0x07, 0x00, 0x00, 0x00, 0x08,
	0x08, 0x32, 0x10, 0x01, 0x00, 0x01, 0x03, 0x72, 0x03, 0x72,
	0x00, 0x08, 0x00, 0x08, 0x33, 0x33, 0x05, 0x05, 0x37, 0x05,
	0x05, 0x37, 0x08, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x01,
	0x0f,
	
	/* 39 00 23 d5 ... (35 bytes) */
	0x39, 0x00, 0x23, 0xd5, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
	0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x18, 0x18,
	0x21, 0x20, 0x18, 0x18, 0x19, 0x19, 0x23, 0x22, 0x38, 0x38,
	0x78, 0x78, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00,
	
	/* 39 00 21 d6 ... (33 bytes) */
	0x39, 0x00, 0x21, 0xd6, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x18, 0x18,
	0x22, 0x23, 0x19, 0x19, 0x18, 0x18, 0x20, 0x21, 0x38, 0x38,
	0x38, 0x38, 0x18, 0x18, 0x18, 0x18,
	
	/* 39 00 2b e0 ... (43 bytes) - Gamma correction */
	0x39, 0x00, 0x2b, 0xe0, 0x00, 0x01, 0x04, 0x20, 0x24, 0x3f,
	0x11, 0x33, 0x09, 0x0a, 0x0c, 0x17, 0x0f, 0x12, 0x15, 0x13,
	0x14, 0x0a, 0x15, 0x16, 0x18, 0x00, 0x01, 0x04, 0x20, 0x24,
	0x3f, 0x11, 0x33, 0x09, 0x0b, 0x0c, 0x17, 0x03, 0x11, 0x14,
	0x13, 0x14, 0x0a, 0x15, 0x16, 0x18,
	
	/* 39 00 03 b6 5e 5e */
	0x39, 0x00, 0x03, 0xb6, 0x5e, 0x5e,
	
	/* 05 ff 01 11 - Exit sleep mode, delay 255ms */
	0x05, 0xff, 0x01, 0x11,
	
	/* 05 96 01 29 - Display ON, delay 150ms */
	0x05, 0x96, 0x01, 0x29,
};

/*
 * Panel exit sequence
 */
static const u8 sogou_translater_exit_seq[] = {
	/* 05 00 01 28 - Display OFF */
	0x05, 0x00, 0x01, 0x28,
	
	/* 05 78 01 10 - Enter sleep mode, delay 120ms */
	0x05, 0x78, 0x01, 0x10,
};


static const struct panel_desc_dsi sogou_translater_rk3399 = {
	.desc = {
		.modes = &sogou_translater_rk3399_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 52,
			.height = 86,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
		.delay = {
			.prepare = 120,
			.init = 120,
			.enable = 120,
			.disable = 120,
			.unprepare = 120,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 2,
	.init_seq = sogou_translater_init_seq,
	.init_seq_len = sizeof(sogou_translater_init_seq),
	.exit_seq = sogou_translater_exit_seq,
	.exit_seq_len = sizeof(sogou_translater_exit_seq),
};



static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "sogou,translater-rk3399",
		.data = &sogou_translater_rk3399
	}, {
		.compatible = "osddisplays,osd101t2045-53ts",
		.data = &osd101t2045_53ts
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);



static int panel_simple_dsi_send_sequence(struct mipi_dsi_device *dsi,
					  const u8 *seq, unsigned int len)
{
	int ret = 0;
	unsigned int i = 0;

	dev_info(&dsi->dev, "Sending sequence: len=%u\n", len);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	while (i < len) {
		u8 data_type = seq[i];
		u8 delay_ms = seq[i + 1];
		u8 payload_len = seq[i + 2];
		const u8 *payload = &seq[i + 3];

		switch (data_type) {
#if 0
		case 0x15:
		case 0x05:
		case 0x39: /* DCS long write (vendor-specific commands) */
			dev_info(&dsi->dev, "DCS long write: cmd=0x%02x, len=%u, delay=%u\n",payload[0], payload_len, delay_ms);
			ret = mipi_dsi_dcs_write_buffer(dsi, payload,
							payload_len);
			if (ret < 0) {
				dev_err(&dsi->dev, "DCS long write failed: %d\n", ret);
				return ret;
			}
			break;
#else 
		case 0x39: /* DCS long write (vendor-specific commands) */
			dev_info(&dsi->dev, "DCS long write: cmd=0x%02x, len=%u, delay=%u\n",
				 payload[0], payload_len, delay_ms);
			ret = mipi_dsi_dcs_write_buffer(dsi, payload,
							payload_len);
			if (ret < 0) {
				dev_err(&dsi->dev, "DCS long write failed: %d\n", ret);
				return ret;
			}
			break;
		case 0x15: /* DCS short write */
			dev_info(&dsi->dev, "DCS short write: cmd=0x%02x, len=%u, delay=%u\n",
				 payload[0], payload_len, delay_ms);
			if (payload_len > 1)
				ret = mipi_dsi_dcs_write(dsi, payload[0],
							 &payload[1], 1);
			else
				ret = mipi_dsi_dcs_write(dsi, payload[0],
							 NULL, 0);
			if (ret < 0) {
				dev_err(&dsi->dev, "DCS short write failed: %d\n", ret);
				return ret;
			}
			break;
		case 0x05: /* DCS command (no parameters) */
			dev_info(&dsi->dev, "DCS command: cmd=0x%02x, delay=%u\n",
				 payload[0], delay_ms);
			ret = mipi_dsi_dcs_write(dsi, payload[0], NULL, 0);
			if (ret < 0) {
				dev_err(&dsi->dev, "DCS command failed: %d\n", ret);
				return ret;
			}
			break;
#endif 
		default:
			dev_err(&dsi->dev, "unknown data type: %02x\n", data_type);
			return -EINVAL;
		}

		if (delay_ms)
			msleep(delay_ms);

		i += 3 + payload_len;
	}

	dev_info(&dsi->dev, "Sequence sent successfully\n");
	return 0;
}

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct panel_desc_dsi *desc;
	struct panel_simple *panel;
	int err;

	panel = panel_simple_probe(&dsi->dev);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	/* Save DSI device pointer */
	panel->dsi = dsi;

	desc = container_of(panel->desc, struct panel_desc_dsi, desc);
	dsi->mode_flags = desc->flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	dev_info(&dsi->dev, "DSI config: mode_flags=0x%lx, format=%d, lanes=%d\n",
		 dsi->mode_flags, dsi->format, dsi->lanes);

	err = mipi_dsi_attach(dsi);
	if (err) {
		struct panel_simple *panel = mipi_dsi_get_drvdata(dsi);

		drm_panel_remove(&panel->base);
	}

	return err;
}

static void panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-simple-dsi",
		.of_match_table = dsi_of_match,
		.pm = &panel_simple_pm_ops,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

static int __init panel_simple_init(void)
{
	int err;

	err = platform_driver_register(&panel_simple_platform_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_simple_dsi_driver);
		if (err < 0)
			goto err_did_platform_register;
	}

	return 0;

err_did_platform_register:
	platform_driver_unregister(&panel_simple_platform_driver);

	return err;
}
module_init(panel_simple_init);

static void __exit panel_simple_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&panel_simple_dsi_driver);

	platform_driver_unregister(&panel_simple_platform_driver);
}
module_exit(panel_simple_exit);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("DRM Driver for Simple Panels");
MODULE_LICENSE("GPL and additional rights");