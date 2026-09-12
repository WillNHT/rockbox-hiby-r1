#ifndef __HIBYLINUX_CODEC__
#define __HIBYLINUX_CODEC__

#define AUDIOHW_CAPS (FILTER_ROLL_OFF_CAP)
/* The usable range on this device, not the hardware's. Anything above
 * -20 dB is unusable with any normal headphone and everything below -70 dB
 * is inaudible, so those become the ends of the scale and the percentage
 * Rockbox shows is a percentage of the range actually worth having. */
AUDIOHW_SETTING(VOLUME, "dB", 1, 5, -70*10, -20*10, -50*10)
#endif

//#define AUDIOHW_MUTE_ON_STOP
#define AUDIOHW_MUTE_ON_SRATE_CHANGE
//#define AUDIOHW_NEEDS_INITIAL_UNMUTE

AUDIOHW_SETTING(FILTER_ROLL_OFF, "", 0, 1, 0, 4, 0)
#define AUDIOHW_HAVE_SHORT2_ROLL_OFF

void audiohw_mute(int mute);
void hiby_set_output(int ps);
int hiby_get_outputs(void);
