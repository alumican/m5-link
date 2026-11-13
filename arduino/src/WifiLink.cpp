#include "WifiLink.h"

#include <Preferences.h>
#include <WiFi.h>

static const char KEY_SSID[] = "ssid";
static const char KEY_PASS[] = "pass";

WifiLink::WifiLink(const char* nvsNamespace) : ns(nvsNamespace) {}

bool WifiLink::saved(Network& out) const {
  Preferences prefs;
  if (!prefs.begin(ns, true)) return false;  // namespace does not exist yet
  out.ssid = prefs.getString(KEY_SSID, "");
  out.password = prefs.getString(KEY_PASS, "");
  prefs.end();
  return out.ssid.length() > 0;
}

bool WifiLink::save(const String& ssid, const String& password) {
  if (!ssid.length()) return false;
  Preferences prefs;
  if (!prefs.begin(ns, false)) return false;
  bool ok = prefs.putString(KEY_SSID, ssid) > 0;
  if (password.length()) {
    ok = ok && prefs.putString(KEY_PASS, password) > 0;
  } else {
    prefs.remove(KEY_PASS);
  }
  prefs.end();
  return ok;
}

void WifiLink::forget() {
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.clear();
  prefs.end();
}

bool WifiLink::connect(const Network& network, uint32_t timeoutMs) {
  if (!network.ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(network.ssid.c_str(), network.password.length() ? network.password.c_str() : nullptr);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    return false;
  }
  current = network.ssid;
  return true;
}

bool WifiLink::connect(const Network* fallbacks, size_t count, uint32_t timeoutMs) {
  Network first;
  bool hasSaved = saved(first);
  if (hasSaved && connect(first, timeoutMs)) return true;
  for (size_t i = 0; i < count; i++) {
    const Network& n = fallbacks[i];
    bool tried = hasSaved && n.ssid == first.ssid;
    for (size_t j = 0; j < i && !tried; j++) tried = fallbacks[j].ssid == n.ssid;
    if (!tried && connect(n, timeoutMs)) return true;
  }
  return false;
}

void WifiLink::disconnect() {
  WiFi.disconnect();
  current = "";
}

bool WifiLink::connected() const { return WiFi.status() == WL_CONNECTED; }
