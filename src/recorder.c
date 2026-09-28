/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Offline keystroke recorder: type notes on battery with no host (and no
 * monitor), replay them later into an editor on the PC.  Controlled from
 * the led_control layer (&ledctl K3YB_LED_REC_*); API in
 * include/k3yb/recorder.h.
 *
 * Capture: a zmk_keycode_state_changed listener stores key PRESSES only
 * (no timing), 2 bytes each - the modifiers the host sees at that press
 * plus the HID keyboard usage id.  Enter, Space, Backspace, Tab... are
 * plain usages.  Modifier keys themselves are not stored; their state
 * rides on the next press.  One exception: when a modifier is released a
 * marker event (usage 0x00) records the new modifier state.  Without it
 * two Alt+numpad accent macros in a row would replay as one long Alt
 * sequence (Alt released only between them), breaking "à" and friends.
 *
 * The modifier snapshot is (explicit mods & report mods) | implicit mods
 * of the event.  That is order-independent: ZMK does not guarantee
 * whether this listener runs before or after hid_listener, and the
 * intersection with the live report drops mods masked by a mod-morph
 * (the accent macros mask Shift) either way.
 *
 * Playback re-raises the raw keycode events through the normal ZMK HID
 * path, holding modifiers across consecutive presses and changing them
 * only when the recorded state changes, with a fixed inter-step delay.
 * Raw usages: the host must use the same OS layout as at recording time
 * (US + NumLock on for the Alt+numpad accents), no layout conversion.
 *
 * Flash format (recorder_partition, internal flash, erased = 0xFF):
 *   offset 0   u32 REC_MAGIC
 *   offset 4+  events, 2 bytes each, two per 32-bit word:
 *                byte 0 = modifier flags
 *                byte 1 = HID usage (0x04-0xE7), 0x00 = mods marker,
 *                         0xFF = erased = end of log
 * Linear log, stop + fast-blink alert when full.  Every event is written
 * immediately - nothing sits in RAM waiting for a flush, so battery
 * removal loses at most the key being written.  nRF52 flash programs
 * 32-bit words and allows each word to be programmed twice between erases
 * (bits only go 1->0): the first event of a pair goes out as {ev, 0xFFFF},
 * the second re-programs the same word as {prev, ev}.
 *
 * NOTE: the buffer holds the typed keys in clear text in flash until
 * CLEAR.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#if IS_ENABLED(CONFIG_K3YB_RECORDER_RESUME)
#include <zephyr/settings/settings.h>
#endif

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include <k3yb/led_ctrl.h>
#include <k3yb/recorder.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define REC_PART_ID FIXED_PARTITION_ID(recorder_partition)
#define REC_PART_SIZE FIXED_PARTITION_SIZE(recorder_partition)
#define REC_PAGE_SIZE DT_PROP(DT_CHOSEN(zephyr_flash), erase_block_size)

BUILD_ASSERT(REC_PART_SIZE % REC_PAGE_SIZE == 0, "recorder partition must be page aligned");

#define REC_MAGIC 0x3152334bu /* "K3R1" little-endian */
#define REC_HDR 4
#define REC_CAPACITY ((REC_PART_SIZE - REC_HDR) / 2)

#define EV(mods, usage) ((uint16_t)((mods) | ((uint16_t)(usage) << 8)))
#define EV_MODS(e) ((uint8_t)((e) & 0xff))
#define EV_USAGE(e) ((uint8_t)((e) >> 8))
#define USAGE_MARKER 0x00
#define USAGE_ERASED 0xff

#define PLAY_DELAY K_MSEC(CONFIG_K3YB_RECORDER_PLAYBACK_DELAY_MS)

#define ALERT_FULL_MS 3000 /* buffer full / flash error */
#define ALERT_ACK_MS 600   /* CLEAR done, or command refused */

/* flash-side state: owned by the recorder work queue once mounted */
static const struct flash_area *fa;
static uint32_t count;        /* events in flash */
static uint16_t pending_half; /* low half of a half-written word (count odd) */

static volatile bool mounted;
static volatile bool recording;
static volatile bool playing;
static volatile bool clearing;

/* listener side: mods of the last queued event, for the release marker */
static uint8_t last_mods;

/* events wait here only for the flash write - milliseconds, not a buffer */
K_MSGQ_DEFINE(rec_msgq, sizeof(uint16_t), 64, 2);

