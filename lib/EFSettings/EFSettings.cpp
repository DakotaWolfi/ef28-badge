//#include <EFConfig.h>
#include "EFSettings.h"
#include <Preferences.h>

namespace {
  Preferences prefs;
  constexpr const char* NS = "badge";
  constexpr const char* KEY_NAME = "name";
  constexpr size_t MAX_NAME = 24;

  bool inited = false;
}

namespace EFSettings {
  bool begin() {
    if (inited) return true;
    inited = prefs.begin(NS, false); // RW
    if (!inited) return false;

    // Seed from EF_USER_NAME on first boot if no name in NVS and EF_USER_NAME provided
    String stored = prefs.getString(KEY_NAME, "");
    #ifdef EF_USER_NAME_FORCE
      // Force override at every boot
      if (strlen(EF_USER_NAME) > 0) {
        prefs.putString(KEY_NAME, String(EF_USER_NAME));
      } else {
        prefs.remove(KEY_NAME);
      }
    #else
      // Only seed once if empty and EF_USER_NAME is non-empty
      if (stored.isEmpty() && strlen(EF_USER_NAME) > 0) {
        prefs.putString(KEY_NAME, String(EF_USER_NAME));
      }
    #endif

    return true;
  }

  String getName() {
    if (!inited) return String();
    return prefs.getString(KEY_NAME, "");
  }

  bool setName(const String& nameIn) {
    if (!inited) return false;
    String n = nameIn; n.trim();
    if (n.isEmpty() || n.length() > MAX_NAME) return false;
    return prefs.putString(KEY_NAME, n) > 0;
  }

  bool resetName() {
    if (!inited) return false;
    prefs.remove(KEY_NAME);
    return true;
  }
}
