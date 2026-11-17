#define M5LINK_USB_HOST_IMPL  // compiled into every application; empty without USB-OTG
#include "UsbHost.h"

#if CONFIG_USB_OTG_SUPPORTED

#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <soc/usb_struct.h>
#include <stdarg.h>

// CDC-ACM (USB CDC PSTN subclass) values
static const uint8_t CLASS_CDC = 0x02, SUBCLASS_ACM = 0x02, CLASS_CDC_DATA = 0x0A;
static const uint8_t SET_LINE_CODING = 0x20, SET_CONTROL_LINE_STATE = 0x22;
static const uint8_t CLASS_INTERFACE_OUT = 0x21;  // bmRequestType: host to device, class, interface

static const size_t IN_SIZE = 512;   // a multiple of the packet size, which is 64 at full speed
static const size_t OUT_SIZE = 512;
static const uint32_t RETRY_MS = 100;
static const uint32_t FLUSH_TIMEOUT_MS = 1000;

// ---- Errors from the USB Host Library
// ESP-IDF 4.4's hub driver logs why an enumeration failed (e.g. "Stage failed: GET_FULL_CONFIG_DESC") but tells
// clients nothing, so its error lines are copied from the log output, which still goes where it went before.

static const char LOG_PREFIX[] = "E (%u) %s: ";  // ESP_LOGE's format without log colours, as in Arduino core 2.0.17
static const int LOG_LINES = 4;
static const int LOG_LEN = 96;
static const size_t MESSAGE_MAX = 320;
static portMUX_TYPE logLock = portMUX_INITIALIZER_UNLOCKED;
static char logLines[LOG_LINES][LOG_LEN];
static uint32_t logCount = 0;  // lines captured since boot
static vprintf_like_t previousVprintf = nullptr;

static bool usbTag(const char* tag) {
  return tag && (!strcmp(tag, "HUB") || !strcmp(tag, "USBH") || !strcmp(tag, "HCD DWC") || !strcmp(tag, "USB HOST"));
}

static int captureLog(const char* fmt, va_list args) {
  if (fmt && !strncmp(fmt, LOG_PREFIX, sizeof LOG_PREFIX - 1)) {
    va_list copy;
    va_copy(copy, args);
    (void)va_arg(copy, unsigned);  // timestamp
    const char* tag = va_arg(copy, const char*);
    if (usbTag(tag)) {
      char line[LOG_LEN];
      vsnprintf(line, sizeof line, fmt + sizeof LOG_PREFIX - 1, copy);
      line[strcspn(line, "\r\n")] = 0;
      portENTER_CRITICAL(&logLock);
      memcpy(logLines[logCount % LOG_LINES], line, LOG_LEN);
      logCount++;
      portEXIT_CRITICAL(&logLock);
    }
    va_end(copy);
  }
  return previousVprintf ? previousVprintf(fmt, args) : vprintf(fmt, args);  // null only while begin() installs the hook
}

// ---- Ring buffer

size_t UsbHostSerial::Ring::push(const uint8_t* data, size_t n) {
  size_t done = 0;
  while (done < n && len < size) {
    size_t end = (start + len) % size;
    size_t chunk = min(n - done, min(size - len, size - end));
    memcpy(buf + end, data + done, chunk);
    len += chunk;
    done += chunk;
  }
  return done;
}

size_t UsbHostSerial::Ring::pop(uint8_t* data, size_t n) {
  size_t done = 0;
  while (done < n && len) {
    size_t chunk = min(n - done, min(len, size - start));
    if (data) memcpy(data + done, buf + start, chunk);
    start = (start + chunk) % size;
    len -= chunk;
    done += chunk;
  }
  return done;
}

// ---- Serial

void UsbHostSerial::setBaud(uint32_t baud) {
  lineBaud = baud;
  sendCoding = ready;
  if (host) host->kickControl();
}

