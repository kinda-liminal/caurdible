#include "ui.hpp"
#include "config.hpp"
#include "path_utils.hpp"
#include "fonts/font_mono12.h"
#include "fonts/font_mono9.h"
#include "fonts/font_text10.h"
#include "fonts/font_text11.h"
#include "fonts/font_title17.h"
#include <M5Cardputer.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>

namespace {

using Face = Ui::Face;
using Key = Ui::Key;

struct UiFont {
  const uint8_t* data;
  const lgfx::IFont* fallback;
  lgfx::PointerWrapper wrapper;
  lgfx::VLWfont font;
  bool loaded;
};

// Order matches Ui::Face.
UiFont gFonts[] = {
    {kFontTitle17, &fonts::efontJA_12, {}, {}, false},
    {kFontText11, &fonts::efontJA_12, {}, {}, false},
    {kFontText10, &fonts::efontJA_10, {}, {}, false},
    {kFontMono12, &fonts::efontJA_12, {}, {}, false},
    {kFontMono9, &fonts::efontJA_10, {}, {}, false},
};

constexpr int kBatteryW = 50;
constexpr int kContentBottom = cfg::kScreenH - cfg::kKeyBarH;
constexpr const char* kEllipsis = "\xE2\x80\xA6";
constexpr const char* kMinus = "\xE2\x88\x92";

UiFont& fontFor(Face face) { return gFonts[static_cast<size_t>(face)]; }

// The smooth fonts hold ASCII, Latin-1, and a few punctuation marks.
bool inSmoothSet(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return true;
  if (cp >= 0xA0 && cp <= 0xFF) return cp != 0xAD && cp != 0xB5;
  switch (cp) {
    case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201C: case 0x201D:
    case 0x2022: case 0x2026: case 0x2039: case 0x203A: case 0x2212:
      return true;
    default:
      return false;
  }
}

bool coveredBySmoothFont(const char* s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    uint32_t cp = *p;
    int extra = 0;
    if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    ++p;
    for (; extra > 0 && (*p & 0xC0) == 0x80; --extra, ++p) cp = (cp << 6) | (*p & 0x3F);
    if (extra != 0 || !inSmoothSet(cp)) return false;
  }
  return true;
}

const lgfx::IFont* selectFont(Face face, const char* s) {
  UiFont& f = fontFor(face);
  if (f.loaded && coveredBySmoothFont(s)) return &f.font;
  return f.fallback;
}

int rowY(size_t i) { return cfg::kListTop + static_cast<int>(i) * cfg::kListRowH; }

void formatClock(char* buf, size_t cap, uint32_t ms) {
  const unsigned long s = ms / 1000;
  if (s >= 3600) {
    snprintf(buf, cap, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  } else {
    snprintf(buf, cap, "%lu:%02lu", s / 60, s % 60);
  }
}

const char* typeTag(const char* name) {
  if (path::hasExtInsensitive(name, ".m4b")) return "M4B";
  if (path::hasExtInsensitive(name, ".m4a")) return "M4A";
  if (path::hasExtInsensitive(name, ".mp3")) return "MP3";
  if (path::hasExtInsensitive(name, ".wav")) return "WAV";
  return "";
}

void fileStem(char* dest, size_t cap, const char* name) {
  snprintf(dest, cap, "%s", name);
  if (typeTag(name)[0] == '\0') return;
  char* dot = std::strrchr(dest, '.');
  if (dot && dot != dest) *dot = '\0';
}

const char* stateWord(PlayState state) {
  switch (state) {
    case PlayState::Playing: return "Playing";
    case PlayState::Paused: return "Paused";
    case PlayState::Done: return "Finished";
    case PlayState::Error: return "Stopped";
    default: return "";
  }
}

}  // namespace

void Ui::begin() {
  for (UiFont& f : gFonts) {
    if (f.loaded) continue;
    f.wrapper.set(f.data);
    f.loaded = f.font.loadFont(&f.wrapper);
  }
  applyTheme(themes::get(0));
  auto& d = M5Cardputer.Display;
  d.setRotation(1);
  d.setBrightness(cfg::kDisplayBrightness);
  d.fillScreen(theme_.bg);
  d.setTextSize(1);
  d.setAttribute(lgfx::attribute_t::utf8_switch, true);
  hasLastBrowse_ = false;
  hasLastPlayer_ = false;
  hasLastSettings_ = false;
  battery_ = {};
  displayOn_ = true;
  lastKeys_[0] = '\0';
}

