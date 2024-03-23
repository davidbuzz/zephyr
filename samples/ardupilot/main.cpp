/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* hello world example: calling functions from a static library */


#include <zephyr/kernel.h>
#include <stdio.h>

#define AP_MAIN notmain

#include <AP_HAL/AP_HAL.h> // needs to be first
#include <AP_Common/AP_Common.h>
#include <AP_HAL/board/esp32.h>     
#include <AP_HAL/board/esp32s3empty.h> 

#include <AP_HAL_ESP32/AP_HAL_ESP32.h>




void setup();
void loop();
void test_high_low_byte(void);

const AP_HAL::HAL& hal = AP_HAL::get_HAL();

void test_high_low_byte(void)
{

    // test each value from 0 to 300
    for (uint16_t i = 0; i <= 300; i++) {
        uint8_t high = HIGHBYTE(i);
        uint8_t low = LOWBYTE(i);
        //hal.console->printf("\ni:%u high:%u low:%u", (unsigned int)i, (unsigned int)high, (unsigned int)low);
    }

    // test values from 300 to 65400 at increments of 200
    for (uint16_t i = 301; i <= 65400; i += 200) {
        uint8_t high = HIGHBYTE(i);
        uint8_t low = LOWBYTE(i);
        //hal.console->printf("\ni:%u high:%u low:%u", (unsigned int)i, (unsigned int)high, (unsigned int)low);
    }
}

/*
 *  euler angle tests
 */
void setup(void)
{
    //hal.console->printf("AP_Common tests\n\n");

    test_high_low_byte();
}

void loop(void)
{
    // do nothing
		printf("zHello World!\n");
		// sleep 
		//hal.scheduler->delay(1000);
}


//AP_HAL_MAIN();
//AP_HAL::HAL::FunCallbacks callbacks(setup, loop);


int main(void)
{
	printf("Hello World!\n");
	//mylib_hello_world();

	int argc=1;
	char* const* argv;

	//hal.run(argc, argv, &callbacks);

    //hal.console->printf("AP_Common tests\n\n");

	setup();
		printf("xHello World!\n");

	while (1)loop();

	return 0;
}
