// https://documentation.espressif.com/esp32-c3_technical_reference_manual_en.pdf

#define LOW_POWER_MANAGEMENT 0x60008000
#define RTC_CNTL_WDTCONFIG0_REG (*(volatile uint32_t*)(LOW_POWER_MANAGEMENT + 0x0090))
#define RTC_CNTL_WDTWPROTECT_REG (*(volatile uint32_t*)(LOW_POWER_MANAGEMENT + 0x00A8))
#define RTC_CNTL_WDT_WKEY 0x50D83AA1

#define TIMER_GROUP_0 0x6001F000
#define TIMG_WDTCONFIG0_REG (*(volatile uint32_t*)(TIMER_GROUP_0 + 0x0048))
#define TIMG_WDTWPROTECT_REG (*(volatile uint32_t*)(TIMER_GROUP_0 + 0x0064))
#define TIMG_WDT_WKEY 0x50D83AA1

static void disable_watchdogs(void) {
    RTC_CNTL_WDTWPROTECT_REG = RTC_CNTL_WDT_WKEY;
    RTC_CNTL_WDTCONFIG0_REG = 0;

    TIMG_WDTWPROTECT_REG = TIMG_WDT_WKEY;
    TIMG_WDTCONFIG0_REG = 0;
}

#define USB_SERIAL_JTAG 0x60043000

#define USB_SERIAL_JTAG_EP1_REG (*(volatile uint32_t*)(USB_SERIAL_JTAG + 0x0000))
#define USB_SERIAL_JTAG_EP1_CONF_REG (*(volatile uint32_t*)(USB_SERIAL_JTAG + 0x0004))

#define USB_SERIAL_JTAG_WR_DONE 0x1
#define USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE 0x2
#define USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL 0x4

static uint8_t serial_read_byte(void) {
    while (!(USB_SERIAL_JTAG_EP1_CONF_REG & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL)) {
        // wait until data becomes available
    }
    return (uint8_t)USB_SERIAL_JTAG_EP1_REG;
}

static void serial_write_byte(uint8_t byte) {
    while (!(USB_SERIAL_JTAG_EP1_CONF_REG & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE)) {
        // wait until there is room for data
    }
    USB_SERIAL_JTAG_EP1_REG = byte;
    USB_SERIAL_JTAG_EP1_CONF_REG = USB_SERIAL_JTAG_WR_DONE;
}