void Ui::applyTheme(const Theme& t) {
  theme_ = t;
}

void Ui::setBrightness(uint8_t b) {
  if (displayOn_) {
    M5Cardputer.Display.setBrightness(b);
  }
}

void Ui::setDisplayOn(bool on) {
  if (on == displayOn_) return;
  displayOn_ = on;
  auto& d = M5Cardputer.Display;
  if (on) {
    // App restores the brightness through setBrightness after wake.
    hasLastBrowse_ = false;
    hasLastPlayer_ = false;
    hasLastSettings_ = false;
    battery_ = {};
  } else {
    d.setBrightness(0);
  }
}

void Ui::showToast(const char* text, uint32_t nowMs) {
  strncpy(toast_.text, text, sizeof(toast_.text) - 1);
  toast_.text[sizeof(toast_.text) - 1] = '\0';
  toast_.expiresAtMs = nowMs + cfg::kToastMs;
}

int Ui::textWidth(const char* s, Face face) {
  auto& d = M5Cardputer.Display;
  d.setFont(selectFont(face, s));
  return d.textWidth(s);
}

// Draws one line with its baseline at `baseline`. Text wider than maxWidth ends in an ellipsis.
// Returns the x coordinate past the far edge of the drawn text.
int Ui::text(const char* s, int x, int baseline, Face face, uint16_t fg, uint16_t bg,
             int maxWidth, bool alignRight) {
  if (!s || !*s) return x;
  auto& d = M5Cardputer.Display;
  const lgfx::IFont* font = selectFont(face, s);
  d.setFont(font);
  d.setTextColor(fg, bg);
  d.setTextDatum(alignRight ? lgfx::textdatum_t::baseline_right : lgfx::textdatum_t::baseline_left);
  char buf[cfg::kMaxPathLen + 8];
  const char* out = s;
  if (maxWidth > 0 && d.textWidth(s) > maxWidth) {
    const char* dots = font == &fontFor(face).font ? kEllipsis : "...";
    const int room = maxWidth - d.textWidth(dots);
    const int keep = room > 0 ? d.textLength(s, room) : 0;
    snprintf(buf, sizeof(buf), "%.*s%s", keep, s, dots);
    out = buf;
  }
  const int w = d.textWidth(out);
  d.drawString(out, x, baseline);
  return alignRight ? x - w : x + w;
}

void Ui::showSplash() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(theme_.bg);
  // An open book, seen from the front.
  d.fillTriangle(97, 44, 118, 48, 118, 72, theme_.fg);
  d.fillTriangle(97, 44, 118, 72, 97, 68, theme_.fg);
  d.fillTriangle(143, 44, 122, 48, 122, 72, theme_.fg);
  d.fillTriangle(143, 44, 122, 72, 143, 68, theme_.fg);
  for (int i = 0; i < 3; ++i) {
    d.drawLine(101, 52 + i * 5, 114, 55 + i * 5, theme_.muted);
    d.drawLine(126, 55 + i * 5, 139, 52 + i * 5, theme_.muted);
  }
  const char* name = "Caurdible";
  text(name, (cfg::kScreenW - textWidth(name, Face::Title)) / 2, 98, Face::Title, theme_.fg,
       theme_.bg);
  const char* tagline = "Audiobooks in your pocket";
  text(tagline, (cfg::kScreenW - textWidth(tagline, Face::Small)) / 2, 114, Face::Small,
       theme_.muted, theme_.bg);
  drawSplashWaves(0);
}

// Sound waves rise from the book one at a time.
void Ui::drawSplashWaves(int step) {
  auto& d = M5Cardputer.Display;
  for (int i = 0; i < 3; ++i) {
    const int r = 7 + i * 6;
    d.fillArc(120, 42, r, r + 1, 235, 305, i < step ? theme_.accent : theme_.bg);
  }
}

void Ui::finishSplash(uint32_t startedMs) {
  int step = 1;
  while (millis() - startedMs < cfg::kSplashMs || step % 4 != 0) {
    drawSplashWaves(step % 4);
    ++step;
    delay(140);
  }
  drawSplashWaves(3);
  delay(250);
  M5Cardputer.Display.fillScreen(theme_.bg);
}