void UsbHostSerial::setDtr(bool on) {
  lineDtr = on;
  sendState = ready;
  if (host) host->kickControl();
}

void UsbHostSerial::setRts(bool on) {
  lineRts = on;
  sendState = ready;
  if (host) host->kickControl();
}

int UsbHostSerial::available() {
  if (host) host->pump();
  return rx.len;
}

int UsbHostSerial::read() {
  if (host) host->pump();
  uint8_t c;
  return rx.pop(&c, 1) ? c : -1;
}

int UsbHostSerial::peek() {
  if (host) host->pump();
  return rx.len ? rx.buf[rx.start] : -1;
}

size_t UsbHostSerial::write(const uint8_t* data, size_t n) {
  if (!ready) return 0;
  size_t queued = tx.push(data, n);
  kickOut();
  return queued;
}

int UsbHostSerial::availableForWrite() { return ready ? tx.size - tx.len : 0; }

void UsbHostSerial::flush() {
  uint32_t start = millis();
  while (ready && (tx.len || outBusy) && millis() - start < FLUSH_TIMEOUT_MS) {
    host->pump();
    delay(1);
  }
}

// Finds the CDC-ACM interfaces, claims the data interface, and starts reading.
void UsbHostSerial::attach(const usb_config_desc_t* config) {
  int offset = 0;
  const usb_standard_desc_t* d = (const usb_standard_desc_t*)config;
  int control = -1, data = -1;
  uint8_t foundIn = 0, foundOut = 0;
  uint16_t foundInSize = 64;
  // Interface being walked
  int number = -1;
  uint8_t cls = 0, alt = 0, candIn = 0, candOut = 0;
  uint16_t candInSize = 64;
  for (;;) {
    d = usb_parse_next_descriptor(d, config->wTotalLength, &offset);
    bool end = d == nullptr;
    if (end || d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      // Close the previous interface: the first data interface with bulk IN and OUT wins
      if (data < 0 && number >= 0 && cls == CLASS_CDC_DATA && alt == 0 && candIn && candOut) {
        data = number;
        foundIn = candIn;
        foundOut = candOut;
        foundInSize = candInSize;
      }
      if (end) break;
      const usb_intf_desc_t* intf = (const usb_intf_desc_t*)d;
      number = intf->bInterfaceNumber;
      cls = intf->bInterfaceClass;
      alt = intf->bAlternateSetting;
      candIn = candOut = 0;
      if (control < 0 && cls == CLASS_CDC && intf->bInterfaceSubClass == SUBCLASS_ACM && alt == 0) control = number;
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cls == CLASS_CDC_DATA && alt == 0) {
      const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
      if ((ep->bmAttributes & 0x03) != 0x02) continue;  // bulk only
      if (ep->bEndpointAddress & 0x80) {
        candIn = ep->bEndpointAddress;
        candInSize = ep->wMaxPacketSize & 0x7FF;
      } else {
        candOut = ep->bEndpointAddress;
      }
    }
  }
  if (control < 0 || data < 0) return;  // not CDC-ACM
  if (usb_host_interface_claim(host->client, host->dev, data, 0) != ESP_OK) return;
  claimed = true;
  dataInterface = data;
  size_t inSize = foundInSize ? IN_SIZE / foundInSize * foundInSize : IN_SIZE;
  if (usb_host_transfer_alloc(inSize, 0, &in) != ESP_OK || usb_host_transfer_alloc(OUT_SIZE, 0, &out) != ESP_OK) {
    release();
    return;
  }
  controlInterface = control;
  epIn = foundIn;
  epOut = foundOut;
  inLength = inSize;
  for (usb_transfer_t* t : {in, out}) {
    t->device_handle = host->dev;
    t->context = this;
    t->timeout_ms = 0;
  }
  in->bEndpointAddress = epIn;
  in->callback = inDone;
  out->bEndpointAddress = epOut;
  out->callback = outDone;
  rx.start = rx.len = tx.start = tx.len = 0;
  ready = true;
  sendCoding = sendState = true;  // line coding first, then DTR/RTS, as a computer does
  submitIn();
}

