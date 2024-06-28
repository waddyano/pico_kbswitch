#pragma once

#include "tusb.h"

extern void uart_task();
extern void init_uart();
extern void send_uart_kb_report(const hid_keyboard_report_t *report);
extern void send_uart_mouse_report(const hid_mouse_report_t *report);
extern void send_uart_keyboard_report(uint8_t leds);
extern void send_uart_keyboard_connected(bool connected);
extern void send_uart_mouse_connected(bool connected);
extern void send_uart_enable_board(int number);
extern void send_uart_set_output_mask(uint8_t mask);
