#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct ams801hk01 {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *bl_dev;
	struct mipi_dsi_device *dsi[2];
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
};

static inline struct ams801hk01 *to_ams801hk01(struct drm_panel *panel)
{
	return container_of(panel, struct ams801hk01, panel);
}

static int ams801hk01_power_on(struct ams801hk01 *ctx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	usleep_range(10000, 11000);

	/* Assert enable_gpio to turn on external display power IC */
	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(5000, 6000);

	/*
	 * DTS defines reset-gpios as GPIO_ACTIVE_LOW.
	 * Logical 1 = Physical LOW (Reset state)
	 * Logical 0 = Physical HIGH (Active state)
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;
}

static int ams801hk01_power_off(struct ams801hk01 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	return regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int ams801hk01_panel_init_sequence(struct ams801hk01 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi[0] };

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 5);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x63);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_msleep(&dsi_ctx, 50);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8, 0x19);
	mipi_dsi_msleep(&dsi_ctx, 40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x10, 0x07, 0xff, 0x00, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	/* TE Vsync ON */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	/* TSP TE ON */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x11, 0x01, 0x02, 0x16, 0x02, 0x16);
	/* Pentile setting */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x00, 0x0f, 0xd8, 0xd8);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x20);
	/* POC setting */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);

	/* ERR_FG setting */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x44);

	/* Column address set (0 to 1535) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x05, 0xff);
	/* Row address set (0 to 2047) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x07, 0xff);

	/* Brightness condition set (Gamma 2.2 Table) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xca,
				     0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
				     0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
				     0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
				     0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
				     0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
				     0x80, 0x00, 0x00, 0x00, 0x00, 0x00);

	/* A0R 0% */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x0a, 0x00);
	/* ELVSS Setting: CAPS ON : acl off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x9c, 0x0d);
	/* VINT set */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0xab, 0x21);
	/* CAPS ON: 16 frame avg. at acl off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x40);
	/* acl off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x00);

	/* Gamma, LTPS(AID) update */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	return dsi_ctx.accum_err;
}

static int ams801hk01_set_brightness(struct backlight_device *bl_dev)
{
	struct ams801hk01 *ctx = bl_get_data(bl_dev);
	u16 brightness = bl_dev->props.brightness;

	if (bl_dev->props.power != BACKLIGHT_POWER_ON ||
		bl_dev->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	return mipi_dsi_dcs_set_display_brightness(ctx->dsi[0], brightness);
}

static const struct backlight_ops ams801hk01_bl_ops = {
	.update_status = ams801hk01_set_brightness,
};

static int ams801hk01_prepare(struct drm_panel *panel)
{
	struct ams801hk01 *ctx = to_ams801hk01(panel);
	int ret;

	ret = ams801hk01_power_on(ctx);
	if (ret < 0)
		return ret;

	ret = ams801hk01_panel_init_sequence(ctx);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to initialize panel: %d\n", ret);
		ams801hk01_power_off(ctx);
		return ret;
	}

	return 0;
}

static int ams801hk01_unprepare(struct drm_panel *panel)
{
	struct ams801hk01 *ctx = to_ams801hk01(panel);

	ams801hk01_power_off(ctx);
	return 0;
}

static int ams801hk01_enable(struct drm_panel *panel)
{
	struct ams801hk01 *ctx = to_ams801hk01(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi[0]);
	if (ret < 0)
		return ret;

	msleep(50);

	ctx->bl_dev->props.power = BACKLIGHT_POWER_ON;
	ams801hk01_set_brightness(ctx->bl_dev);

	return 0;
}

static int ams801hk01_disable(struct drm_panel *panel)
{
	struct ams801hk01 *ctx = to_ams801hk01(panel);

	ctx->bl_dev->props.power = BACKLIGHT_POWER_REDUCED;
	ams801hk01_set_brightness(ctx->bl_dev);

	mipi_dsi_dcs_set_display_off(ctx->dsi[0]);
	msleep(120);
	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi[0]);
	msleep(120);

	return 0;
}