bool Ui::browseChanged(const BrowseSnapshot& b) const {
  if (!hasLastBrowse_) return true;
  if (b.sdOk != lastSdOk_) return true;
  if (b.count != lastCount_) return true;
  if (b.cursor != lastCursor_) return true;
  if (b.scroll != lastScroll_) return true;
  if (b.truncated != lastTruncated_) return true;
  if (std::strcmp(b.path, lastPath_) != 0) return true;
  if (b.entries) {
    for (size_t i = 0; i < b.count; ++i) {
      if (b.entries[i].kind != lastEntries_[i].kind) return true;
      if (std::strcmp(b.entries[i].name, lastEntries_[i].name) != 0) return true;
    }
  }
  return false;
}

bool Ui::playerChromeChanged(const PlayerSnapshot& p) const {
  if (!hasLastPlayer_) return true;
  if (p.state != lastPlayer_.state) return true;
  if (p.volumePercent != lastPlayer_.volumePercent) return true;
  if (p.speedPercent != lastPlayer_.speedPercent) return true;
  if (std::strcmp(p.fileName, lastPlayer_.fileName) != 0) return true;
  if (std::strcmp(p.folderName, lastPlayer_.folderName) != 0) return true;
  return false;
}

bool Ui::playerChanged(const PlayerSnapshot& p) const {
  if (playerChromeChanged(p)) return true;
  if ((p.positionMs / 1000) != (lastPlayer_.positionMs / 1000)) return true;
  if ((p.durationMs / 1000) != (lastPlayer_.durationMs / 1000)) return true;
  if (p.durationEstimated != lastPlayer_.durationEstimated) return true;
  return false;
}

bool Ui::settingsChanged(const Settings& s) const {
  if (!hasLastSettings_) return true;
  SettingsSnapshot cur = s.snapshot();
  return cur.volumePercent != lastSettings_.volumePercent ||
         cur.brightness != lastSettings_.brightness ||
         cur.displayTimeoutMs != lastSettings_.displayTimeoutMs ||
         cur.idleTimeoutMs != lastSettings_.idleTimeoutMs ||
         cur.autoNext != lastSettings_.autoNext ||
         cur.themeIndex != lastSettings_.themeIndex ||
         cur.cursor != lastSettings_.cursor;
}

void Ui::rememberBrowse(const BrowseSnapshot& b) {
  lastSdOk_ = b.sdOk;
  lastCount_ = b.count;
  lastCursor_ = b.cursor;
  lastScroll_ = b.scroll;
  lastTruncated_ = b.truncated;
  std::strncpy(lastPath_, b.path, cfg::kMaxPathLen - 1);
  lastPath_[cfg::kMaxPathLen - 1] = '\0';
  if (b.entries) {
    for (size_t i = 0; i < b.count && i < cfg::kMaxDirEntries; ++i) {
      lastEntries_[i] = b.entries[i];
    }
  }
  hasLastBrowse_ = true;
}

void Ui::rememberPlayer(const PlayerSnapshot& p) {
  lastPlayer_ = p;
  hasLastPlayer_ = true;
}

void Ui::rememberSettings(const Settings& s) {
  lastSettings_ = s.snapshot();
  hasLastSettings_ = true;
}

bool Ui::recentTimesChanged(const Settings& s) const {
  return s.recentTimesDiffer(lastRecentPos_, lastRecentCount_);
}

void Ui::rememberRecentTimes(const Settings& s) {
  lastRecentCount_ = s.recentCount();
  for (size_t i = 0; i < lastRecentCount_ && i < cfg::kRecentCount; ++i) {
    const RecentEntry* e = s.recentEntry(i);
    lastRecentPos_[i] = e ? e->positionMs : 0;
  }
}