void UsbHostSerial::detach() {
  if (!in && !out) return;
  ready = false;
  sendCoding = sendState = false;
  tx.start = tx.len = 0;
  inRetry = false;
  // Retire transfers still in flight; their callbacks arrive through pump()
  for (uint8_t ep : {epIn, epOut}) {
    usb_host_endpoint_halt(host->dev, ep);
    usb_host_endpoint_flush(host->dev, ep);
  }
}

// Frees the transfers and the interface once nothing is in flight.
void UsbHostSerial::release() {
  if (in) usb_host_transfer_free(in);
  if (out) usb_host_transfer_free(out);
  in = out = nullptr;
  if (claimed) usb_host_interface_release(host->client, host->dev, dataInterface);
  claimed = false;
}

void UsbHostSerial::submitIn() {
  if (!ready || inBusy) return;
  in->num_bytes = inLength;
  if (usb_host_transfer_submit(in) == ESP_OK) {
    inBusy = true;
  } else {
    inRetry = true;
    inRetryAt = millis() + RETRY_MS;
  }
}

void UsbHostSerial::kickOut() {
  if (!ready || outBusy || !tx.len) return;
  size_t n = tx.pop(out->data_buffer, out->data_buffer_size);
  out->num_bytes = n;
  if (usb_host_transfer_submit(out) == ESP_OK) outBusy = true;  // on failure the bytes are lost
}

void UsbHostSerial::service() {
  if (inRetry && (int32_t)(millis() - inRetryAt) >= 0) {
    inRetry = false;
    submitIn();
  }
  kickOut();
}

void UsbHostSerial::inDone(usb_transfer_t* t) {
  UsbHostSerial* self = (UsbHostSerial*)t->context;
  self->inBusy = false;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    size_t n = t->actual_num_bytes;
    self->droppedBytes += n - self->rx.push(t->data_buffer, n);
    self->submitIn();
  } else if (self->ready) {
    // Errors and stalls: try again later rather than spinning inside the event handler
    self->inRetry = true;
    self->inRetryAt = millis() + RETRY_MS;
  }
}

void UsbHostSerial::outDone(usb_transfer_t* t) {
  UsbHostSerial* self = (UsbHostSerial*)t->context;
  self->outBusy = false;
  self->kickOut();
}

// ---- Host

static void hostDaemon(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);  // enumeration and the library's own work
  }
}

UsbHost::UsbHost(size_t rxBufferSize, size_t txBufferSize) : rxSize(rxBufferSize), txSize(txBufferSize) {
  cdc.host = this;
}

bool UsbHost::begin() {
  if (begun) return true;
#if defined(ARDUINO_USB_MODE) && !ARDUINO_USB_MODE && \
    (ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT)
  return false;  // TinyUSB took the port before setup()
#else
  if (!claimUsbRole(UsbRole::Host)) return false;
  if (!previousVprintf) previousVprintf = esp_log_set_vprintf(captureLog);
  cdc.rx.buf = (uint8_t*)malloc(rxSize);
  cdc.tx.buf = (uint8_t*)malloc(txSize);
  if (!cdc.rx.buf || !cdc.tx.buf) return false;
  cdc.rx.size = rxSize;
  cdc.tx.size = txSize;

  usb_host_config_t config;
  memset(&config, 0, sizeof config);
  config.skip_phy_setup = false;  // the library switches the internal PHY to USB-OTG in host mode
  config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&config) != ESP_OK) return false;
  if (xTaskCreate(hostDaemon, "usb_host", 4096, nullptr, 2, nullptr) != pdPASS) return false;

  usb_host_client_config_t clientConfig;
  memset(&clientConfig, 0, sizeof clientConfig);
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = clientEvent;
  clientConfig.async.callback_arg = this;
  if (usb_host_client_register(&clientConfig, &client) != ESP_OK) return false;
  if (usb_host_transfer_alloc(64, 0, &control) != ESP_OK) return false;
  control->context = this;
  control->callback = controlDone;
  control->bEndpointAddress = 0;
  begun = true;
  return true;