static const struct drm_display_mode ams801hk01_mode = {
	.clock = 273708,
	.hdisplay = 1536,
	.hsync_start = 1536 + 424,
	.hsync_end = 1536 + 424 + 32,
	.htotal = 1536 + 424 + 32 + 216,
	.vdisplay = 2048,
	.vsync_start = 2048 + 11,
	.vsync_end = 2048 + 11 + 2,
	.vtotal = 2048 + 11 + 2 + 5,
	.width_mm = 122,
	.height_mm = 163,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int ams801hk01_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ams801hk01_mode);
}

static const struct drm_panel_funcs ams801hk01_drm_funcs = {
	.disable = ams801hk01_disable,
	.unprepare = ams801hk01_unprepare,
	.prepare = ams801hk01_prepare,
	.enable = ams801hk01_enable,
	.get_modes = ams801hk01_get_modes,
};

static int ams801hk01_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ams801hk01 *ctx;
	struct mipi_dsi_device *dsi1_device;
	struct device_node *dsi1_node;
	struct mipi_dsi_host *dsi1_host;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 200,
		.max_brightness = 255,
	};
	int ret, i;

	const struct mipi_dsi_device_info info = {
		.type = "ams801hk01-slave",
		.channel = 0,
		.node = NULL,
	};

	ctx = devm_drm_panel_alloc(dev, struct ams801hk01, panel,
				   &ams801hk01_drm_funcs,
			    DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* Setup DSI1 (Slave) via OF graph */
	dsi1_node = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
	if (!dsi1_node) {
		dev_err(dev, "failed to get remote node for dsi1_device\n");
		return -ENODEV;
	}

	dsi1_host = of_find_mipi_dsi_host_by_node(dsi1_node);
	of_node_put(dsi1_node);
	if (!dsi1_host)
		return dev_err_probe(dev, -EPROBE_DEFER, "failed to find dsi1 host\n");

	dsi1_device = mipi_dsi_device_register_full(dsi1_host, &info);
	if (IS_ERR(dsi1_device))
		return dev_err_probe(dev, PTR_ERR(dsi1_device), "failed to create dsi1 device\n");

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;
	ctx->dsi[0] = dsi;
	ctx->dsi[1] = dsi1_device;

	ctx->supplies[0].supply = "vdd3";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		goto err_dsi1_unreg;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		goto err_dsi1_unreg;
	}

	ctx->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio)) {
		ret = PTR_ERR(ctx->enable_gpio);
		goto err_dsi1_unreg;
	}

	ctx->bl_dev = devm_backlight_device_register(dev, "ams801hk01", dev, ctx,
						     &ams801hk01_bl_ops, &props);
	if (IS_ERR(ctx->bl_dev)) {
		ret = PTR_ERR(ctx->bl_dev);
		goto err_dsi1_unreg;
	}

	ctx->panel.prepare_prev_first = true;
	drm_panel_add(&ctx->panel);

	/* Attach to both hosts in Command Mode */
	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		ctx->dsi[i]->lanes = 4;
		ctx->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		ctx->dsi[i]->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;

		ret = mipi_dsi_attach(ctx->dsi[i]);
		if (ret < 0) {
			dev_err(dev, "dsi attach failed i = %d\n", i);
			goto err_attach;
		}
	}

	return 0;

	err_attach:
	if (i == 1)
		mipi_dsi_detach(ctx->dsi[0]);
	drm_panel_remove(&ctx->panel);
	err_dsi1_unreg:
	mipi_dsi_device_unregister(dsi1_device);
	return ret;
}

static void ams801hk01_remove(struct mipi_dsi_device *dsi)
{
	struct ams801hk01 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(ctx->dsi[1]);
	mipi_dsi_device_unregister(ctx->dsi[1]);
	mipi_dsi_detach(ctx->dsi[0]);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ams801hk01_of_match[] = {
	{ .compatible = "samsung,ams801hk01" },
	{ }
};
MODULE_DEVICE_TABLE(of, ams801hk01_of_match);

static struct mipi_dsi_driver ams801hk01_driver = {
	.probe = ams801hk01_probe,
	.remove = ams801hk01_remove,
	.driver = {
		.name = "panel-samsung-ams801hk01",
		.of_match_table = ams801hk01_of_match,
	},
};
module_mipi_dsi_driver(ams801hk01_driver);

MODULE_DESCRIPTION("Samsung AMS801HK01 Dual-DSI AMOLED Panel Driver");
MODULE_LICENSE("GPL v2");
