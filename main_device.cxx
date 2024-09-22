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

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "common.h"
#include "pio_usb.h"
#include "tusb.h"
#include "uart_messages.h"
#include "usb_descriptors.h"

/*------------- MAIN -------------*/

// core1: handle host events
extern void core1_main();

const uint LED_PIN = PICO_DEFAULT_LED_PIN;
const uint LED2_PIN = 14;
const uint SENSE_PIN = 13;
const uint TOGGLE_PIN = 17;

static int click_state;

static void gpio_callback(uint gpio, uint32_t events)
{
  //printf("irq %u %lx - state %d\n", gpio, events, click_state);
  if (gpio == TOGGLE_PIN)
  {
    if ((events & GPIO_IRQ_EDGE_FALL) != 0)
    {
      if (click_state == 0)
      {
        click_state = 1;
      }
    }
    else if ((events & GPIO_IRQ_EDGE_RISE) != 0)
    {
      if ((click_state & 1) != 0)
      {
        click_state = 2;
      }
    }
  }
}

static int64_t click_timer_callback(alarm_id_t id, void *p)
{
  bool *debouncing = static_cast<bool *>(p);
  printf("tick click %ld\n", id);
  *debouncing = false;
  click_state = 0;
  return 0;
}

static void init_gpio()
{
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  //gpio_init(LED2_PIN);
  //gpio_set_dir(LED2_PIN, GPIO_OUT);
  gpio_set_function(LED2_PIN, GPIO_FUNC_PWM);
  uint slice_num = pwm_gpio_to_slice_num(LED2_PIN);
  pwm_config config = pwm_get_default_config();
  pwm_config_set_clkdiv(&config, 4.f);
  pwm_init(slice_num, &config, true);

  gpio_init(SENSE_PIN);
  gpio_set_dir(SENSE_PIN, GPIO_IN);
  gpio_set_pulls(SENSE_PIN, true, false);

  gpio_init(TOGGLE_PIN);
  gpio_set_dir(TOGGLE_PIN, GPIO_IN);
  gpio_set_pulls(TOGGLE_PIN, true, false);
  gpio_set_irq_enabled_with_callback(TOGGLE_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
}

void set_led(bool on)
{
  //gpio_put(LED2_PIN, on);
  pwm_set_gpio_level(LED2_PIN, on ? 10000 : 0);
}

static uint8_t board_number = 0; // set once at startup
static uint8_t current_output_mask = 1; // board zero is the default

// save current_output_mask so if watchdog triggers it can be restored
static void update_watchdog_state()
{
  watchdog_hw->scratch[3] = current_output_mask;
}

void set_current_output_mask(u_int8_t val)
{
  current_output_mask = val;
  update_watchdog_state();
}

void toggle_output()
{
  printf("toggle output curr %u\n", current_output_mask);
  if (current_output_mask == 1)
  {
    current_output_mask = 2;
  }
  else if (current_output_mask == 2)
  {
    current_output_mask = 1;
  }
  update_watchdog_state();
  send_uart_set_output_mask(current_output_mask);
}

bool should_output()
{
  return (current_output_mask & (1 << board_number)) != 0;
}

// core0: handle device events
int main(void) {
  // default 125MHz is not appropreate. Sysclock should be multiple of 12MHz.
  set_sys_clock_khz(120000, true);
  stdio_uart_init_full(uart1, 115200, 8, 9);
  init_gpio();

  sleep_ms(10);

  if (!gpio_get(SENSE_PIN))
    board_number = 1;

  init_uart();

  multicore_reset_core1();
  // all USB task run in core1
  multicore_launch_core1(core1_main);

  // init device stack on native usb (roothub port0)
  tud_init(0);

  uint64_t led_last_change = time_us_64();
  bool led_on = true;
  gpio_put(LED_PIN, led_on);
  int flash_count = 15;
  if (watchdog_enable_caused_reboot())
  {
    flash_count = 36000; // about two hours
    current_output_mask = watchdog_hw->scratch[3];
    int step = watchdog_hw->scratch[2];
    printf("watchdog caused reboot at step %d mask %d\n", step, current_output_mask);
  }

  watchdog_enable(100, 0);

  bool debouncing = false;
  while (true) {
    watchdog_hw->scratch[2] = 1;
    tud_task(); // tinyusb device task
    watchdog_hw->scratch[2] = 2;
    tud_cdc_write_flush();
    watchdog_hw->scratch[2] = 3;
    uart_task();
    if (do_disconnect)
    {
      do_disconnect = false;
      printf("do disconnect\n");
      tud_disconnect();
    }
    if (do_connect)
    {
      do_connect = false;
      printf("do connect\n");
      tud_connect();
    }
    watchdog_hw->scratch[2] = 4;
    set_led(should_output());
    if (flash_count > 0)
    {
      uint64_t tick = time_us_64();
      if (tick - led_last_change > 200000)
      {
        flash_count--;
        led_on = flash_count != 0 && !led_on;
        gpio_put(LED_PIN, led_on);
        led_last_change = tick;
      }
    }
    watchdog_hw->scratch[2] = 5;
    if (click_state == 2 && !debouncing)
    {
      printf("process click %lld ms\n", time_us_64() / 1000ll);
      debouncing = true;
      auto id = add_alarm_in_ms(500, click_timer_callback, &debouncing, false);
      printf("alarm id %ld\n", id);
      toggle_output();
    }
    watchdog_hw->scratch[2] = 6;
    watchdog_update();
    watchdog_hw->scratch[2] = 7;
  }

  return 0;
}

//--------------------------------------------------------------------+
// Device CDC
//--------------------------------------------------------------------+

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
  (void) itf;

  char buf[64];
  uint32_t count = tud_cdc_read(buf, sizeof(buf));

  // TODO control LED on keyboard of host stack
  (void) count;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  printf("report itf %d kda %d kitf %d id %d type %d size %d buf %x\n", keyboard_dev_addr, keyboard_instance, instance, report_id, report_type, bufsize, bufsize > 0 ? buffer[0] : 0);
  if (keyboard_instance == instance && report_type == HID_REPORT_TYPE_OUTPUT && bufsize > 0)
  {
    static uint8_t leds;
    leds = buffer[0];
    printf("send leds %x\n", leds);
    if (keyboard_dev_addr != NO_DEV)
    {
      tuh_hid_set_report(keyboard_dev_addr, keyboard_instance, 0, HID_REPORT_TYPE_OUTPUT, &leds, sizeof(leds));
    }
    send_uart_keyboard_report(leds);
  }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  printf("get report type %d\n", report_type);
  // TODO not Implemented
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) instance;
  (void) report;
  (void) len;


  //uint8_t next_report_id = report[0] + 1u;
}








