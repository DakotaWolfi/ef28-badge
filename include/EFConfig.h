// EFConfig.h

#pragma once
//General Config
#define HasDisplay   1      // or comment out to disable
#define BOARD_NAME   "EF28"
#define EF_USER_NAME "N/A"
//#define Mel 1 // comment out to disable if you dont know mel4anie XD






//#define EF_USER_NAME_FORCE 1   // uncomment to force EF_USER_NAME on each boot




//EFBord Config




// The Step-Down converter still manages to hold 3.00V with 3,32V input. The ESP needs 3.0V at least
#define EFBOARD_PIN_VBAT 10                //!< Pin the analog voltage divider for V_BAT is connected to (ADC1_CH9)
// --- choose battery type ---
#define EFBOARD_BAT_TYPE_LIION     // or comment this out to use Alkaline


//EFLed Config
#define EFLED_PIN_LED_DATA 21
#define EFLED_PIN_5VBOOST_ENABLE 9

#define EFLED_TOTAL_NUM 17
#define EFLED_DRAGON_NUM 6
#define EFLED_EFBAR_NUM 11

#define EFLED_DARGON_OFFSET 0
#define EFLED_EFBAR_OFFSET 6

#define EFLED_DRAGON_NOSE_IDX 0
#define EFLED_DRAGON_MUZZLE_IDX 1
#define EFLED_DRAGON_EYE_IDX 2
#define EFLED_DRAGON_CHEEK_IDX 3
#define EFLED_DRAGON_EAR_BOTTOM_IDX 4
#define EFLED_DRAGON_EAR_TOP_IDX 5


//EFDisplay Config

#define OLED_CS    5   // Chip Select
#define OLED_DC    6   // Data/Command
#define OLED_RESET 7   // Reset
#define OLED_MOSI  17  // MOSI
#define OLED_SCLK  18  // SCLK

// --- AUDIO / NOISE CONFIG ---
#define AUDIO_PIN   4
#define NOISE_PIN   12   // <- use exactly one floating pin


//EFTouch Config 
#define EFTOUCH_PIN_TOUCH_FINGERPRINT 3
#define EFTOUCH_PIN_TOUCH_NOSE 1

#define EFTOUCH_CALIBRATE_NUM_SAMPLES 10
#define EFTOUCH_SHORTPRESS_DURATION_MS 450
#define EFTOUCH_LONGPRESS_DURATION_MS  1800
#define EFTOUCH_MULTITOUCH_COOLDOWN_MS 1000