bool Ui::render(Screen screen,
                const BrowseSnapshot& browse,
                const PlayerSnapshot& player,
                const Settings& settings,
                bool locked,
                uint32_t nowMs,
                bool force) {
  if (!displayOn_) {
    if (toast_.expiresAtMs != 0 && nowMs >= toast_.expiresAtMs) {
      toast_.expiresAtMs = 0;
      lastToastExp_ = 0;
    }
    return false;
  }
  if (locked != locked_) {
    locked_ = locked;
    force = true;
  }

  const int previousBatteryMv = battery_.millivolts();
  const int previousBatteryLevel = battery_.level();
  if (battery_.due(nowMs)) {
    battery_.addSample(nowMs, M5.Power.getBatteryVoltage());
  }
  const bool batteryChanged = battery_.level() != previousBatteryLevel;
  const bool batteryVoltageChanged = battery_.millivolts() != previousBatteryMv;

  // Read the selected theme from settings. Apply theme changes immediately.
  bool themeChanged = false;
  if (theme_.name != settings.theme().name) {
    applyTheme(settings.theme());
    themeChanged = true;
  }

  const bool toastAppeared = (toast_.expiresAtMs != 0 && toast_.expiresAtMs != lastToastExp_);
  const bool toastExpired =
      (lastToastExp_ != 0 && (toast_.expiresAtMs == 0 || nowMs >= toast_.expiresAtMs));

  if (toast_.expiresAtMs != 0 && nowMs >= toast_.expiresAtMs) {
    toast_.expiresAtMs = 0;
  }

  const bool screenSwitch = (screen != lastScreen_);
  bool dirty = force || themeChanged || screenSwitch || toastAppeared || toastExpired;
  bool progressOnly = false;
  if (!dirty) {
    if (screen == Screen::Browse) {
      dirty = browseChanged(browse);
    } else if (screen == Screen::Recent) {
      dirty = browseChanged(browse) || recentTimesChanged(settings);
    } else if (screen == Screen::Playing) {
      if (playerChromeChanged(player)) {
        dirty = true;
      } else if (playerChanged(player)) {
        dirty = true;
        progressOnly = hasLastPlayer_;
      }
    } else if (screen == Screen::Settings) {
      dirty = settingsChanged(settings);
    } else if (screen == Screen::System) {
      dirty = batteryVoltageChanged || nowMs - lastSystemMs_ >= 1000 ||
              browse.sdOk != lastSdOk_;
    }
  }

  if (!dirty) {
    if (batteryChanged) drawBattery();
    return batteryChanged;
  }

  auto& d = M5Cardputer.Display;
  d.startWrite();

  // A toast covers content, so its removal redraws the whole screen.
  const bool full = force || themeChanged || screenSwitch || toastExpired;
  if (full) {
    d.fillScreen(theme_.bg);
    hasLastBrowse_ = false;
    hasLastPlayer_ = false;
    hasLastSettings_ = false;
    lastKeys_[0] = '\0';
    progressOnly = false;
  }
  lastScreen_ = screen;

  if (screen == Screen::Browse) {
    drawBrowse(browse, !hasLastBrowse_);
    rememberBrowse(browse);
  } else if (screen == Screen::Recent) {
    drawRecent(browse, settings, !hasLastBrowse_);
    rememberBrowse(browse);
    rememberRecentTimes(settings);
  } else if (screen == Screen::Playing) {
    if (progressOnly) {
      drawPlayingProgress(player);
    } else {
      drawPlaying(player, !hasLastPlayer_);
    }
    rememberPlayer(player);
  } else if (screen == Screen::Settings) {
    drawSettings(settings);
    rememberSettings(settings);
  } else if (screen == Screen::System) {
    drawSystem(browse, nowMs, full);
    lastSdOk_ = browse.sdOk;
  }

  if (full || batteryChanged) drawBattery();
  drawToastIfAny(nowMs);
  lastToastExp_ = toast_.expiresAtMs;
  d.endWrite();
  return true;
}

void Ui::drawTopBar(const char* title, const char* detail) {
  auto& d = M5Cardputer.Display;
  const int right = cfg::kScreenW - kBatteryW;
  d.fillRect(0, 0, right, cfg::kTopBarH, theme_.bg);
  int x = 8;
  if (title && *title) {
    x = text(title, x, 11, Face::Small, theme_.fg, theme_.bg, right - 16) + 6;
  }
  if (detail && *detail && x < right - 16) {
    text(detail, x, 11, Face::Small, theme_.muted, theme_.bg, right - 8 - x);
  }
}

