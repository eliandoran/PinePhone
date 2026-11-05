/* <sys/prctl.h>, which Emscripten does not have: there are no processes to set up */
#ifndef PHONE_UI_WASM_SYS_PRCTL_H
#define PHONE_UI_WASM_SYS_PRCTL_H

#define PR_SET_PDEATHSIG 1

static inline int prctl(int option, ...)
{
	(void)option;
	return -1;
}

#endif
