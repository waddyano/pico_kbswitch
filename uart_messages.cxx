#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/critical_section.h"

#include "common.h"
#include "cppcrc.h"
#include "tusb.h"
#include "uart_messages.h"
#include "usb_descriptors.h"

#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_IRQ UART0_IRQ 

const uint8_t SENTINEL = 0x7e;
const uint8_t ESCAPE = 0x7d;

enum MessageType : uint8_t
{
  KEYBOARD,
  MOUSE,
  KEYBOARD_REPORT,
  CONNECTION_CHANGED,
  SET_OUTPUT_MASK,
  TICK
};

static critical_section rx_cs;
static const int RX_BUF_SIZE = 64;
static uint8_t rx_buf[RX_BUF_SIZE];
static volatile int rx_rptr;
static volatile int rx_wptr;
static volatile bool had_interrupt;

static void read_pending()
{
  critical_section_enter_blocking(&rx_cs);
  int wlimit = rx_rptr - 1;
  int orig_wptr = rx_wptr;
  if (wlimit < 0)
  {
    wlimit = RX_BUF_SIZE - 1;
  }
  while (uart_is_readable(UART_ID))
  {
    if (rx_wptr == wlimit)
    {
      printf("oh dear buffer collision wlimit %d orig %d\n", wlimit, orig_wptr);
      break;
    }
    uint8_t ch = uart_getc(UART_ID);
    rx_buf[rx_wptr++] = ch;
    if (rx_wptr == RX_BUF_SIZE)
    {
      rx_wptr = 0;
    }
  }
  critical_section_exit(&rx_cs);
}

static void on_uart_rx()
{
  had_interrupt = true;
  read_pending();
}

void init_uart()
{
  rx_rptr = 0;
  rx_wptr = 0;
  critical_section_init(&rx_cs);
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

template <int N>
class uart_buffer
{
public:
  void put_sentinel()
  {
    m_buf[m_ptr++] = SENTINEL;
  }
  void put(uint8_t b)
  {
    m_crc = CRC8::CRC8::calc(&b, 1, m_crc);
    putbyte(b);
  }
  void set_crc() 
  {
    putbyte(m_crc);
  }
  void send()
  {
    uart_write_blocking(UART_ID, m_buf, m_ptr);
  }
private:
  void putbyte(uint8_t b)
  {
    if (b == SENTINEL || b == ESCAPE)
    {
      m_buf[m_ptr++] = ESCAPE;
    }
    m_buf[m_ptr++] = b;
  }
  uint8_t m_crc = 0;
  uint8_t m_ptr = 0;
  uint8_t m_buf[N];
};

void send_uart_kb_report(const hid_keyboard_report_t *report)
{
  printf("send kb on uart\n");
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::KEYBOARD);
  b.put(report->modifier);
  for (int i = 0; i < 6; ++i)
  {
    b.put(report->keycode[i]);
  }
  b.set_crc();
  b.put_sentinel();
  b.send();
}

void send_uart_mouse_report(const hid_mouse_report_t *report)
{
  printf("send mouse on uart\n");
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::MOUSE);
  b.put(report->buttons);
  b.put(report->x);
  b.put(report->y);
  b.put(report->wheel);
  b.put(report->pan);
  b.set_crc();
  b.put_sentinel();
  b.send();
}

void send_uart_keyboard_report(uint8_t leds)
{
  printf("send kb report on uart\n");
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::KEYBOARD_REPORT);
  b.put(leds);
  b.set_crc();
  b.put_sentinel();
  b.send();
}

void send_uart_keyboard_connected(bool connected)
{
  printf("send kb connected %d on uart\n", connected);
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::CONNECTION_CHANGED);
  b.put(connected ? 1 : 0);
  b.set_crc();
  b.put_sentinel();
  b.send();
}

void send_uart_mouse_connected(bool connected)
{
  printf("send mouse connected %d on uart\n", connected);
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::CONNECTION_CHANGED);
  b.put(connected ? 3 : 2);
  b.set_crc();
  b.put_sentinel();
  b.send();
}

void send_uart_set_output_mask(uint8_t mask)
{
  printf("send output mask %u\n", mask);
  uart_buffer<32> b;
  b.put_sentinel();
  b.put(MessageType::SET_OUTPUT_MASK);
  b.put(mask);
  b.set_crc();
  b.put_sentinel();
  b.send();
}

static void print_pkt(const uint8_t *pbuf, int plen)
{
  printf("len=%d:", plen);
  for (int i = 0; i < plen; ++i)
  {
    printf(" %02x", pbuf[i]);
  }
  printf("\n");
}

