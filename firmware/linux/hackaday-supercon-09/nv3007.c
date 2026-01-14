// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for NV3007 TFT LCD panels (Hackaday Supercon 2025 Badge)
 *
 * Based on ili9341.c by David Lechner
 * Vibe coded by Claude 4.5 Opus High
 *
 * The NV3007 is a 2.79" TFT LCD with 142x428 resolution.
 * It uses a MIPI DBI Type C (SPI) interface with a D/C GPIO.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <video/mipi_display.h>

/* NV3007 specific commands (beyond standard MIPI DCS) */
#define NV3007_FRMCTR1		0xb1	/* Frame Rate Control */
#define NV3007_DISCTRL		0xb6	/* Display Function Control */
#define NV3007_PWCTRL1		0xc0	/* Power Control 1 */
#define NV3007_PWCTRL2		0xc1	/* Power Control 2 */
#define NV3007_VMCTRL1		0xc5	/* VCOM Control 1 */
#define NV3007_VMCTRL2		0xc7	/* VCOM Control 2 */
#define NV3007_PGAMCTRL		0xe0	/* Positive Gamma Control */
#define NV3007_NGAMCTRL		0xe1	/* Negative Gamma Control */

/* MADCTL bits */
#define NV3007_MADCTL_MY	BIT(7)	/* Row Address Order */
#define NV3007_MADCTL_MX	BIT(6)	/* Column Address Order */
#define NV3007_MADCTL_MV	BIT(5)	/* Row/Column Exchange */
#define NV3007_MADCTL_ML	BIT(4)	/* Vertical Refresh Order */
#define NV3007_MADCTL_BGR	BIT(3)	/* RGB/BGR Order */
#define NV3007_MADCTL_MH	BIT(2)	/* Horizontal Refresh Order */

/* Display dimensions */
#define NV3007_WIDTH		142
#define NV3007_HEIGHT		428

/**
 * nv3007_enable - Enable and initialize the NV3007 display
 * @pipe: DRM simple display pipe
 * @crtc_state: CRTC state
 * @plane_state: Plane state
 *
 * This function performs the display initialization sequence.
 * The sequence is derived from the MicroPython nv3007 driver.
 */
static void nv3007_enable(struct drm_simple_display_pipe *pipe,
			  struct drm_crtc_state *crtc_state,
			  struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	dev_dbg(pipe->crtc.dev->dev, "Enabling NV3007 display\n");

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1)
		goto out_enable;

	/* Turn off display during init */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);

	/* Exit sleep mode */
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(120);

	/* Set pixel format to 16-bit RGB565 */
	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, MIPI_DCS_PIXEL_FMT_16BIT);

	/* Power Control settings */
	mipi_dbi_command(dbi, NV3007_PWCTRL1, 0x23);
	mipi_dbi_command(dbi, NV3007_PWCTRL2, 0x10);

	/* VCOM Control */
	mipi_dbi_command(dbi, NV3007_VMCTRL1, 0x3e, 0x28);
	mipi_dbi_command(dbi, NV3007_VMCTRL2, 0x86);

	/* Frame Rate Control - ~60Hz */
	mipi_dbi_command(dbi, NV3007_FRMCTR1, 0x00, 0x1b);

	/* Display Function Control */
	mipi_dbi_command(dbi, NV3007_DISCTRL, 0x08, 0x82, 0x27, 0x00);

	/* Gamma settings (positive) */
	mipi_dbi_command(dbi, NV3007_PGAMCTRL,
			 0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
			 0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00);

	/* Gamma settings (negative) */
	mipi_dbi_command(dbi, NV3007_NGAMCTRL,
			 0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
			 0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f);

	/* Turn on display */
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(20);

out_enable:
	/*
	 * Configure memory access control (MADCTL) based on rotation.
	 * The NV3007 panel is physically 142x428, but the badge uses it
	 * in landscape orientation (428x142) with 270° rotation.
	 */
	switch (dbidev->rotation) {
	default:
	case 0:
		addr_mode = 0;
		break;
	case 90:
		addr_mode = NV3007_MADCTL_MV | NV3007_MADCTL_MX;
		break;
	case 180:
		addr_mode = NV3007_MADCTL_MX | NV3007_MADCTL_MY;
		break;
	case 270:
		/* Default for badge - landscape mode */
		addr_mode = NV3007_MADCTL_MV | NV3007_MADCTL_MY;
		break;
	}
	/* Use BGR color order (common for these panels) */
	addr_mode |= NV3007_MADCTL_BGR;

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	/* Flush the framebuffer to the display */
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);

out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs nv3007_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(nv3007_enable),
};

