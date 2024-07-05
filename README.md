# Pico KB Switch

This is firmware for creating a keyboard and mouse switch using two raspberry pi Picos.
The original starting point was https://github.com/brendena/pico_device_and_host but the code
has evolved significantly.

## Design
This uses two raspberry pi picos connected together by uart0 each of which can act
as both a USB device and a USB host. Both run exactly the same firmware but one of them identifies itself 
by tying gpio 13 to ground. The other lets the internal pull up keep the same pin high.


## links
* [starting point](https://github.com/brendena/pico_device_and_host)
* [library](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
* [Host and device example](https://github.com/sekigon-gonnoc/Pico-PIO-USB/tree/main/examples/host_hid_to_device_cdc)
* [USB device](https://github.com/hathach/tinyusb/tree/master/examples/device/hid_composite)