static bool process_pkt(const uint8_t *pbuf, int plen)
{
  //printf("got packet type %d, len %d\n", pbuf[0], plen);
  if (plen == 0)
  {
    printf("empty packet\n");
    return false;
  }
  if (pbuf[0] == MessageType::KEYBOARD)
  {
    if (plen != 9)
    {
      printf("invalid kb packet %d\n", plen);
      return false;
    }
    uint8_t c = CRC8::CRC8::calc(pbuf, plen - 1);
    if (c != pbuf[8])
    {
      printf("bad kb crc %x != %x ptrs %d %d\n", c, pbuf[8], rx_rptr, rx_wptr);
      print_pkt(pbuf, plen);
      return false;
    }
    hid_keyboard_report_t report;
    report.modifier = pbuf[1];
    for (int i = 0; i < 6; i++)
    {
      report.keycode[i] = pbuf[i + 2];
    }
    if (should_output())
    {
      tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report.modifier, report.keycode);
      print_kbd_report(&report);
    }
    else
    {
      printf("dropped kb\n");
    }
    return true;
  }
  else if (pbuf[0] == MessageType::MOUSE)
  {
    if (plen != 7)
    {
      printf("invalid mouse packet %d\n", plen);
      return false;
    }
    uint8_t c = CRC8::CRC8::calc(pbuf, plen - 1);
    if (c != pbuf[6])
    {
      printf("bad mouse crc %x %d %d\n", c, rx_rptr, rx_wptr);
      print_pkt(pbuf, plen);
      return false;
    }
    hid_mouse_report_t report;
    report.buttons = pbuf[1];
    report.x = pbuf[2];
    report.y = pbuf[3];
    report.wheel = pbuf[4];
    report.pan = pbuf[5];
    if (should_output())
    {
      tud_hid_mouse_report(REPORT_ID_MOUSE, report.buttons, report.x, report.y, report.wheel, report.pan);
      print_mouse_report(&report);
    }
    else
    {
      printf("dropped mouse\n");
    }
    return true;
  }
  else if (pbuf[0] == MessageType::KEYBOARD_REPORT)
  {
    if (plen != 3)
    {
      printf("invalid kb report packet %d\n", plen);
      return false;
    }
    uint8_t c = CRC8::CRC8::calc(pbuf, plen - 1);
    if (c != pbuf[2])
    {
      printf(" bad kb report crc %x\n", c);
      return false;
    }
    static uint8_t leds;
    leds = pbuf[1];
    printf("got kb report %d via uart\n", leds);
    if (keyboard_dev_addr != NO_DEV)
    {
      tuh_hid_set_report(keyboard_dev_addr, keyboard_instance, 0, HID_REPORT_TYPE_OUTPUT, &leds, sizeof(leds));
    }
    return true;
  }
  else if (pbuf[0] == MessageType::CONNECTION_CHANGED)
  {
    if (plen != 3)
    {
      printf("invalid conn changed packet %d\n", plen);
      return false;
    }
    uint8_t c = CRC8::CRC8::calc(pbuf, plen - 1);
    if (c != pbuf[2])
    {
      printf(" bad conn changed crc %x\n", c);
      return false;
    }
    printf("got conn changed %d via uart\n", pbuf[1]);
    return true;
  }
  else if (pbuf[0] == MessageType::SET_OUTPUT_MASK)
  {
    if (plen != 3)
    {
      printf("invalid output mask packet %d\n", plen);
      return false;
    }
    uint8_t c = CRC8::CRC8::calc(pbuf, plen - 1);
    if (c != pbuf[2])
    {
      printf(" bad set output mask crc %x\n", c);
      return false;
    }
    printf("got set output mask %u via uart\n", pbuf[1]);
    set_current_output_mask(pbuf[1]);
    return true;
  }
  else
  {
    printf("unrecognised uart message %u\n", pbuf[0]);
    return false;
  }
}

void uart_task()
{
  had_interrupt = false;
  read_pending();
  if (rx_rptr == rx_wptr)
  {
    return;
  }

  int r = rx_rptr;
  int orig_r = r;
  int w = rx_wptr;
  bool in_pkt = false;
  uint8_t pbuf[32];
  int plen = 0;
  while (r != w)
  {
    //printf("buf[%d] = %x, r %d w %d\n", r, rx_buf[r], r, w);
    if (r > RX_BUF_SIZE)
    {
      printf("dearie me! r = %d w = %d\n", r, w);
      break;
    }
    if (in_pkt && rx_buf[r] == ESCAPE)
    {
      r++;
      if (r == RX_BUF_SIZE)
      {
        r = 0;
      }
      if (r == w)
      {
        break;
      }
      pbuf[plen++] = rx_buf[r];
      r++;
    }
    else if (rx_buf[r] == SENTINEL)
    {
      r++;
      if (r == RX_BUF_SIZE)
      {
        r = 0;
      }
      if (in_pkt)
      {
        int save_rx_rptr = rx_rptr;
        if (!process_pkt(pbuf, plen))
        {
          printf("process pkt failed r %d orig %d save_rx_rptr %d rx_rptr %d rx_wptr %d w %d plen %d\n", 
              r, orig_r, save_rx_rptr, rx_rptr, rx_wptr, w, plen);
          for (int i = 0; i < plen; ++i)
          {
            printf(" %02x", pbuf[i]);
          }
          printf("\nbuf\n");
          for (int i = 0; i < RX_BUF_SIZE / 32; ++i)
          {
            printf("%3d: ", i * 32);
            char line[128];
            int lptr = 0;
            for (int j = 0; j < 32; ++j)
            {
              static const char *hexdigits = "0123456789abcdef";
              uint8_t b = rx_buf[i * 32 + j];
              line[lptr++] = hexdigits[b >> 4];
              line[lptr++] = hexdigits[b & 0xf];
              line[lptr++] = ' ';
            }
            line[lptr++] = '\n';
            line[lptr++] = '\0';
            puts(line);
          }
        }
        else if (w == 0)
        {
          printf("process pkt ok r %d orig %d rx_rptr %d w %d plen %d\n", r, orig_r, save_rx_rptr, w, plen);
        }
        if (had_interrupt)
        {
          printf("had interrupt\n");
        }
        plen = 0;
        in_pkt = false;
        rx_rptr = r;
      }
      else
      {
        in_pkt = true;
      }
    }
    else if (in_pkt)
    {
      pbuf[plen++] = rx_buf[r++];
    }
    else
    {
      printf("drop byte %02x r %d w %d %d %d\n", rx_buf[r], r, w, rx_rptr, rx_wptr);
      r++; // drop
      if (r == RX_BUF_SIZE)
      {
        r = 0;
      }
      rx_rptr = r;
    }
    if (r == RX_BUF_SIZE)
    {
      r = 0;
    }
  }
}
