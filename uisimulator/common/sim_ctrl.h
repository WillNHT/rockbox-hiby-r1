/***************************************************************************
 * Headless control channel for the simulator. See sim_ctrl.c.
 ****************************************************************************/
#ifndef _SIM_CTRL_H_
#define _SIM_CTRL_H_

/* Starts the control thread if RBSIM_CTL names a path. Does nothing
 * otherwise, so a normally-launched simulator is unaffected. */
void sim_ctrl_init(void);

#endif
