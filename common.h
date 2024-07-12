#pragma once

#include "tusb.h"

const uint8_t NO_DEV = 0xff;

extern uint8_t keyboard_dev_addr;
extern uint8_t mouse_dev_addr;
extern uint8_t keyboard_instance;
extern uint8_t mouse_instance;

extern bool do_connect;
extern bool do_disconnect;

extern bool should_output();
extern void toggle_output();
extern void set_led(bool on);
extern void set_current_output_mask(uint8_t val);

extern void print_kbd_report(const hid_keyboard_report_t *report);
extern void print_mouse_report(const hid_mouse_report_t *report);
extern void check_kbd_report(const hid_keyboard_report_t *report);
