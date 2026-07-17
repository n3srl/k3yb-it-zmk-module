/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Minimal SSD1327 (128x128 4-bit grayscale OLED) display driver, I2C only,
 * exposed to Zephyr/LVGL as a 1bpp monochrome display.  Zephyr 3.5 (ZMK
 * v0.3) has no in-tree SSD1327 driver, hence this module driver.
 *
 * A full shadow framebuffer (width/2 * height bytes, 8 KiB at 128x128) is
 * kept in RAM so LVGL flush regions with odd X coordinates can be merged
 * into the 2-pixels-per-byte GDDRAM layout.
 */

#define DT_DRV_COMPAT k3yb_ssd1327

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ssd1327, CONFIG_DISPLAY_LOG_LEVEL);

#define SSD1327_CTRL_CMD 0x00
#define SSD1327_CTRL_DATA 0x40

struct ssd1327_config {
    struct i2c_dt_spec bus;
    uint16_t width;
    uint16_t height;
    uint8_t remap;
};

struct ssd1327_data {
    /* 2 pixels per byte, high nibble = even (left) pixel */
    uint8_t fb[CONFIG_K3YB_SSD1327_FB_SIZE];
};

static int ssd1327_cmds(const struct device *dev, const uint8_t *cmds, size_t len) {
    const struct ssd1327_config *cfg = dev->config;
    uint8_t buf[8];

    if (len + 1 > sizeof(buf)) {
        return -EINVAL;
    }
    buf[0] = SSD1327_CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_write_dt(&cfg->bus, buf, len + 1);
}

static int ssd1327_set_window(const struct device *dev, uint8_t col_start, uint8_t col_end,
                              uint8_t row_start, uint8_t row_end) {
    const uint8_t cmds[] = {0x15, col_start, col_end, 0x75, row_start, row_end};

    return ssd1327_cmds(dev, cmds, sizeof(cmds));
}

