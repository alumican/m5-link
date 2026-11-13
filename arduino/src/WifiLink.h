#pragma once
// WifiLink: joins Wi-Fi as a station, trying the network saved in NVS first and then fallbacks in order.
// Getting credentials from a person (QR codes, setup pages) is left to the application; pass the result
// to save().
//
//   WifiLink wifi("myapp-wifi");          // NVS namespace, up to 15 characters
//   const WifiLink::Network spare[] = {{"backup-ssid", "backup-password"}};
//   if (wifi.connect(spare, 1)) {
//     ... wifi.ssid() ...
//   }

#include <Arduino.h>

class WifiLink {
 public:
  struct Network {
    String ssid;
    String password;  // empty for open networks
  };

  explicit WifiLink(const char* nvsNamespace);

  // Saved network, or false if none.
  bool saved(Network& out) const;
  // Replaces the saved network. Returns false if ssid is empty or NVS fails.
  bool save(const String& ssid, const String& password);
  // Erases the saved network.
  void forget();

  // Tries the saved network, then each fallback, waiting up to timeoutMs for each.
  // Networks with an empty ssid and repeats of an ssid already tried are skipped.
  // Returns true once connected.
  bool connect(const Network* fallbacks = nullptr, size_t count = 0, uint32_t timeoutMs = 10000);
  // Joins one network. Returns true once connected.
  bool connect(const Network& network, uint32_t timeoutMs = 10000);
  void disconnect();

  bool connected() const;
  // Network of the last successful connect(), empty if none.
  const String& ssid() const { return current; }

 private:
  const char* ns;
  String current;
};
