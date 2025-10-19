#pragma once
#include <EFConfig.h>
#include <Arduino.h>

namespace EFSettings {
  bool begin();                 // call in EFBoardClass::setup()
  String getName();             // returns "" if not set
  bool setName(const String&);  // trims, length-checks, saves to NVS
  bool resetName();             // clears the stored name (back to default)
}
