#include <M5Cardputer.h>
#include "usb_switch_hid.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    amiibo::AmiiboTag demo_tag;
    demo_tag.loadDemoBlankTag();

    auto& usb = amiibo::SwitchUsbTransport::instance();
    usb.loadAndArmTag(demo_tag, true);
    usb.begin();
}

void loop() {
    M5Cardputer.update();
    delay(16);
}
