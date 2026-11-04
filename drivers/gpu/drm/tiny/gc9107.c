// SPDX-License-Identifier: GPL-2.0+

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>

#include <video/mipi_display.h>

#define GC9107_MADCTL_BGR	BIT(3)
#define GC9107_MADCTL_MV	BIT(5)
#define GC9107_MADCTL_MX	BIT(6)
#define GC9107_MADCTL_MY	BIT(7)

static void gc9107_enable(struct drm_simple_display_pipe *pipe,
			  struct drm_crtc_state *crtc_state,
			  struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1)
		goto out_enable;

	mipi_dbi_command(dbi, 0xFE);
	mipi_dbi_command(dbi, 0xEF);

	mipi_dbi_command(dbi, 0xB0, 0xC0);
	mipi_dbi_command(dbi, 0xB2, 0x2F);
	mipi_dbi_command(dbi, 0xB3, 0x03);
	mipi_dbi_command(dbi, 0xB6, 0x19);
	mipi_dbi_command(dbi, 0xB7, 0x01);
	mipi_dbi_command(dbi, 0xAC, 0xC8);
	mipi_dbi_command(dbi, 0xAB, 0x07);
	mipi_dbi_command(dbi, 0xB4, 0x04);
	mipi_dbi_command(dbi, 0xA8, 0x0C);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	mipi_dbi_command(dbi, 0xB8, 0x08);
	mipi_dbi_command(dbi, 0xE8, 0x20);
	mipi_dbi_command(dbi, 0xE9, 0x44);
	mipi_dbi_command(dbi, 0xEA, 0x44);
	mipi_dbi_command(dbi, 0xEB, 0xB0);
	mipi_dbi_command(dbi, 0xC6, 0x3D);
	mipi_dbi_command(dbi, 0xC7, 0x10);

	mipi_dbi_command(dbi, 0xF0, 0x0F, 0x20, 0x25, 0x35, 0x26, 0x3F, 0x30,
			      0x06, 0x02, 0x60, 0x30, 0x09, 0x13, 0x1A);
	mipi_dbi_command(dbi, 0xF1, 0x0F, 0x28, 0x7F, 0x39, 0xA5, 0x0F, 0x03,
			      0x06, 0x00, 0x05, 0x01, 0x0F, 0x0F, 0x1F);

	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(20);

out_enable:
	switch (dbidev->rotation) {
	default:
		addr_mode = 0;
		break;
	case 90:
		addr_mode = GC9107_MADCTL_MV | GC9107_MADCTL_MY;
		break;
	case 180:
		addr_mode = GC9107_MADCTL_MX | GC9107_MADCTL_MY;
		break;
	case 270:
		addr_mode = GC9107_MADCTL_MV | GC9107_MADCTL_MX;
		break;
	}

	addr_mode |= GC9107_MADCTL_BGR;

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs gc9107_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(gc9107_enable),
};

static const struct drm_display_mode gc9107_mode = {
	DRM_SIMPLE_MODE(128, 128, 26, 26),
};

DEFINE_DRM_GEM_DMA_FOPS(gc9107_fops);

static struct drm_driver gc9107_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &gc9107_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "gc9107",
	.desc			= "GalaxyCore GC9107",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id gc9107_of_match[] = {
	{ .compatible = "galaxycore,gc9107" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc9107_of_match);

static const struct spi_device_id gc9107_id[] = {
	{ "gc9107", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, gc9107_id);

static int gc9107_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	dbidev = devm_drm_dev_alloc(dev, &gc9107_driver,
				    struct mipi_dbi_dev, drm);
	if (IS_ERR(dbidev))
		return PTR_ERR(dbidev);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	spi_set_drvdata(spi, drm);

	dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get gpio 'reset'\n");

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get gpio 'dc'\n");

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_dev_init(dbidev, &gc9107_pipe_funcs, &gc9107_mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void gc9107_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void gc9107_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver gc9107_spi_driver = {
	.driver = {
		.name = "gc9107",
		.of_match_table = gc9107_of_match,
	},
	.id_table = gc9107_id,
	.probe = gc9107_probe,
	.remove = gc9107_remove,
	.shutdown = gc9107_shutdown,
};
module_spi_driver(gc9107_spi_driver);

MODULE_DESCRIPTION("GalaxyCore GC9107 DRM driver");
MODULE_AUTHOR("Ryan Gosling");
MODULE_LICENSE("GPL");