#endif
}

// Runs client events (attach, detach, transfer callbacks) on the caller's task, so no locks are needed.
void UsbHost::pump() {
  if (!begun) return;
  usb_host_client_handle_events(client, 0);
  if (devGone) {
    devGone = false;
    gone();
  }
  if (newAddress) {
    uint8_t address = newAddress;
    newAddress = 0;
    if (!dev) open(address);  // otherwise openWaiting() picks it up after the close
  }
  if (closing) {
    finishClose();
  } else if (dev) {
    cdc.service();
    kickControl();
  }
}

UsbHost::Port UsbHost::port() const {
  Port p;
  if (!begun) return p;
  uint32_t hprt = USB0.hprt;  // DWC OTG host port register; reading it has no side effects
  p.connected = hprt & (1u << 0);
  p.enabled = hprt & (1u << 2);
  p.dPlus = hprt & (1u << 10);
  p.dMinus = hprt & (1u << 11);
  if (p.connected) {
    static const UsbSpeed SPEEDS[] = {UsbSpeed::High, UsbSpeed::Full, UsbSpeed::Low, UsbSpeed::Unknown};
    p.speed = SPEEDS[(hprt >> 17) & 3];
  }
  return p;
}

void UsbHost::poll() {
  pump();
  if (!begun) return;
  Port p = port();
  bool connectedNow = p.connected || dev;  // an attached device counts even if the register blinks
  if (connectedNow && !wasConnected) {
    wasConnected = true;
    connectedAt = millis();
    if (connectCallback) connectCallback(p.speed);
  }
  if (detachEvent) {
    detachEvent = false;
    if (detachCallback) detachCallback(goneInfo);
  }
  if (attachEvent) {
    attachEvent = false;
    failed = false;
    if (attachCallback) attachCallback(info);
  }
  if (!connectedNow && wasConnected) {
    wasConnected = false;
    failed = false;
    lastFailure = EnumerationFailure();
    portENTER_CRITICAL(&logLock);
    logSeen = logCount;  // errors from here on belong to the next connection
    portEXIT_CRITICAL(&logLock);
    if (disconnectCallback) disconnectCallback();
  }
  if (wasConnected && !dev && !failed) watchPort();
  if (failedEvent) {
    failedEvent = false;
    if (failedCallback) failedCallback(lastFailure);
  }
}

// A connected device that is not attached: collects the library's error lines and decides whether it failed.
void UsbHost::watchPort() {
  char lines[LOG_LINES][LOG_LEN];
  portENTER_CRITICAL(&logLock);
  uint32_t count = logCount;
  memcpy(lines, logLines, sizeof lines);
  portEXIT_CRITICAL(&logLock);
  if (count - logSeen > (uint32_t)LOG_LINES) logSeen = count - LOG_LINES;  // older lines were overwritten
  bool definite = false;
  for (; logSeen != count; logSeen++) {
    const char* line = lines[logSeen % LOG_LINES];
    if (lastFailure.message.length() + strlen(line) < MESSAGE_MAX) {
      if (lastFailure.message.length()) lastFailure.message += "; ";
      lastFailure.message += line;
    }
    static const char STAGE[] = "Stage failed: ";
    if (!strncmp(line, STAGE, sizeof STAGE - 1)) {
      lastFailure.stage = line + sizeof STAGE - 1;
      definite = true;
    } else if (strstr(line, "reset failed")) {
      definite = true;
    }
  }
  bool late = millis() - connectedAt >= enumTimeout;
  if (!definite && !late) return;
  lastFailure.speed = port().speed;
  lastFailure.timedOut = !definite;
  failed = true;
  failedEvent = true;
}