void Ui::drawBattery() {
  auto& d = M5Cardputer.Display;
  const int x0 = cfg::kScreenW - kBatteryW;
  d.fillRect(x0, 0, kBatteryW, cfg::kTopBarH, theme_.bg);
  const int level = battery_.level();
  char label[8];
  if (level < 0) {
    snprintf(label, sizeof(label), "--");
  } else {
    snprintf(label, sizeof(label), "%d%%", level);
  }
  const int ix = cfg::kScreenW - 21;
  const int iy = 5;
  text(label, ix - 4, 11, Face::Tiny, theme_.muted, theme_.bg, 0, true);
  d.drawRoundRect(ix, iy, 12, 7, 1, theme_.muted);
  d.fillRect(ix + 12, iy + 2, 2, 3, theme_.muted);
  if (level > 0) {
    int fill = (level * 8 + 99) / 100;
    if (fill > 8) fill = 8;
    d.fillRect(ix + 2, iy + 2, fill, 3, theme_.muted);
  }
}

void Ui::drawKeys(const Key* keys, size_t count) {
  static const Key kLocked[] = {{"Fn+L", "Unlock keys"}};
  if (locked_) {
    keys = kLocked;
    count = 1;
  }
  char signature[sizeof(lastKeys_)] = "";
  size_t used = 0;
  for (size_t i = 0; i < count && used < sizeof(signature); ++i) {
    used += snprintf(signature + used, sizeof(signature) - used, "%s|%s|", keys[i].cap,
                     keys[i].label);
  }
  if (std::strcmp(signature, lastKeys_) == 0) return;
  std::strncpy(lastKeys_, signature, sizeof(lastKeys_) - 1);
  lastKeys_[sizeof(lastKeys_) - 1] = '\0';

  auto& d = M5Cardputer.Display;
  const int y = kContentBottom;
  const int capY = y + 4;
  const int baseline = y + 12;
  d.fillRect(0, y, cfg::kScreenW, cfg::kKeyBarH, theme_.surface);
  int x = 7;
  for (size_t i = 0; i < count; ++i) {
    const bool leftRight = std::strcmp(keys[i].cap, "<>") == 0;
    const bool upDown = std::strcmp(keys[i].cap, "^v") == 0;
    int w = (leftRight || upDown) ? 17 : textWidth(keys[i].cap, Face::Tiny) + 6;
    if (w < 11) w = 11;
    d.fillRoundRect(x, capY, w, 11, 2, theme_.bg);
    if (leftRight) {
      d.fillTriangle(x + 3, capY + 5, x + 7, capY + 2, x + 7, capY + 8, theme_.fg);
      d.fillTriangle(x + 14, capY + 5, x + 10, capY + 2, x + 10, capY + 8, theme_.fg);
    } else if (upDown) {
      d.fillTriangle(x + 5, capY + 2, x + 2, capY + 7, x + 8, capY + 7, theme_.fg);
      d.fillTriangle(x + 12, capY + 8, x + 9, capY + 3, x + 15, capY + 3, theme_.fg);
    } else {
      text(keys[i].cap, x + (w - textWidth(keys[i].cap, Face::Tiny)) / 2, baseline, Face::Tiny,
           theme_.fg, theme_.bg);
    }
    x += w + 3;
    x = text(keys[i].label, x, baseline, Face::Small, theme_.muted, theme_.surface) + 8;
  }
}

void Ui::drawToastIfAny(uint32_t nowMs) {
  if (toast_.expiresAtMs == 0 || nowMs >= toast_.expiresAtMs) {
    return;
  }
  auto& d = M5Cardputer.Display;
  int w = textWidth(toast_.text, Face::Small) + 18;
  if (w > cfg::kScreenW - 16) w = cfg::kScreenW - 16;
  const int x = (cfg::kScreenW - w) / 2;
  const int y = 90;
  d.fillRoundRect(x, y, w, 17, 4, theme_.fg);
  text(toast_.text, x + 9, y + 12, Face::Small, theme_.bg, theme_.fg, w - 18);
}

