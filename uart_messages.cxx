#include "hardware/gpio.h"
#include "hardware/uart.h"

#include "uart_messages.h"

#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_IRQ UART0_IRQ 

const uint8_t SENTINEL = 0x7e;
const uint8_t ESCAPE = 0x7d;

enum MessageType : uint8_t
{
  KEYBOARD,
  MOUSE
};

static cri
static uint8_t rx_buf[256];
static int rx_rptr;
static int rx_wptr;

static void read_pending()
{
  while (uart_is_readable(UART_ID))
  {
    uint8_t ch = uart_getc(UART_ID);
    rx_buf[rx_ptr++] = ch;
    if (rx_ptr == sizeof(rx_buf))
    {
      printf("oh dear\n");
    }
  }
}

static void on_uart_rx()
{
  read_pending();
}

void init_uart()
{
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  uint baud = uart_init(UART_ID, 115200);

  uart_set_hw_flow(UART_ID, false, false);

  uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);

  uart_set_fifo_enabled(UART_ID, true);
  printf("baud rate %u\n", baud);
  irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
  irq_set_enabled(UART_IRQ, true);

  uart_set_irq_enables(UART_ID, true, false);
}

void send_uart_kb_report(hid_keyboard_report_t *report)
{
  printf("send on uart\n");
  uint8_t buf[32];
  uint8_t ptr = 0;
  buf[ptr++] = SENTINEL;
  buf[ptr++] = MessageType::KEYBOARD;
  buf[ptr++] = report->modifier;
  for (int i = 0; i < 6; ++i)
  {
    buf[ptr++] = report->keycode[i];
  }
  buf[ptr++] = SENTINEL;
  uart_write_blocking(UART_ID, buf, ptr);
}

void uart_task()
{

}
#if 0
    if (got_chars > 0)
    {
      printf("got %d chars\n", got_chars);
      got_chars = 0;
    }
    while (uart_is_readable(UART_ID))
    {
      uint8_t ch = uart_getc(UART_ID);
      if (ch == '$')
      {
        ++ndollars;
      }
      else if (ndollars == 2)
      {
        ubuf[uptr++] = ch;
        if (uptr == 7)
        {
          printf("got whole message\n");
          hid_keyboard_report_t report;
          report.modifier = ubuf[0];
          for (int i = 0; i < 6; ++i)
            report.keycode[i] = ubuf[i + 1];
          print_kbd_report(&report);
          tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report.modifier, report.keycode);

          ndollars = 0;
          uptr = 0;
        }
      }
      else
      {
        ndollars = 0;
        uptr = 0;
      }
    }
#endif