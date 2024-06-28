/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *                    sekigon-gonnoc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

// This example runs both host and device concurrently. The USB host receive
// reports from HID device and print it out over USB Device CDC interface.
// For TinyUSB roothub port0 is native usb controller, roothub port1 is
// pico-pio-usb.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "common.h"
#include "pio_usb.h"
#include "tusb.h"
#include "uart_messages.h"
#include "usb_descriptors.h"

static uint8_t const keycode2ascii[128][2] =  { HID_KEYCODE_TO_ASCII };

uint8_t keyboard_dev_addr = NO_DEV;
uint8_t mouse_dev_addr = NO_DEV;
uint8_t keyboard_instance;
uint8_t mouse_instance;

bool connected = true;
bool do_connect = false;
bool do_disconnect = false;

#define PIO_USB_CONFIG                                                 \
  {                                                                            \
    PIO_USB_DP_PIN_DEFAULT, PIO_USB_TX_DEFAULT, PIO_SM_USB_TX_DEFAULT,         \
        PIO_USB_DMA_TX_DEFAULT, PIO_USB_RX_DEFAULT, PIO_SM_USB_RX_DEFAULT,     \
        PIO_SM_USB_EOP_DEFAULT, NULL, PIO_USB_DEBUG_PIN_NONE,                  \
        PIO_USB_DEBUG_PIN_NONE                                                 \
  }

const int SEND_TO_HOST = 1;
const int SEND_TO_UART = 2;

int destination = SEND_TO_HOST | SEND_TO_UART;


// core1: handle host events
void core1_main() {
  sleep_ms(10);

  // Use tuh_configure() to pass pio configuration to the host stack
  // Note: tuh_configure() must be called before
  pio_usb_configuration_t pio_cfg = PIO_USB_CONFIG;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // To run USB SOF interrupt in core1, init host stack for pio_usb (roothub
  // port1) on core1
  tuh_init(1);

  while (true) {
    tuh_task(); // tinyusb host task
  }
}

//--------------------------------------------------------------------+
// Host HID
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  (void)desc_report;
  (void)desc_len;

  // Interface protocol (hid_interface_protocol_enum_t)
  const char* protocol_str[] = { "None", "Keyboard", "Mouse" };
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
  {
    keyboard_dev_addr = dev_addr;
    keyboard_instance = instance;
    send_uart_keyboard_connected(true);
  }
  else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
  {
    mouse_dev_addr = dev_addr;
    mouse_instance = instance;
    send_uart_mouse_connected(true);
  }

  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  char tempbuf[256];
  int count = sprintf(tempbuf, "[%04x:%04x][%u] HID Interface%u, Protocol = %s, Desc len %d\r\n", vid, pid, dev_addr, instance, protocol_str[itf_protocol], desc_len);
  printf("%s\n", tempbuf);

  tud_cdc_write(tempbuf, count);
  tud_cdc_write_flush();

  while (desc_len > 0)
  {
    char *p = tempbuf;
    int max = desc_len;
    if (max > 32)
    {
      max = 32;
    }

    for (int i = 0; i < max; i++)
    {
      p += snprintf(p, 4, "%02x ", desc_report[i]);
    }
    *p++ = '\r';
    *p++ = '\n';
    *p++ = '\0';
    printf("%s", tempbuf);
    tud_cdc_write(tempbuf, p - 1 - tempbuf);
    tud_cdc_write_flush();
    desc_len -= max;
    desc_report += max;
  }

  // Receive report from boot keyboard & mouse only
  // tuh_hid_report_received_cb() will be invoked when report is available
  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD || itf_protocol == HID_ITF_PROTOCOL_MOUSE)
  {
    if ( !tuh_hid_receive_report(dev_addr, instance) )
    {
      tud_cdc_write_str("Error: cannot request report\r\n");
    }
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  if (dev_addr == keyboard_dev_addr)
  {
    keyboard_dev_addr = NO_DEV;
    send_uart_keyboard_connected(false);
  }
  else if (dev_addr == mouse_dev_addr)
  {
    mouse_dev_addr = NO_DEV;
    send_uart_mouse_connected(false);
  }

  printf("[%u] HID Interface%u is unmounted\r\n", dev_addr, instance);
}

// look up new key in previous keys
static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++)
  {
    if (report->keycode[i] == keycode)  return true;
  }

  return false;
}

