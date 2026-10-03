#pragma once

#include <driver/gpio.h>


#define DISPLAY_SDA_PIN GPIO_NUM_4
#define DISPLAY_SCL_PIN GPIO_NUM_5 

#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true