static int ssd1327_write_data(const struct device *dev, const uint8_t *data, size_t len) {
    const struct ssd1327_config *cfg = dev->config;
    /* one row of 128px = 64 bytes; chunk buffer holds control byte + row */
    uint8_t buf[65];
    int err;

    while (len > 0) {
        size_t chunk = MIN(len, sizeof(buf) - 1);

        buf[0] = SSD1327_CTRL_DATA;
        memcpy(&buf[1], data, chunk);
        err = i2c_write_dt(&cfg->bus, buf, chunk + 1);
        if (err) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return 0;
}

static int ssd1327_blanking_on(const struct device *dev) {
    const uint8_t cmd = 0xAE; /* display off */

    return ssd1327_cmds(dev, &cmd, 1);
}

static int ssd1327_blanking_off(const struct device *dev) {
    const uint8_t cmd = 0xAF; /* display on */

    return ssd1327_cmds(dev, &cmd, 1);
}

static int ssd1327_flush_rows(const struct device *dev, uint16_t y0, uint16_t y1) {
    const struct ssd1327_config *cfg = dev->config;
    struct ssd1327_data *data = dev->data;
    const uint16_t stride = cfg->width / 2;
    int err;

    err = ssd1327_set_window(dev, 0, stride - 1, y0, y1);
    if (err) {
        return err;
    }
    return ssd1327_write_data(dev, &data->fb[y0 * stride], (y1 - y0 + 1) * stride);
}

static int ssd1327_write(const struct device *dev, const uint16_t x, const uint16_t y,
                         const struct display_buffer_descriptor *desc, const void *buf) {
    const struct ssd1327_config *cfg = dev->config;
    struct ssd1327_data *data = dev->data;
    const uint8_t *src = buf;
    const uint16_t stride = cfg->width / 2;

    if (x + desc->width > cfg->width || y + desc->height > cfg->height) {
        return -EINVAL;
    }

    /* merge 1bpp MSB-first rows into the 4bpp shadow framebuffer */
    for (uint16_t row = 0; row < desc->height; row++) {
        for (uint16_t col = 0; col < desc->width; col++) {
            const size_t bit = (size_t)row * desc->pitch + col;
            const bool on = (src[bit / 8] >> (7 - (bit % 8))) & 0x1;
            const uint16_t px = x + col;
            uint8_t *cell = &data->fb[(y + row) * stride + px / 2];

            if ((px & 1) == 0) {
                *cell = (*cell & 0x0F) | (on ? 0xF0 : 0x00);
            } else {
                *cell = (*cell & 0xF0) | (on ? 0x0F : 0x00);
            }
        }
    }

    return ssd1327_flush_rows(dev, y, y + desc->height - 1);
}

static void ssd1327_get_capabilities(const struct device *dev, struct display_capabilities *caps) {
    const struct ssd1327_config *cfg = dev->config;

    memset(caps, 0, sizeof(*caps));
    caps->x_resolution = cfg->width;
    caps->y_resolution = cfg->height;
    caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
    caps->current_pixel_format = PIXEL_FORMAT_MONO10;
    caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST;
}

static int ssd1327_set_pixel_format(const struct device *dev, const enum display_pixel_format pf) {
    if (pf == PIXEL_FORMAT_MONO10) {
        return 0;
    }
    return -ENOTSUP;
}

static int ssd1327_set_contrast(const struct device *dev, const uint8_t contrast) {
    const uint8_t cmds[] = {0x81, contrast};

    return ssd1327_cmds(dev, cmds, sizeof(cmds));
}

static int ssd1327_init(const struct device *dev) {
    const struct ssd1327_config *cfg = dev->config;
    struct ssd1327_data *data = dev->data;
    int err;

    if (!i2c_is_ready_dt(&cfg->bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    const uint8_t init_seq[][3] = {
        {2, 0xFD, 0x12},              /* unlock */
        {1, 0xAE, 0},                 /* display off */
        {2, 0xA8, cfg->height - 1},   /* mux ratio */
        {2, 0xA1, 0x00},              /* start line */
        {2, 0xA2, 0x00},              /* display offset */
        {2, 0xA0, cfg->remap},        /* segment remap / orientation */
        {2, 0xAB, 0x01},              /* internal VDD regulator */
        {2, 0x81, 0x80},              /* contrast */
        {2, 0xB1, 0xF1},              /* phase length */
        {2, 0xB3, 0x00},              /* clock divider */
        {2, 0xBC, 0x08},              /* precharge voltage */
        {2, 0xBE, 0x07},              /* VCOMH */
        {2, 0xB6, 0x0F},              /* second precharge */
        {1, 0xA4, 0},                 /* normal display mode */
    };

    for (size_t i = 0; i < ARRAY_SIZE(init_seq); i++) {
        err = ssd1327_cmds(dev, &init_seq[i][1], init_seq[i][0]);
        if (err) {
            LOG_ERR("init cmd %d failed (%d)", (int)i, err);
            return err;
        }
    }

    /* clear GDDRAM (and shadow) before turning on */
    memset(data->fb, 0, sizeof(data->fb));

#if IS_ENABLED(CONFIG_K3YB_SSD1327_TEST_PATTERN)
    /* Geometry fingerprint to calibrate the panel mapping from a photo:
     *  - 2px line along the full TOP edge (y = 0,1)
     *  - 2px line along the full LEFT edge (x = 0,1)
     *  - solid 24x24 block in the TOP-LEFT corner
     *  - solid 8x8 block in the TOP-RIGHT corner
     */
    {
        const uint16_t stride = cfg->width / 2;

        for (uint16_t x = 0; x < cfg->width; x++) { /* top edge */
            for (uint16_t y = 0; y < 2; y++) {
                data->fb[y * stride + x / 2] |= (x & 1) ? 0x0F : 0xF0;
            }
        }
        for (uint16_t y = 0; y < cfg->height; y++) { /* left edge */
            data->fb[y * stride + 0] |= 0xF0;
            data->fb[y * stride + 0] |= 0x0F;
        }
        for (uint16_t y = 0; y < 24; y++) { /* 24x24 top-left */
            for (uint16_t x = 0; x < 24; x++) {
                data->fb[y * stride + x / 2] |= (x & 1) ? 0x0F : 0xF0;
            }
        }
        for (uint16_t y = 0; y < 8; y++) { /* 8x8 top-right */
            for (uint16_t x = cfg->width - 8; x < cfg->width; x++) {
                data->fb[y * stride + x / 2] |= (x & 1) ? 0x0F : 0xF0;
            }
        }
    }
#endif

    err = ssd1327_flush_rows(dev, 0, cfg->height - 1);
    if (err) {
        return err;
    }

    return ssd1327_blanking_off(dev);
}

static const struct display_driver_api ssd1327_api = {
    .blanking_on = ssd1327_blanking_on,
    .blanking_off = ssd1327_blanking_off,
    .write = ssd1327_write,
    .get_capabilities = ssd1327_get_capabilities,
    .set_pixel_format = ssd1327_set_pixel_format,
    .set_contrast = ssd1327_set_contrast,
};

#define SSD1327_DEFINE(n)                                                                          \
    static const struct ssd1327_config ssd1327_config_##n = {                                      \
        .bus = I2C_DT_SPEC_INST_GET(n),                                                            \
        .width = DT_INST_PROP(n, width),                                                           \
        .height = DT_INST_PROP(n, height),                                                         \
        .remap = DT_INST_PROP(n, remap_value),                                                     \
    };                                                                                             \
    static struct ssd1327_data ssd1327_data_##n;                                                   \
    DEVICE_DT_INST_DEFINE(n, ssd1327_init, NULL, &ssd1327_data_##n, &ssd1327_config_##n,           \
                          POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &ssd1327_api);

DT_INST_FOREACH_STATUS_OKAY(SSD1327_DEFINE)
