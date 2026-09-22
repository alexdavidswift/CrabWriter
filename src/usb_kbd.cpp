#include "usb_kbd.h"
#include "input.h"
#include "usb/usb_host.h"

// Everything below runs inside one FreeRTOS task, including the USB transfer
// callbacks (they fire from usb_host_client_handle_events), so no locking is
// needed for this state. Only s_status / s_connected are read elsewhere.

static bool s_started = false;
static volatile bool s_connected = false;
static char s_status[40] = "off";

static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t s_dev = nullptr;
static uint8_t s_pendingAddr = 0;
static bool s_devGone = false;
static uint32_t s_goneAt = 0;

static uint8_t s_ifNum = 0;
static uint8_t s_epIn = 0;
static uint16_t s_mps = 8;
static bool s_claimed = false;

static usb_transfer_t* s_in = nullptr;
static usb_transfer_t* s_ctrl = nullptr;
static bool s_inFlight = false;
static bool s_ctrlBusy = false;
static bool s_inRetry = false;
static uint32_t s_inRetryAt = 0;

enum Stage { ST_IDLE, ST_SET_PROTOCOL, ST_SET_IDLE, ST_RUNNING };
static Stage s_stage = ST_IDLE;

// Keyboard state
static uint8_t s_prev[6] = {0};
static uint8_t s_mods = 0;
static bool s_caps = false;
static uint8_t s_repeatUsage = 0;
static uint32_t s_repeatAt = 0;
static bool s_ledDirty = false;

static const uint32_t REPEAT_DELAY_MS = 420;
static const uint32_t REPEAT_RATE_MS = 40;

static void setStatus(const char* s) { strlcpy(s_status, s, sizeof(s_status)); }

// ------------------------------------------------------ usage translation --

static bool usageToEvent(uint8_t u, uint8_t hidMods, KeyEvent& ev) {
  ev = KeyEvent();
  ev.src = SRC_USB;
  bool ctrl = hidMods & 0x11, shift = hidMods & 0x22, alt = hidMods & 0x44;
  ev.mods = (ctrl ? MOD_CTRL : 0) | (shift ? MOD_SHIFT : 0) | (alt ? MOD_ALT : 0);
  bool plainShift = shift && !ctrl && !alt;

  static const char kNum[] = "1234567890";
  static const char kNumS[] = "!@#$%^&*()";
  static const char kSym[] = "-=[]\\#;'`,./";
  static const char kSymS[] = "_+{}|~:\"~<>?";
  static const char kPad[] = "/*-+\n1234567890.";

  if (u >= 0x04 && u <= 0x1D) {
    char c = 'a' + (u - 0x04);
    bool upper = (plainShift != s_caps) && !ctrl && !alt;
    ev.key = K_CHAR;
    ev.ch = upper ? c - 32 : c;
    return true;
  }
  if (u >= 0x1E && u <= 0x27) {
    ev.key = K_CHAR;
    ev.ch = plainShift ? kNumS[u - 0x1E] : kNum[u - 0x1E];
    return true;
  }
  if (u >= 0x2D && u <= 0x38) {
    ev.key = K_CHAR;
    ev.ch = plainShift ? kSymS[u - 0x2D] : kSym[u - 0x2D];
    return true;
  }
  if (u >= 0x54 && u <= 0x63) {
    char c = kPad[u - 0x54];
    if (c == '\n') { ev.key = K_ENTER; return true; }
    ev.key = K_CHAR;
    ev.ch = c;
    return true;
  }
  switch (u) {
    case 0x28: ev.key = K_ENTER; return true;
    case 0x29: ev.key = K_ESC; return true;
    case 0x2A: ev.key = K_BACKSPACE; return true;
    case 0x2B: ev.key = K_TAB; return true;
    case 0x2C: ev.key = K_CHAR; ev.ch = ' '; return true;
    case 0x4A: ev.key = K_HOME; return true;
    case 0x4B: ev.key = K_PGUP; return true;
    case 0x4C: ev.key = K_DELETE; return true;
    case 0x4D: ev.key = K_END; return true;
    case 0x4E: ev.key = K_PGDN; return true;
    case 0x4F: ev.key = K_RIGHT; return true;
    case 0x50: ev.key = K_LEFT; return true;
    case 0x51: ev.key = K_DOWN; return true;
    case 0x52: ev.key = K_UP; return true;
    case 0x64: ev.key = K_CHAR; ev.ch = plainShift ? '|' : '\\'; return true;
    default: return false;
  }
}

