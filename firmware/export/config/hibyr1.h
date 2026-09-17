/*
 * This config file is for the HiBy R1
 */

//#define ROCKBOX_HAS_LOGF

/* For Rolo and boot loader */
#define MODEL_NUMBER 124
#define MODEL_NAME   "HIBY R1"

#define PIVOT_ROOT "/data/mnt/sd_0"
#define MULTIDRIVE_DIR "/data/mnt/usb"

/* LCD dimensions */
/* sqrt(width^2 + height^2) / 4 = 233 */
#define LCD_WIDTH 480
#define LCD_HEIGHT 800
#define LCD_DPI 233

#define HAVE_LCD_SLEEP
#define LCD_SLEEP_TIMEOUT (2*HZ)

/* Both of these live in lcd-linuxfb.c, which the simulator does not build:
 * the sim has its own SDL LCD driver and no framebuffer planes at all. */
#ifndef SIMULATOR
/* Present through a second framebuffer plane, flipped on a vertical blank.
 * Removes tearing and the clear-then-draw flash; falls back to the old
 * single-plane path by itself if the driver will not give us the plane. */
#define HAVE_FB_DOUBLEBUF
/* Per-frame timing and damage instrumentation, shown in the debug menu */
#define HAVE_LCD_PRESENT_STATS
#endif

/* Surfaces and layers over the framebuffer (apps/canvas.c, canvas_glue.c).
 * Costs a pool of at most CANVAS_POOL_MAX_KIB and degrades to the plain
 * direct-to-framebuffer path if it cannot have it. */
#define HAVE_COMPOSITOR

/* Retained layers composed at present time (firmware/drivers/lcd-layers.c).
 * The stick overlay lives in one instead of in the framebuffer. */
#define HAVE_LCD_LAYERS

/* Animated screen changes (firmware/drivers/lcd-transition.c), presented
 * through the layers' update hook. Two frame copies, 1.5 MiB. */
#define HAVE_LCD_TRANSITIONS

/* Albums, tracks, playlists and audiobooks as covers (apps/gui/coverview.c).
 * A cache of twelve 320 px covers, 2.5 MiB. */
#define HAVE_COVER_VIEWS

/* Blend antialiased glyphs and alpha images in linear light rather than in
 * gamma-encoded RGB565, so a glyph keeps its weight whatever it landed on.
 * Six table lookups per blended pixel; 2.2 KiB of tables. */
#define HAVE_GAMMA_AWARE_TEXT

/* PNG album art (apps/recorder/png_load.c). The decoder is ~600 lines on
 * top of firmware/common/inflate.c, which was already in the build. */
#define HAVE_PNG

#define LCD_DEPTH  16   /* 65536 colours */
#define LCD_PIXELFORMAT RGB565 /* rgb565 */

#define CPU_FREQ           1008000000

#ifndef SIMULATOR
#define HAVE_GENERAL_PURPOSE_LED
#endif

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN
#define HAVE_HOTKEY

#define HAVE_HEADPHONE_DETECTION
//#define NO_BUTTON_LR

#ifndef BOOTLOADER
#define HAVE_BUTTON_DATA
#define HAVE_TOUCHSCREEN
#endif

#ifndef CONFIG_BACKLIGHT_FADING
#undef CONFIG_BACKLIGHT_FADING
#endif

/* KeyPad configuration for plugins */
#define CONFIG_KEYPAD HIBY_R1_PAD
//unlock_combo ignores pre-button. so it's triggered for both short press or long press release actions
#define DISABLE_ACTION_REMAP_UNLOCK_COMBO
#define BUTTON_NEED_DEV_INPUT_ID

/* Default mapping doesn't support it, but we allow it via remapping */
#define HAVE_VOLUME_IN_LIST

/* Battery */
#define BATTERY_TYPES_COUNT  1

/* Audio codec */
#define HAVE_HIBY_LINUX_CODEC
#define HAVE_HIBY_BLUETOOTH

/* We don't have hardware controls */
#define HAVE_SW_TONE_CONTROLS

/* HW codec is flexible */
#define HW_SAMPR_CAPS SAMPR_CAP_ALL_192

/* Battery */
#define HAVE_HIBY_LINUX_POWER_CHARGE_LIMIT
#define CONFIG_BATTERY_MEASURE (VOLTAGE_MEASURE|PERCENTAGE_MEASURE|TIME_MEASURE)

#define BATTERY_CAPACITY_DEFAULT 100 /* default battery capacity */
#define BATTERY_CAPACITY_MIN 100  /* min. capacity selectable */
#define BATTERY_CAPACITY_MAX 100 /* max. capacity selectable */
#define BATTERY_CAPACITY_INC 0   /* capacity increment */

/* Special backlight paths */
#define BACKLIGHT_HIBY

/* The idle timeout dims the panel instead of blanking it, to a level the
 * user chooses. See do_backlight_off() in firmware/backlight.c. */
#define HAVE_BACKLIGHT_DIM_IDLE
/* backlight_hw_brightness_fine(): tenths of a percent, scaled to the
 * driver's own max_brightness, for dim levels finer than 1 %. */
#define HAVE_BACKLIGHT_FINE_BRIGHTNESS

/* This device has no hold switch, and its lock is a software one the user
 * asks for with a key. Locking input must not also blank the screen. See
 * backlight_get_current_timeout(). */
#define SOFTLOCK_KEEPS_BACKLIGHT

/* Physical keys pressed on a dimmed or dark screen only wake it; the press
 * is dropped (firmware/drivers/button.c). Bluetooth remote keys are left
 * out: a remote is used with the player in a pocket. The value is
 * BUTTON_POWER|RIGHT|LEFT|UP|DOWN from the target's button-target.h. */
#define BUTTON_WAKE_SWALLOWS 0x3d
#ifndef BOOTLOADER
/* A tap on a dimmed or dark screen wakes it (button-devinput.c). */
#define BUTTON_TOUCH_WAKES
#endif

#define MIN_BRIGHTNESS_SETTING      1
#define MAX_BRIGHTNESS_SETTING      100
#define BRIGHTNESS_STEP             5
#define DEFAULT_BRIGHTNESS_SETTING  100

/* ROLO */
#define BOOTFILE_EXT "r1"
#define BOOTFILE     "rockbox." BOOTFILE_EXT
#define BOOTDIR      "/.rockbox"

/* USB */
#define USB_VID_STR "32BB"
#define USB_PID_STR "0101"

/* Generic HiBy stuff */
#include "hibylinux_x1600.h"