void Ui::drawBrowse(const BrowseSnapshot& b, bool full) {
  auto& d = M5Cardputer.Display;

  if (!b.sdOk) {
    if (full || lastSdOk_) {
      d.fillRect(0, 0, cfg::kScreenW, kContentBottom, theme_.bg);
      drawTopBar("Library", nullptr);
      text("No SD card", 8, 56, Face::Title, theme_.fg, theme_.bg);
      text("Insert a FAT32 card, then press Enter.", 8, 76, Face::Small, theme_.muted, theme_.bg);
    }
    static const Key kKeys[] = {{"Ent", "Retry"}, {"S", "Settings"}, {"I", "Info"}};
    drawKeys(kKeys, 3);
    return;
  }

  const bool listIdentityChanged =
      full || !hasLastBrowse_ || b.scroll != lastScroll_ || b.count != lastCount_ ||
      std::strcmp(b.path, lastPath_) != 0 || !lastSdOk_;

  if (listIdentityChanged || b.truncated != lastTruncated_) {
    char folder[cfg::kMaxNameLen];
    path::fileName(folder, sizeof(folder), b.path);
    char detail[cfg::kMaxNameLen + 16];
    snprintf(detail, sizeof(detail), "%s%s", folder[0] ? folder : "/",
             b.truncated ? "  (first 256)" : "");
    drawTopBar("Library", detail);
  }

  size_t visible = (b.count > b.scroll) ? (b.count - b.scroll) : 0;
  if (visible > static_cast<size_t>(cfg::kMaxVisibleRows)) visible = cfg::kMaxVisibleRows;

  for (size_t i = 0; i < static_cast<size_t>(cfg::kMaxVisibleRows); ++i) {
    const int y = rowY(i);
    const bool rowVisible = (i < visible);
    const size_t idx = b.scroll + i;

    bool needRow = listIdentityChanged;
    if (!needRow && rowVisible && hasLastBrowse_) {
      const size_t oldIdx = lastScroll_ + i;
      const size_t oldVis = (lastCount_ > lastScroll_) ? (lastCount_ - lastScroll_) : 0;
      const bool wasVisible = (i < oldVis && i < static_cast<size_t>(cfg::kMaxVisibleRows));
      const bool selNow = (idx == b.cursor);
      const bool selWas = wasVisible && (oldIdx == lastCursor_);
      if (selNow != selWas) needRow = true;
      if (!needRow && wasVisible && b.entries) {
        if (b.entries[idx].kind != lastEntries_[oldIdx].kind ||
            std::strcmp(b.entries[idx].name, lastEntries_[oldIdx].name) != 0) {
          needRow = true;
        }
      }
      if (!wasVisible) needRow = true;
    } else if (!needRow && !rowVisible && hasLastBrowse_) {
      const size_t oldVis = (lastCount_ > lastScroll_) ? (lastCount_ - lastScroll_) : 0;
      if (i < oldVis) needRow = true;
    }
    if (!needRow) continue;

    if (!rowVisible || !b.entries) {
      d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, theme_.bg);
      continue;
    }
    const DirEntry& e = b.entries[idx];
    const bool selected = (idx == b.cursor);
    const uint16_t bg = selected ? theme_.surface : theme_.bg;
    const uint16_t icon = selected ? theme_.accent : theme_.muted;
    d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, bg);
    int right = cfg::kScreenW - 8;
    if (e.kind == EntryKind::Dir) {
      d.drawRoundRect(8, y + 5, 10, 8, 1, icon);
      d.fillRect(8, y + 3, 4, 2, icon);
    } else {
      d.fillCircle(13, y + 8, 2, icon);
      right = text(typeTag(e.name), right, y + 12, Face::Tiny, theme_.muted, bg, 0, true) - 6;
    }
    text(e.name, 24, y + 12, Face::Text, theme_.fg, bg, right - 24);
  }

  if (b.count == 0 && (full || lastCount_ != 0)) {
    text("Empty folder", 24, rowY(0) + 12, Face::Text, theme_.muted, theme_.bg);
  }

  static const Key kKeys[] = {{"Ent", "Open"}, {"Del", "Back"}, {"R", "Recent"}, {"P", "Playing"}};
  drawKeys(kKeys, 4);
}

