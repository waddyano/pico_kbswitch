#pragma once

#include "tusb.h"

const uint8_t NO_DEV = 0xff;

extern uint8_t keyboard_dev_addr;
extern uint8_t mouse_dev_addr;
extern uint8_t keyboard_instance;
extern uint8_t mouse_instance;

extern void print_kbd_report(hid_keyboard_report_t *report);
