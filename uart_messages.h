#pragma once

#include "tusb.h"

extern void uart_task();
extern void init_uart();
extern void send_uart_kb_report(hid_keyboard_report_t *report);
