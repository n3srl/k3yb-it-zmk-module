/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Minimal SH1107 128x128 monochrome OLED driver, I2C only (Zephyr 3.5 has
 * no SH1107 support).  Same external interface as the k3yb SSD1327 driver:
 * accepts row-major MSB-first 1bpp buffers and converts to the SH1107's
 * page layout (8 vertical pixels per byte, LSB = top row of the page)
 * through a shadow framebuffer, so LVGL and the raw boot logo path work
 * unchanged.
 */

#define DT_DRV_COMPAT k3yb_sh1107

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sh1107, CONFIG_DISPLAY_LOG_LEVEL);

#define SH1107_CTRL_CMD 0x00
#define SH1107_CTRL_DATA 0x40

struct sh1107_config {
    struct i2c_dt_spec bus;
    uint16_t width;
    uint16_t height;
    bool segment_remap;
    bool com_invdir;
    uint8_t display_offset;
    bool internal_dcdc;
};

struct sh1107_data {
    /* row-major MSB-first shadow, width/8 * height bytes */
    uint8_t fb[CONFIG_K3YB_SH1107_FB_SIZE];
};

static int sh1107_cmds(const struct device *dev, const uint8_t *cmds, size_t len) {
    const struct sh1107_config *cfg = dev->config;
    uint8_t buf[8];

    if (len + 1 > sizeof(buf)) {
        return -EINVAL;
    }
    buf[0] = SH1107_CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_write_dt(&cfg->bus, buf, len + 1);
}

static int sh1107_write_page(const struct device *dev, uint8_t page, const uint8_t *data,
                             size_t len) {
    const struct sh1107_config *cfg = dev->config;
    uint8_t cmds[] = {(uint8_t)(0xB0 | page), 0x00, 0x10}; /* page, col low, col high */
    uint8_t buf[129];
    int err;

    err = sh1107_cmds(dev, cmds, sizeof(cmds));
    if (err) {
        return err;
    }
    buf[0] = SH1107_CTRL_DATA;
    memcpy(&buf[1], data, len);
    return i2c_write_dt(&cfg->bus, buf, len + 1);
}

static int sh1107_flush_pages(const struct device *dev, uint8_t page_start, uint8_t page_end) {
    const struct sh1107_config *cfg = dev->config;
    struct sh1107_data *data = dev->data;
    const uint16_t stride = cfg->width / 8;
    uint8_t pagebuf[128];
    int err;

    for (uint8_t p = page_start; p <= page_end; p++) {
        /* convert 8 row-major rows into one vtiled page */
        for (uint16_t x = 0; x < cfg->width; x++) {
            uint8_t b = 0;

            for (uint8_t bit = 0; bit < 8; bit++) {
                uint16_t y = p * 8 + bit;

                if (data->fb[y * stride + x / 8] & (0x80 >> (x % 8))) {
                    b |= 1 << bit; /* LSB = top row of the page */
                }
            }
            pagebuf[x] = b;
        }
        err = sh1107_write_page(dev, p, pagebuf, cfg->width);
        if (err) {
            return err;
        }
    }
    return 0;
}

static int sh1107_blanking_on(const struct device *dev) {
    const uint8_t cmd = 0xAE;

    return sh1107_cmds(dev, &cmd, 1);
}

static int sh1107_blanking_off(const struct device *dev) {
    const uint8_t cmd = 0xAF;

    return sh1107_cmds(dev, &cmd, 1);
}

static int sh1107_write(const struct device *dev, const uint16_t x, const uint16_t y,
                        const struct display_buffer_descriptor *desc, const void *buf) {
    const struct sh1107_config *cfg = dev->config;
    struct sh1107_data *data = dev->data;
    const uint8_t *src = buf;
    const uint16_t stride = cfg->width / 8;

    if (x + desc->width > cfg->width || y + desc->height > cfg->height) {
        return -EINVAL;
    }

    for (uint16_t row = 0; row < desc->height; row++) {
        for (uint16_t col = 0; col < desc->width; col++) {
            const size_t bit = (size_t)row * desc->pitch + col;
            const bool on = (src[bit / 8] >> (7 - (bit % 8))) & 0x1;
            const uint16_t px = x + col;
            uint8_t *cell = &data->fb[(y + row) * stride + px / 8];
            const uint8_t mask = 0x80 >> (px % 8);

            if (on) {
                *cell |= mask;
            } else {
                *cell &= ~mask;
            }
        }
    }

    return sh1107_flush_pages(dev, y / 8, (y + desc->height - 1) / 8);
}

