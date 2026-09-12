#ifndef __HIBYLINUX_CODEC__
#define __HIBYLINUX_CODEC__

#define AUDIOHW_CAPS (FILTER_ROLL_OFF_CAP)
/* The usable range on this device, not the hardware's. Everything below
 * -70 dB is inaudible, so that is the bottom of the scale and the
 * percentage Rockbox shows is a percentage of the range actually worth
 * having.
 *
 * The top was -20 dB, which is plenty for efficient IEMs and short of what
 * anything harder to drive wants; -10 dB leaves headroom for high-impedance
 * cans and quietly mastered material. It is a ceiling, not a target - 100%
 * here is loud, and the scale is 60 dB wide now rather than 50, so a given
 * percentage is a little quieter than it was before this change. */
AUDIOHW_SETTING(VOLUME, "dB", 1, 5, -70*10, -10*10, -50*10)
#endif

//#define AUDIOHW_MUTE_ON_STOP
#define AUDIOHW_MUTE_ON_SRATE_CHANGE
//#define AUDIOHW_NEEDS_INITIAL_UNMUTE

AUDIOHW_SETTING(FILTER_ROLL_OFF, "", 0, 1, 0, 4, 0)
#define AUDIOHW_HAVE_SHORT2_ROLL_OFF

void audiohw_mute(int mute);
void hiby_set_output(int ps);
int hiby_get_outputs(void);
