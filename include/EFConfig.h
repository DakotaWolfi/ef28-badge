// EFConfig.h

#pragma once

#define HasDisplay   1      // or comment out to disable
#define BOARD_NAME   "EF28"
#define EF_USER_NAME ""
//#define Mel 1 // comment out to disable if you dont know mel4anie XD

//#define EF_USER_NAME_FORCE 1   // uncomment to force EF_USER_NAME on each boot

// The Step-Down converter still manages to hold 3.00V with 3,32V input. The ESP needs 3.0V at least
#define EFBOARD_PIN_VBAT 10                //!< Pin the analog voltage divider for V_BAT is connected to (ADC1_CH9)
// --- choose battery type ---
#define EFBOARD_BAT_TYPE_LIION     // or comment this out to use Alkaline
