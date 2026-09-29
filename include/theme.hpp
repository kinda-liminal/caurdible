#pragma once

#include <cstdint>
#include <cstddef>

// RGB565 palettes for the 240x135 TFT. Source colors are in the comments.
// Slate is dark and low-glare for listening in bed. Paper is light for daylight.
struct Theme {
  const char* name;
  uint16_t bg;       // screen background
  uint16_t surface;  // selected rows, key bar, chips
  uint16_t fg;       // main text
  uint16_t muted;    // secondary text and icons
  uint16_t accent;   // progress, current item
};

namespace themes {

constexpr size_t kCount = 2;

inline const Theme& get(size_t index) {
  static const Theme kAll[kCount] = {
      // Slate: #1B1F27 #282D37 #E6E2DA #8B93A1 #E3AA6E
      {"Slate", 0x1905, 0x2967, 0xE71B, 0x8C94, 0xE54D},
      // Paper: #EEEBE5 #DEDAD2 #24262B #575B63 #2F6B8F
      {"Paper", 0xEF5C, 0xDEDA, 0x2125, 0x5ACC, 0x3351},
  };
  if (index >= kCount) index = 0;
  return kAll[index];
}

inline const char* name(size_t index) { return get(index).name; }

}  // namespace themes