static void sh1107_get_capabilities(const struct device *dev, struct display_capabilities *caps) {
    const struct sh1107_config *cfg = dev->config;

    memset(caps, 0, sizeof(*caps));
    caps->x_resolution = cfg->width;
    caps->y_resolution = cfg->height;
    caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
    caps->current_pixel_format = PIXEL_FORMAT_MONO10;
    caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST;
}

static int sh1107_set_pixel_format(const struct device *dev, const enum display_pixel_format pf) {
    return (pf == PIXEL_FORMAT_MONO10) ? 0 : -ENOTSUP;
}

static int sh1107_set_contrast(const struct device *dev, const uint8_t contrast) {
    const uint8_t cmds[] = {0x81, contrast};

    return sh1107_cmds(dev, cmds, sizeof(cmds));
}

static int sh1107_init(const struct device *dev) {
    const struct sh1107_config *cfg = dev->config;
    struct sh1107_data *data = dev->data;
    int err;

    if (!i2c_is_ready_dt(&cfg->bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    const uint8_t init_seq[][3] = {
        {1, 0xAE, 0},                                          /* display off */
        {2, 0xDC, 0x00},                                       /* start line */
        {2, 0x81, 0x80},                                       /* contrast */
        {1, 0x20, 0},                                          /* page addressing */
        {1, (uint8_t)(cfg->segment_remap ? 0xA1 : 0xA0), 0},   /* segment remap */
        {1, (uint8_t)(cfg->com_invdir ? 0xC8 : 0xC0), 0},      /* COM scan dir */
        {2, 0xA8, (uint8_t)(cfg->height - 1)},                 /* multiplex */
        {2, 0xD3, cfg->display_offset},                        /* display offset */
        {2, 0xD5, 0x51},                                       /* clock */
        {2, 0xD9, 0x22},                                       /* precharge */
        {2, 0xDB, 0x35},                                       /* VCOMH */
        {2, 0xAD, (uint8_t)(cfg->internal_dcdc ? 0x8B : 0x8A)}, /* DC-DC */
        {1, 0xA4, 0},                                          /* RAM content */
        {1, 0xA6, 0},                                          /* normal (not inverted) */
    };

    for (size_t i = 0; i < ARRAY_SIZE(init_seq); i++) {
        err = sh1107_cmds(dev, &init_seq[i][1], init_seq[i][0]);
        if (err) {
            LOG_ERR("init cmd %d failed (%d)", (int)i, err);
            return err;
        }
    }

    memset(data->fb, 0, sizeof(data->fb));
    err = sh1107_flush_pages(dev, 0, cfg->height / 8 - 1);
    if (err) {
        return err;
    }

    return sh1107_blanking_off(dev);
}

static const struct display_driver_api sh1107_api = {
    .blanking_on = sh1107_blanking_on,
    .blanking_off = sh1107_blanking_off,
    .write = sh1107_write,
    .get_capabilities = sh1107_get_capabilities,
    .set_pixel_format = sh1107_set_pixel_format,
    .set_contrast = sh1107_set_contrast,
};

#define SH1107_DEFINE(n)                                                                           \
    static const struct sh1107_config sh1107_config_##n = {                                        \
        .bus = I2C_DT_SPEC_INST_GET(n),                                                            \
        .width = DT_INST_PROP(n, width),                                                           \
        .height = DT_INST_PROP(n, height),                                                         \
        .segment_remap = DT_INST_PROP(n, segment_remap),                                           \
        .com_invdir = DT_INST_PROP(n, com_invdir),                                                 \
        .display_offset = DT_INST_PROP(n, display_offset),                                         \
        .internal_dcdc = DT_INST_PROP(n, internal_dcdc),                                           \
    };                                                                                             \
    static struct sh1107_data sh1107_data_##n;                                                     \
    DEVICE_DT_INST_DEFINE(n, sh1107_init, NULL, &sh1107_data_##n, &sh1107_config_##n, POST_KERNEL, \
                          CONFIG_DISPLAY_INIT_PRIORITY, &sh1107_api);

DT_INST_FOREACH_STATUS_OKAY(SH1107_DEFINE)