/*
 * Display mode for NV3007
 * Native: 142x428, but typically used rotated as 428x142
 * Using native orientation here; rotation handles the rest.
 */
static const struct drm_display_mode nv3007_mode = {
	DRM_SIMPLE_MODE(NV3007_WIDTH, NV3007_HEIGHT, 35, 107),
};

DEFINE_DRM_GEM_DMA_FOPS(nv3007_fops);

static const struct drm_driver nv3007_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &nv3007_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "nv3007",
	.desc			= "NV3007 TFT LCD",
	.date			= "20241101",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id nv3007_of_match[] = {
	{ .compatible = "nv3007-spi" },
	{ .compatible = "hackaday,nv3007" },
	{ }
};
MODULE_DEVICE_TABLE(of, nv3007_of_match);

static const struct spi_device_id nv3007_id[] = {
	{ "nv3007", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, nv3007_id);

/**
 * nv3007_probe - Probe function for NV3007 SPI driver
 * @spi: SPI device
 *
 * This function initializes the NV3007 display driver.
 * It demonstrates:
 * - Getting GPIO descriptors from device tree (dc-gpios, reset-gpios)
 * - Initializing the MIPI DBI SPI interface
 * - Registering the DRM device
 */
static int nv3007_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 270;	/* Default rotation for badge */
	int ret;

	dev_info(dev, "Probing NV3007 display\n");

	/* Allocate the DRM device structure */
	dbidev = devm_drm_dev_alloc(dev, &nv3007_driver,
				    struct mipi_dbi_dev, drm);
	if (IS_ERR(dbidev))
		return PTR_ERR(dbidev);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	/*
	 * Get the reset GPIO from device tree.
	 * In DTS: reset-gpios = <&gpio0 40 GPIO_ACTIVE_LOW>;
	 * The GPIO is optional; display may still work without it.
	 */
	dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset)) {
		return dev_err_probe(dev, PTR_ERR(dbi->reset),
				     "Failed to get GPIO 'reset'\n");
	}
	if (dbi->reset)
		dev_info(dev, "Reset GPIO acquired\n");

	/*
	 * Get the D/C (Data/Command) GPIO from device tree.
	 * In DTS: dc-gpios = <&gpio0 39 GPIO_ACTIVE_HIGH>;
	 * This GPIO distinguishes between command (low) and data (high).
	 * Required for SPI interface.
	 */
	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		return dev_err_probe(dev, PTR_ERR(dc),
				     "Failed to get GPIO 'dc'\n");
	}
	if (dc)
		dev_info(dev, "D/C GPIO acquired\n");

	/*
	 * Get optional backlight from device tree.
	 * Can be defined as a separate backlight node or PWM.
	 */
	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	/* Read rotation property from device tree, default to 270 for badge */
	device_property_read_u32(dev, "rotation", &rotation);
	dev_info(dev, "Using rotation: %u degrees\n", rotation);

	/*
	 * Initialize the MIPI DBI SPI interface.
	 * This sets up the SPI transfer functions and D/C GPIO handling.
	 */
	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret) {
		dev_err(dev, "Failed to initialize MIPI DBI SPI: %d\n", ret);
		return ret;
	}

	/*
	 * Initialize the MIPI DBI device with our pipe functions and mode.
	 * This creates the DRM mode configuration.
	 */
	ret = mipi_dbi_dev_init(dbidev, &nv3007_pipe_funcs, &nv3007_mode, rotation);
	if (ret) {
		dev_err(dev, "Failed to initialize MIPI DBI device: %d\n", ret);
		return ret;
	}

	/* Reset the mode configuration to defaults */
	drm_mode_config_reset(drm);

	/* Register the DRM device */
	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(dev, "Failed to register DRM device: %d\n", ret);
		return ret;
	}

	/* Store DRM device in SPI driver data for later access */
	spi_set_drvdata(spi, drm);

	/* Set up fbdev emulation for legacy applications */
	drm_fbdev_dma_setup(drm, 0);

	dev_info(dev, "NV3007 display initialized (%dx%d)\n",
		 NV3007_WIDTH, NV3007_HEIGHT);

	return 0;
}

static void nv3007_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void nv3007_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver nv3007_spi_driver = {
	.driver = {
		.name = "nv3007",
		.of_match_table = nv3007_of_match,
	},
	.id_table = nv3007_id,
	.probe = nv3007_probe,
	.remove = nv3007_remove,
	.shutdown = nv3007_shutdown,
};
module_spi_driver(nv3007_spi_driver);

MODULE_DESCRIPTION("NV3007 TFT LCD DRM driver");
MODULE_AUTHOR("Hackaday Supercon Badge Team");
MODULE_LICENSE("GPL");

