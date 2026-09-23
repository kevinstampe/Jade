#ifndef INPUT_H_
#define INPUT_H_

#include "sdkconfig.h"

void input_init(void);

#if defined(CONFIG_DISPLAY_TOUCHSCREEN)
void touchscreen_init(void);
// Stops the touch polling task and releases the touch device and its i2c bus,
// allowing the touch controller to drop into its own standby state.
void touchscreen_deinit(void);
#endif

#endif /* INPUT_H_ */