void Ui::drawRecent(const BrowseSnapshot& b, const Settings& s, bool full) {
  auto& d = M5Cardputer.Display;

  if (full || b.count != lastCount_) {
    char detail[8];
    snprintf(detail, sizeof(detail), "%u", static_cast<unsigned>(b.count));
    drawTopBar("Recent", b.count ? detail : nullptr);
  }

  for (size_t i = 0; i < static_cast<size_t>(cfg::kMaxVisibleRows); ++i) {
    const int y = rowY(i);
    const bool rowVisible = (i < b.count);
    bool needRow = full || !hasLastBrowse_ || b.count != lastCount_;
    if (!needRow && rowVisible && hasLastBrowse_) {
      if ((i == b.cursor) != (i == lastCursor_)) needRow = true;
      if (!needRow && b.entries && std::strcmp(b.entries[i].name, lastEntries_[i].name) != 0) {
        needRow = true;
      }
      if (!needRow) {
        const RecentEntry* e = s.recentEntry(i);
        const uint32_t pos = e ? e->positionMs : 0;
        if (i >= lastRecentCount_ || pos / 1000 != lastRecentPos_[i] / 1000) needRow = true;
      }
    } else if (!needRow && !rowVisible && hasLastBrowse_) {
      if (i < lastCount_) needRow = true;
    }
    if (!needRow) continue;

    if (!rowVisible || !b.entries) {
      d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, theme_.bg);
      continue;
    }
    const bool selected = (i == b.cursor);
    const uint16_t bg = selected ? theme_.surface : theme_.bg;
    d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, bg);
    int right = cfg::kScreenW - 8;
    const RecentEntry* e = s.recentEntry(i);
    if (e) {
      char clock[16];
      formatClock(clock, sizeof(clock), e->positionMs);
      right = text(clock, right, y + 12, Face::Tiny, theme_.muted, bg, 0, true) - 8;
    }
    char stem[cfg::kMaxNameLen];
    fileStem(stem, sizeof(stem), b.entries[i].name);
    text(stem, 8, y + 12, Face::Text, theme_.fg, bg, right - 8);
  }

  if (b.count == 0 && (full || lastCount_ != 0)) {
    text("Nothing played yet", 8, rowY(0) + 12, Face::Text, theme_.muted, theme_.bg);
  }

  static const Key kKeys[] = {{"Ent", "Play"}, {"^v", "Move"}, {"Del", "Back"}};
  drawKeys(kKeys, 3);
}

void Ui::drawPlayingProgress(const PlayerSnapshot& p) {
  auto& d = M5Cardputer.Display;
  const int barX = 8;
  const int barW = cfg::kScreenW - 16;
  int fill = 0;
  if (p.durationMs > 0) {
    fill = static_cast<int>((static_cast<uint64_t>(p.positionMs) * barW) / p.durationMs);
    if (fill > barW) fill = barW;
  }
  const uint16_t done = p.state == PlayState::Playing ? theme_.accent : theme_.muted;
  d.fillRect(barX, 66, fill, 3, done);
  d.fillRect(barX + fill, 66, barW - fill, 3, theme_.surface);

  d.fillRect(0, 73, cfg::kScreenW, 15, theme_.bg);
  char clock[16];
  formatClock(clock, sizeof(clock), p.positionMs);
  text(clock, 8, 84, Face::Time, theme_.fg, theme_.bg);
  char remaining[24];
  if (p.durationMs == 0) {
    snprintf(remaining, sizeof(remaining), "--:--");
  } else {
    formatClock(clock, sizeof(clock), p.durationMs > p.positionMs ? p.durationMs - p.positionMs : 0);
    snprintf(remaining, sizeof(remaining), "%s%s%s", p.durationEstimated ? "~" : "", kMinus, clock);
  }
  text(remaining, cfg::kScreenW - 8, 84, Face::Time, theme_.muted, theme_.bg, 0, true);
}