/* own queue: flash writes/erases never stall the system workqueue (and
 * with it the key scan) */
K_THREAD_STACK_DEFINE(rec_wq_stack, 1536);
static struct k_work_q rec_wq;

static void rec_state_dirty(void);

/* ---- flash side (rec_wq) ------------------------------------------------ */

static int write_magic(void) {
    uint32_t magic = REC_MAGIC;

    return flash_area_write(fa, 0, &magic, sizeof(magic));
}

static uint16_t read_event(uint32_t idx) {
    uint16_t ev = EV(0, USAGE_ERASED);

    flash_area_read(fa, REC_HDR + idx * 2, &ev, sizeof(ev));
    return ev;
}

/* the log is contiguous (written in order, erased tail), so the first
 * erased slot can be found by bisection */
static uint32_t scan_count(void) {
    uint32_t lo = 0, hi = REC_CAPACITY;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (EV_USAGE(read_event(mid)) == USAGE_ERASED) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

static void stop_recording(uint16_t alert_ms) {
    recording = false;
    k_msgq_purge(&rec_msgq);
    k3yb_status_blink_set(false);
    if (alert_ms) {
        k3yb_status_blink_alert(alert_ms);
    }
    rec_state_dirty();
}

static void rec_mount(struct k_work *work) {
    uint32_t magic = 0;
    int err;

    ARG_UNUSED(work);

    err = flash_area_open(REC_PART_ID, &fa);
    if (err) {
        LOG_ERR("recorder: cannot open partition (%d)", err);
        return;
    }
    flash_area_read(fa, 0, &magic, sizeof(magic));
    if (magic != REC_MAGIC) {
        /* first boot, or leftovers of an older firmware image */
        LOG_INF("recorder: formatting %d KiB", (int)(REC_PART_SIZE / 1024));
        err = flash_area_erase(fa, 0, REC_PART_SIZE);
        if (!err) {
            err = write_magic();
        }
        if (err) {
            LOG_ERR("recorder: format failed (%d)", err);
            return;
        }
        count = 0;
    } else {
        count = scan_count();
        if (count & 1) {
            pending_half = read_event(count - 1);
        }
    }
    mounted = true;
    LOG_INF("recorder: %u/%u events stored", count, (unsigned)REC_CAPACITY);
}

static K_WORK_DEFINE(rec_mount_work, rec_mount);

static void rec_write(struct k_work *work) {
    uint16_t ev;

    ARG_UNUSED(work);

    while (k_msgq_get(&rec_msgq, &ev, K_NO_WAIT) == 0) {
        uint32_t off = REC_HDR + count * 2;
        uint32_t word;
        int err;

        if (!mounted) {
            continue; /* partition unusable: drop */
        }
        if (count >= REC_CAPACITY) {
            LOG_WRN("recorder: buffer full, recording stopped");
            stop_recording(ALERT_FULL_MS);
            return;
        }
        if (count & 1) {
            off -= 2; /* second program of the same word */
            word = pending_half | ((uint32_t)ev << 16);
        } else {
            word = ev | 0xffff0000u;
            pending_half = ev;
        }
        err = flash_area_write(fa, off, &word, sizeof(word));
        if (err) {
            LOG_ERR("recorder: flash write failed (%d), recording stopped", err);
            stop_recording(ALERT_FULL_MS);
            return;
        }
        count++;
        if (count >= REC_CAPACITY) {
            LOG_WRN("recorder: buffer full, recording stopped");
            stop_recording(ALERT_FULL_MS);
            return;
        }
    }
}

static K_WORK_DEFINE(rec_write_work, rec_write);

static void rec_clear(struct k_work *work) {
    uint32_t pages = DIV_ROUND_UP(REC_HDR + count * 2, REC_PAGE_SIZE);
    int err = 0;

    ARG_UNUSED(work);

    k_msgq_purge(&rec_msgq);
    /* only used pages (the tail is erased by invariant), last first: an
     * interrupted CLEAR leaves a shorter but valid log, and page 0 (with
     * the magic) goes last - a missing magic just reformats at boot */
    for (int p = (int)pages - 1; p >= 0 && !err; p--) {
        err = flash_area_erase(fa, p * REC_PAGE_SIZE, REC_PAGE_SIZE);
    }
    if (!err) {
        err = write_magic();
    }
    if (err) {
        LOG_ERR("recorder: clear failed (%d)", err);
        mounted = false;
    } else {
        LOG_INF("recorder: cleared (%u pages)", pages);
        k3yb_status_blink_alert(ALERT_ACK_MS);
    }
    count = 0;
    clearing = false;
}

static K_WORK_DEFINE(rec_clear_work, rec_clear);

/* ---- capture ------------------------------------------------------------ */

static void rec_push(uint16_t ev) {
    if (k_msgq_put(&rec_msgq, &ev, K_NO_WAIT) != 0) {
        LOG_WRN("recorder: queue full, event dropped");
        return;
    }
    k_work_submit_to_queue(&rec_wq, &rec_write_work);
}

static int rec_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    uint8_t shown, mods;

    if (!ev || !recording || ev->usage_page != HID_USAGE_KEY ||
        ev->keycode > HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* explicit mods do not change for a non-modifier event, so this is
     * the same before or after hid_listener; the report drops masked mods */
    shown = zmk_hid_get_keyboard_report()->body.modifiers;
    mods = zmk_hid_get_explicit_mods() & shown;

    if (is_mod(ev->usage_page, ev->keycode)) {
        if (!ev->state) {
            mods &= ~BIT(ev->keycode - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
            if (last_mods & ~mods) {
                rec_push(EV(mods, USAGE_MARKER));
                last_mods = mods;
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state && ev->keycode >= HID_USAGE_KEY_KEYBOARD_A) {
        mods |= ev->implicit_modifiers;
        rec_push(EV(mods, ev->keycode));
        last_mods = mods;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(k3yb_recorder, rec_listener);
ZMK_SUBSCRIPTION(k3yb_recorder, zmk_keycode_state_changed);

static void start_recording(void) {
    /* session start marker: a log cut by power loss (or a previous session)
     * may end with mods still "held" - reset them before the first key */
    last_mods = 0;
    rec_push(EV(0, USAGE_MARKER));
    recording = true;
    k3yb_status_blink_set(true);
}

/* ---- playback (system workqueue, one step per delay) ----------------------- */

enum play_state { PLAY_FETCH, PLAY_PRESS, PLAY_RELEASE };

static enum play_state play_state;
static uint32_t play_pos, play_end;
static uint8_t play_mods, play_usage;
static volatile bool play_stop;

static void send_key(uint8_t usage, bool pressed) {
    raise_zmk_keycode_state_changed_from_encoded(ZMK_HID_USAGE(HID_USAGE_KEY, usage), pressed,
                                                 k_uptime_get());
}

/* modifiers are real modifier key events, held until the recorded state
 * changes (Alt must stay down across the 4 digits of an Alt+numpad code) */
static void set_mods(uint8_t mods) {
    uint8_t diff = play_mods ^ mods;

    for (int i = 0; i < 8; i++) {
        if (diff & BIT(i)) {
            send_key(HID_USAGE_KEY_KEYBOARD_LEFTCONTROL + i, mods & BIT(i));
        }
    }
    play_mods = mods;
}

static void play_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(play_work, play_step);

static void play_step(struct k_work *work) {
    ARG_UNUSED(work);

    switch (play_state) {
    case PLAY_PRESS:
        send_key(play_usage, true);
        play_state = PLAY_RELEASE;
        break;
    case PLAY_RELEASE:
        send_key(play_usage, false);
        play_state = PLAY_FETCH;
        break;
    case PLAY_FETCH:
        for (;;) {
            uint16_t ev;
            uint8_t usage;

            if (play_stop || play_pos >= play_end) {
                set_mods(0);
                playing = false;
                LOG_INF("recorder: playback %s at %u/%u", play_stop ? "stopped" : "done",
                        play_pos, play_end);
                return;
            }
            ev = read_event(play_pos++);
            usage = EV_USAGE(ev);
            if (usage == USAGE_ERASED) {
                play_pos = play_end;
                continue;
            }
            if (EV_MODS(ev) != play_mods) {
                /* mods first, key on the next step */
                set_mods(EV_MODS(ev));
                if (usage != USAGE_MARKER) {
                    play_usage = usage;
                    play_state = PLAY_PRESS;
                }
                break;
            }
            if (usage != USAGE_MARKER) {
                play_usage = usage;
                send_key(usage, true);
                play_state = PLAY_RELEASE;
                break;
            }
            /* marker without a mods change: nothing to send */
        }
        break;
    }
    k_work_schedule(&play_work, PLAY_DELAY);
}

/* ---- persistence of the REC flag (optional) ------------------------------ */

#if IS_ENABLED(CONFIG_K3YB_RECORDER_RESUME)

/* ZMK deep-sleeps after the idle timeout and wakes through a reset: keep
 * recording across it, so a long pause in a meeting does not silently end
 * the notes.  One tiny settings entry, not the buffer. */
static void rec_state_save(struct k_work *work) {
    uint8_t on = recording;

    ARG_UNUSED(work);
    settings_save_one("k3yb_rec/on", &on, sizeof(on));
}

static K_WORK_DELAYABLE_DEFINE(rec_state_save_work, rec_state_save);

static void rec_state_dirty(void) { k_work_reschedule(&rec_state_save_work, K_MSEC(800)); }

static int rec_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                            void *cb_arg) {
    uint8_t on;

    if (strcmp(name, "on") != 0 || len != sizeof(on)) {
        return -ENOENT;
    }
    if (read_cb(cb_arg, &on, sizeof(on)) != sizeof(on)) {
        return -EIO;
    }
    if (on && !recording) {
        LOG_INF("recorder: resuming recording after reset");
        start_recording();
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(k3yb_rec, "k3yb_rec", NULL, rec_settings_set, NULL, NULL);

#else
static void rec_state_dirty(void) {}
#endif /* CONFIG_K3YB_RECORDER_RESUME */

/* ---- public API ------------------------------------------------------------- */

static bool busy(const char *what) {
    if (!mounted || playing || clearing) {
        LOG_WRN("recorder: %s refused (%s)", what,
                !mounted ? "not mounted" : playing ? "playing" : "clearing");
        k3yb_status_blink_alert(ALERT_ACK_MS);
        return true;
    }
    return false;
}

void k3yb_recorder_toggle(void) {
    if (recording) {
        recording = false;
        k3yb_status_blink_set(false);
        LOG_INF("recorder: REC off, %u events", count);
    } else {
        if (busy("REC")) {
            return;
        }
        if (count >= REC_CAPACITY) {
            LOG_WRN("recorder: buffer full, CLEAR first");
            k3yb_status_blink_alert(ALERT_FULL_MS);
            return;
        }
        start_recording();
        LOG_INF("recorder: REC on, %u/%u events", count, (unsigned)REC_CAPACITY);
    }
    rec_state_dirty();
}

void k3yb_recorder_playback(void) {
    if (playing) {
        play_stop = true;
        return;
    }
    if (recording) {
        LOG_WRN("recorder: stop REC before playback");
        k3yb_status_blink_alert(ALERT_ACK_MS);
        return;
    }
    if (busy("playback")) {
        return;
    }
    /* drain pending writes so count is final */
    k_work_flush(&rec_write_work, &(struct k_work_sync){});
    play_pos = 0;
    play_end = count;
    play_mods = 0;
    play_state = PLAY_FETCH;
    play_stop = false;
    playing = true;
    LOG_INF("recorder: playback of %u events", play_end);
    k_work_schedule(&play_work, PLAY_DELAY);
}

void k3yb_recorder_clear(void) {
    if (busy("CLEAR")) {
        return;
    }
    if (recording) {
        recording = false;
        k3yb_status_blink_set(false);
        rec_state_dirty();
    }
    clearing = true;
    k_work_submit_to_queue(&rec_wq, &rec_clear_work);
}

bool k3yb_recorder_is_recording(void) { return recording; }
bool k3yb_recorder_is_playing(void) { return playing; }
uint32_t k3yb_recorder_count(void) { return count; }
uint32_t k3yb_recorder_capacity(void) { return REC_CAPACITY; }

static int recorder_init(void) {
    k_work_queue_start(&rec_wq, rec_wq_stack, K_THREAD_STACK_SIZEOF(rec_wq_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
    k_thread_name_set(&rec_wq.thread, "k3yb_rec");
    k_work_submit_to_queue(&rec_wq, &rec_mount_work);
    return 0;
}

SYS_INIT(recorder_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