static void handleReport(const uint8_t* r, int len) {
  if (len < 3) return;
  uint8_t mods = r[0];
  const uint8_t* keys = r + 2;
  int nkeys = len - 2 > 6 ? 6 : len - 2;
  if (keys[0] == 0x01) return;  // phantom state / rollover error

  uint32_t now = millis();
  for (int i = 0; i < nkeys; i++) {
    uint8_t u = keys[i];
    if (u < 0x04) continue;
    bool wasDown = false;
    for (int j = 0; j < 6; j++) if (s_prev[j] == u) wasDown = true;
    if (wasDown) continue;
    if (u == 0x39) {  // Caps Lock
      s_caps = !s_caps;
      s_ledDirty = true;
      continue;
    }
    KeyEvent ev;
    if (usageToEvent(u, mods, ev)) {
      inputPush(ev);
      s_repeatUsage = u;
      s_repeatAt = now + REPEAT_DELAY_MS;
    }
  }
  // Stop repeating once the key is released.
  bool held = false;
  for (int i = 0; i < nkeys; i++) if (keys[i] == s_repeatUsage) held = true;
  if (!held) s_repeatUsage = 0;

  memset(s_prev, 0, sizeof(s_prev));
  memcpy(s_prev, keys, nkeys);
  s_mods = mods;
}

static void serviceRepeat() {
  if (!s_repeatUsage || !s_connected) return;
  uint32_t now = millis();
  if ((int32_t)(now - s_repeatAt) < 0) return;
  KeyEvent ev;
  if (usageToEvent(s_repeatUsage, s_mods, ev)) inputPush(ev);
  s_repeatAt = now + REPEAT_RATE_MS;
}

// --------------------------------------------------------------- transfers --

static void submitIn();

static void inCb(usb_transfer_t* t) {
  s_inFlight = false;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    handleReport(t->data_buffer, t->actual_num_bytes);
    if (!s_devGone) submitIn();
  } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
             t->status != USB_TRANSFER_STATUS_CANCELED && !s_devGone) {
    // Transient error or stall: try again shortly.
    s_inRetry = true;
    s_inRetryAt = millis() + 20;
  }
}

static void submitIn() {
  if (!s_in || s_inFlight || s_devGone) return;
  s_in->device_handle = s_dev;
  s_in->bEndpointAddress = s_epIn;
  s_in->num_bytes = s_mps;
  s_in->callback = inCb;
  s_in->context = nullptr;
  s_in->timeout_ms = 0;
  if (usb_host_transfer_submit(s_in) == ESP_OK) s_inFlight = true;
}

static void ctrlCb(usb_transfer_t* t);

// Class request to the keyboard interface (host-to-device).
static bool ctrlOut(uint8_t bRequest, uint16_t wValue, const uint8_t* data, uint16_t len) {
  if (!s_ctrl || s_ctrlBusy || s_devGone) return false;
  usb_setup_packet_t* setup = (usb_setup_packet_t*)s_ctrl->data_buffer;
  setup->bmRequestType = 0x21;  // class, interface, host-to-device
  setup->bRequest = bRequest;
  setup->wValue = wValue;
  setup->wIndex = s_ifNum;
  setup->wLength = len;
  if (len) memcpy(s_ctrl->data_buffer + sizeof(usb_setup_packet_t), data, len);
  s_ctrl->num_bytes = sizeof(usb_setup_packet_t) + len;
  s_ctrl->device_handle = s_dev;
  s_ctrl->bEndpointAddress = 0;
  s_ctrl->callback = ctrlCb;
  s_ctrl->context = nullptr;
  s_ctrl->timeout_ms = 1000;
  if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return false;
  s_ctrlBusy = true;
  return true;
}

static void advanceSetup() {
  // Failures (e.g. a keyboard that STALLs SET_IDLE) are not fatal; keep going.
  if (s_stage == ST_SET_PROTOCOL) {
    s_stage = ST_SET_IDLE;
    if (ctrlOut(0x0A /*SET_IDLE*/, 0, nullptr, 0)) return;
  }
  if (s_stage == ST_SET_IDLE) {
    s_stage = ST_RUNNING;
    s_connected = true;
    setStatus("keyboard connected");
    s_ledDirty = true;
    submitIn();
  }
}

static void ctrlCb(usb_transfer_t*) {
  s_ctrlBusy = false;
  if (s_stage != ST_RUNNING) advanceSetup();
}

// ----------------------------------------------------------- device setup --

static void closeDevice() {
  if (s_claimed) usb_host_interface_release(s_client, s_dev, s_ifNum);
  s_claimed = false;
  if (s_in) { usb_host_transfer_free(s_in); s_in = nullptr; }
  if (s_ctrl) { usb_host_transfer_free(s_ctrl); s_ctrl = nullptr; }
  if (s_dev) usb_host_device_close(s_client, s_dev);
  s_dev = nullptr;
  s_inFlight = s_ctrlBusy = s_inRetry = false;
  s_stage = ST_IDLE;
  s_connected = false;
  s_repeatUsage = 0;
  memset(s_prev, 0, sizeof(s_prev));
}