void Ui::drawPlaying(const PlayerSnapshot& p, bool full) {
  auto& d = M5Cardputer.Display;

  if (full || std::strcmp(p.folderName, lastPlayer_.folderName) != 0) {
    drawTopBar(nullptr, p.folderName[0] ? p.folderName : "Now Playing");
  }
  if (full || std::strcmp(p.fileName, lastPlayer_.fileName) != 0) {
    d.fillRect(0, 20, cfg::kScreenW, 24, theme_.bg);
    char stem[cfg::kMaxNameLen];
    fileStem(stem, sizeof(stem), p.fileName);
    text(stem, 8, 37, Face::Title, theme_.fg, theme_.bg, cfg::kScreenW - 16);
  }
  if (full || p.state != lastPlayer_.state) {
    d.fillRect(0, 45, cfg::kScreenW, 14, theme_.bg);
    text(stateWord(p.state), 8, 56, Face::Text, theme_.muted, theme_.bg);
  }

  drawPlayingProgress(p);

  if (full || p.speedPercent != lastPlayer_.speedPercent ||
      p.volumePercent != lastPlayer_.volumePercent) {
    d.fillRect(0, 94, cfg::kScreenW, 17, theme_.bg);
    if (p.speedPercent != 100) {
      char speed[16];
      snprintf(speed, sizeof(speed), "%d.%d\xC3\x97", p.speedPercent / 100, p.speedPercent % 100 / 10);
      const int w = textWidth(speed, Face::Small) + 10;
      d.fillRoundRect(8, 96, w, 13, 3, theme_.surface);
      text(speed, 13, 106, Face::Small, theme_.fg, theme_.surface);
    }
    char volume[16];
    snprintf(volume, sizeof(volume), "Vol %d%%", p.volumePercent);
    text(volume, cfg::kScreenW - 8, 106, Face::Small, theme_.muted, theme_.bg, 0, true);
  }

  const bool playing = p.state == PlayState::Playing;
  const Key keys[] = {{"Spc", playing ? "Pause" : "Play"}, {"[ ]", "Track"}, {"<>", "30s"},
                      {"9 0", "Speed"}};
  drawKeys(keys, 4);
}

void Ui::drawSettings(const Settings& s) {
  auto& d = M5Cardputer.Display;
  drawTopBar("Settings", nullptr);

  for (size_t i = 0; i < Settings::kCount && i < static_cast<size_t>(cfg::kMaxVisibleRows); ++i) {
    const int y = rowY(i);
    const bool sel = (i == s.cursor());
    const uint16_t bg = sel ? theme_.surface : theme_.bg;
    d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, bg);
    char val[24];
    s.formatValue(i, val, sizeof(val));
    char shown[32];
    if (sel) {
      snprintf(shown, sizeof(shown), "\xE2\x80\xB9 %s \xE2\x80\xBA", val);
    } else {
      snprintf(shown, sizeof(shown), "%s", val);
    }
    const int right = text(shown, cfg::kScreenW - 8, y + 12, Face::Text,
                           sel ? theme_.fg : theme_.muted, bg, 0, true);
    text(s.label(i), 8, y + 12, Face::Text, theme_.fg, bg, right - 16);
  }

  static const Key kKeys[] = {{"^v", "Choose"}, {"<>", "Change"}, {"Del", "Back"}};
  drawKeys(kKeys, 3);
}

void Ui::drawSystem(const BrowseSnapshot& b, uint32_t nowMs, bool full) {
  auto& d = M5Cardputer.Display;
  char value[40];
  auto row = [&](size_t i, const char* label, const char* v) {
    const int y = rowY(i);
    d.fillRect(0, y, cfg::kScreenW, cfg::kListRowH, theme_.bg);
    const int right = text(v, cfg::kScreenW - 8, y + 12, Face::Text, theme_.muted, theme_.bg, 0, true);
    text(label, 8, y + 12, Face::Text, theme_.fg, theme_.bg, right - 16);
  };
  if (full) {
    drawTopBar("Info", "Caurdible");
    snprintf(value, sizeof(value), "%s \xC2\xB7 %lu MHz", ESP.getChipModel(),
             static_cast<unsigned long>(ESP.getCpuFreqMHz()));
    row(0, "Chip", value);
    snprintf(value, sizeof(value), "%lu MB",
             static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)));
    row(1, "Flash", value);
  }
  snprintf(value, sizeof(value), "%lu KB", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  row(2, "Free memory", value);
  const uint64_t seconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
  formatClock(value, sizeof(value), static_cast<uint32_t>(seconds * 1000));
  row(3, "Uptime", value);
  row(4, "SD card", b.sdOk ? "Mounted" : "Not found");
  const int mv = battery_.millivolts();
  if (mv > 0) {
    snprintf(value, sizeof(value), "%d.%02d V", mv / 1000, (mv % 1000) / 10);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  row(5, "Battery", value);

  static const Key kKeys[] = {{"Del", "Back"}};
  drawKeys(kKeys, 1);
  lastSystemMs_ = nowMs;
}