void check_kbd_report(const hid_keyboard_report_t *report)
{
  static hid_keyboard_report_t prev_report = { 0, 0, {0} }; // previous report to check key state changed
  uint8_t keycode = report->keycode[0];
  uint8_t prev_keycode = prev_report.keycode[0];
  printf("check keycode %u %u %u\n", keycode, prev_keycode, HID_KEY_F12);
  if (prev_keycode != keycode)
  {
    if (keycode == 0 && prev_keycode == HID_KEY_F12)
    {
      prev_keycode = 0;
      printf("toggle output curr %u\n", current_output_mask);
      if (current_output_mask == 1)
      {
        current_output_mask = 2;
      }
      else if (current_output_mask == 2)
      {
        current_output_mask = 1;
      }
      send_uart_set_output_mask(current_output_mask);
#if 0
      connected = !connected;
      printf("toggle %d\n", !connected);
      if (!connected)
      {
        do_disconnect = true;
      }
      else
      {
        do_connect = true;
      }
#endif          
    }
  }
  prev_report = *report;
}

// convert hid keycode to ascii and print via usb device CDC (ignore non-printable)
void print_kbd_report(const hid_keyboard_report_t *report)
{
  char buf[64];
  int pos = 0;
  bool is_shift = report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);

  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_LEFTSHIFT) != 0 ? 'L' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_LEFTCTRL) != 0 ? 'l' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_LEFTALT) != 0 ? 'a' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_LEFTGUI) != 0 ? 'A' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) != 0 ? 'R' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_RIGHTCTRL) != 0 ? 'r' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_RIGHTALT) != 0 ? 'b' : ' ';
  buf[pos++] = (report->modifier & KEYBOARD_MODIFIER_RIGHTGUI) != 0 ? 'B' : ' ';
  buf[pos++] = ' ';
  for(uint8_t i=0; i<6; i++)
  {
    uint8_t keycode = report->keycode[i];
    pos += snprintf(buf + pos, 6, "[%02x] ", keycode & 0xff);
    if ( keycode )
    {
      uint8_t ch = keycode2ascii[keycode][is_shift ? 1 : 0];

      /*if ( find_key_in_report(&prev_report, keycode) )
      {
      }
      else*/
      if (ch >= 0x20)
      {
        buf[pos++] = ch;
        buf[pos++] = ' ';
      }
    }
  }

  printf("%s\n", buf);
}

static void process_kbd_report(uint8_t dev_addr, hid_keyboard_report_t *report)
{
  (void) dev_addr;
  //bool flush = false;

  if (connected)
  {
    if (should_output())
    {
      tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report->modifier, report->keycode);
    }

    if ((destination & SEND_TO_UART) != 0)
    {
      send_uart_kb_report(report);
    }
  }
  else
  {
    printf("not connected\n");
  }
  
  check_kbd_report(report);
  print_kbd_report(report);
}

void print_mouse_report(const hid_mouse_report_t *report)
{
  //------------- button state  -------------//
  //uint8_t button_changed_mask = report->buttons ^ prev_report.buttons;
  char l = report->buttons & MOUSE_BUTTON_LEFT   ? 'L' : '-';
  char m = report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-';
  char r = report->buttons & MOUSE_BUTTON_RIGHT  ? 'R' : '-';

  printf("%c%c%c %d %d %d %d\n", l, m, r, report->x, report->y, report->wheel, report->pan);
}

// send mouse report to usb device CDC
static void process_mouse_report(uint8_t /*dev_addr*/, hid_mouse_report_t const * report)
{
  if (connected)
  {
    if (should_output())
    {
      tud_hid_mouse_report(REPORT_ID_MOUSE, report->buttons, report->x, report->y, report->wheel, report->pan);
    }

    if ((destination & SEND_TO_UART) != 0)
    {
      send_uart_mouse_report(report);
    }
  }
  else
  {
    printf("not connected\n");
  }

  print_mouse_report(report);
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) len;
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  printf("got report %d\n", itf_protocol);
  switch(itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      process_kbd_report(dev_addr, (hid_keyboard_report_t *) report );
    break;

    case HID_ITF_PROTOCOL_MOUSE:
      process_mouse_report(dev_addr, (hid_mouse_report_t const*) report );
    break;

    default: break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    printf("Error: cannot request report\n");
  }
}