static void openDevice(uint8_t addr) {
  if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
    s_dev = nullptr;
    setStatus("device open failed");
    return;
  }
  const usb_config_desc_t* cd;
  if (usb_host_get_active_config_descriptor(s_dev, &cd) != ESP_OK) {
    closeDevice();
    setStatus("no config descriptor");
    return;
  }

  // Walk the descriptors looking for a HID keyboard interface (class 3,
  // protocol 1) and its interrupt IN endpoint. Prefer the boot subclass.
  const uint8_t* p = (const uint8_t*)cd;
  int total = cd->wTotalLength;
  int bestScore = -1;
  uint8_t curIf = 0, curAlt = 0;
  int curScore = -1;
  uint8_t foundIf = 0, foundAlt = 0, foundEp = 0;
  uint16_t foundMps = 8;
  for (int i = 0; i + 1 < total && p[i] > 0; i += p[i]) {
    uint8_t type = p[i + 1];
    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[i] >= 9) {
      curIf = p[i + 2];
      curAlt = p[i + 3];
      uint8_t cls = p[i + 5], sub = p[i + 6], proto = p[i + 7];
      curScore = (cls == 0x03 && proto == 0x01) ? (sub == 0x01 ? 2 : 1) : -1;
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && curScore > bestScore && p[i] >= 7) {
      uint8_t ea = p[i + 2];
      if ((ea & 0x80) && (p[i + 3] & 0x03) == 3) {  // interrupt IN
        bestScore = curScore;
        foundIf = curIf;
        foundAlt = curAlt;
        foundEp = ea;
        foundMps = (p[i + 4] | (p[i + 5] << 8)) & 0x7FF;
      }
    }
  }
  if (bestScore < 0) {
    closeDevice();
    setStatus("not a keyboard");
    return;
  }
  if (usb_host_interface_claim(s_client, s_dev, foundIf, foundAlt) != ESP_OK) {
    closeDevice();
    setStatus("claim failed");
    return;
  }
  s_claimed = true;
  s_ifNum = foundIf;
  s_epIn = foundEp;
  s_mps = foundMps < 8 ? 8 : foundMps;
  if (usb_host_transfer_alloc(s_mps, 0, &s_in) != ESP_OK ||
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 8, 0, &s_ctrl) != ESP_OK) {
    closeDevice();
    setStatus("alloc failed");
    return;
  }
  s_devGone = false;
  setStatus("configuring...");
  // Ask for the simple 8-byte boot report format.
  s_stage = ST_SET_PROTOCOL;
  if (!ctrlOut(0x0B /*SET_PROTOCOL*/, 0 /*boot*/, nullptr, 0)) advanceSetup();
}

static void clientCb(const usb_host_client_event_msg_t* msg, void*) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    s_pendingAddr = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (!s_devGone) s_goneAt = millis();
    s_devGone = true;
    s_connected = false;
    setStatus("keyboard unplugged");
  }
}

static void usbTask(void*) {
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(pdMS_TO_TICKS(2), &flags);
    usb_host_client_handle_events(s_client, pdMS_TO_TICKS(2));

    if (s_devGone && s_dev) {
      // Wait for outstanding transfers to be returned before tearing down
      // (with a safety timeout).
      if ((!s_inFlight && !s_ctrlBusy) || millis() - s_goneAt > 500) {
        closeDevice();
        s_devGone = false;
        setStatus("waiting for keyboard");
      }
    }
    if (s_pendingAddr && !s_dev) {
      uint8_t a = s_pendingAddr;
      s_pendingAddr = 0;
      openDevice(a);
    }
    if (s_inRetry && (int32_t)(millis() - s_inRetryAt) >= 0) {
      s_inRetry = false;
      submitIn();
    }
    if (s_ledDirty && s_stage == ST_RUNNING && !s_ctrlBusy) {
      uint8_t leds = s_caps ? 0x02 : 0x00;
      if (ctrlOut(0x09 /*SET_REPORT*/, 0x0200 /*output report*/, &leds, 1)) s_ledDirty = false;
    }
    serviceRepeat();
  }
}

bool usbKbdStart() {
  if (s_started) return true;
  const usb_host_config_t hostCfg = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  if (usb_host_install(&hostCfg) != ESP_OK) {
    setStatus("host install failed");
    return false;
  }
  const usb_host_client_config_t clientCfg = {
    .is_synchronous = false,
    .max_num_event_msg = 8,
    .async = {.client_event_callback = clientCb, .callback_arg = nullptr},
  };
  if (usb_host_client_register(&clientCfg, &s_client) != ESP_OK) {
    setStatus("client register failed");
    return false;
  }
  s_started = true;
  setStatus("waiting for keyboard");
  xTaskCreatePinnedToCore(usbTask, "usbkbd", 4096, nullptr, 5, nullptr, 0);
  return true;
}

bool usbKbdStarted() { return s_started; }
bool usbKbdConnected() { return s_connected; }
const char* usbKbdStatus() { return s_status; }