static String ascii(const usb_str_desc_t* desc) {
  String s;
  if (!desc) return s;
  int n = (desc->bLength - 2) / 2;
  for (int i = 0; i < n; i++) {
    uint16_t c = desc->wData[i];
    s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
  }
  return s;
}

void UsbHost::open(uint8_t address) {
  if (dev) return;  // one device at a time
  if (usb_host_device_open(client, address, &dev) != ESP_OK) {
    dev = nullptr;
    return;
  }
  info = Device();
  const usb_device_desc_t* desc;
  if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK) {
    info.vid = desc->idVendor;
    info.pid = desc->idProduct;
    info.deviceClass = desc->bDeviceClass;
  }
  usb_device_info_t devInfo;
  if (usb_host_device_info(dev, &devInfo) == ESP_OK) {
    info.manufacturer = ascii(devInfo.str_desc_manufacturer);
    info.product = ascii(devInfo.str_desc_product);
    info.serialNumber = ascii(devInfo.str_desc_serial_num);
  }
  const usb_config_desc_t* config;
  if (usb_host_get_active_config_descriptor(dev, &config) == ESP_OK) cdc.attach(config);
  info.cdcAcm = (bool)cdc;
  attachEvent = true;
}

void UsbHost::gone() {
  if (!dev) return;
  closing = true;
  goneInfo = info;
  info = Device();
  if (attachEvent) {
    attachEvent = false;  // attached and left between two polls: report neither
  } else {
    detachEvent = true;
  }
  cdc.detach();
}

// Closes the device once its transfers have come back.
void UsbHost::finishClose() {
  if (!cdc.idle() || controlBusy) return;
  cdc.release();
  usb_host_device_close(client, dev);
  dev = nullptr;
  closing = false;
  openWaiting();
}

// Opens a device that attached while the previous one was still closing.
void UsbHost::openWaiting() {
  uint8_t addresses[4];
  int n = 0;
  if (usb_host_device_addr_list_fill(sizeof addresses, addresses, &n) != ESP_OK) return;
  for (int i = 0; i < n && !dev; i++) open(addresses[i]);
}

void UsbHost::kickControl() {
  if (!dev || closing || controlBusy || !cdc.ready) return;
  usb_setup_packet_t* setup = (usb_setup_packet_t*)control->data_buffer;
  setup->bmRequestType = CLASS_INTERFACE_OUT;
  setup->wIndex = cdc.controlInterface;
  if (cdc.sendCoding) {
    cdc.sendCoding = false;
    uint8_t* coding = control->data_buffer + sizeof(usb_setup_packet_t);
    uint32_t baud = cdc.lineBaud;
    for (int i = 0; i < 4; i++) coding[i] = baud >> (8 * i);
    coding[4] = 0;  // 1 stop bit
    coding[5] = 0;  // no parity
    coding[6] = 8;  // data bits
    setup->bRequest = SET_LINE_CODING;
    setup->wValue = 0;
    setup->wLength = 7;
  } else if (cdc.sendState) {
    cdc.sendState = false;
    setup->bRequest = SET_CONTROL_LINE_STATE;
    setup->wValue = (cdc.lineDtr ? 1 : 0) | (cdc.lineRts ? 2 : 0);
    setup->wLength = 0;
  } else {
    return;
  }
  control->num_bytes = sizeof(usb_setup_packet_t) + setup->wLength;
  control->device_handle = dev;
  if (usb_host_transfer_submit_control(client, control) == ESP_OK) controlBusy = true;
}

void UsbHost::controlDone(usb_transfer_t* t) {
  UsbHost* self = (UsbHost*)t->context;
  self->controlBusy = false;
  self->kickControl();
}

void UsbHost::clientEvent(const usb_host_client_event_msg_t* msg, void* arg) {
  UsbHost* self = (UsbHost*)arg;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    self->newAddress = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == self->dev) {
    self->devGone = true;
  }
}

#endif
