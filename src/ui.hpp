#pragma once
#include "types.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "battery_reading.hpp"
#include <cstddef>
#include <cstdint>

class Ui {
 public:
  // Font roles. Each has a smooth Atkinson Hyperlegible face and a bitmap fallback
  // for text outside Latin-1.
  enum class Face : uint8_t { Title, Text, Small, Time, Tiny };

  // One entry in the key bar. Cap "<>" draws left and right arrows, "^v" up and down.
  struct Key {
    const char* cap;
    const char* label;
  };

  void begin();
  void showSplash();
  void finishSplash(uint32_t startedMs);
  void showToast(const char* text, uint32_t nowMs);
  bool render(Screen screen,
              const BrowseSnapshot& browse,
              const PlayerSnapshot& player,
              const Settings& settings,
              bool locked,
              uint32_t nowMs,
              bool force = false);

  void setDisplayOn(bool on);
  bool displayOn() const { return displayOn_; }
  void setBrightness(uint8_t b);

 private:
  void drawBrowse(const BrowseSnapshot& b, bool full);
  void drawPlaying(const PlayerSnapshot& p, bool full);
  void drawRecent(const BrowseSnapshot& b, const Settings& s, bool full);

  void drawPlayingProgress(const PlayerSnapshot& p);
  void drawSettings(const Settings& s);
  void drawSystem(const BrowseSnapshot& b, uint32_t nowMs, bool full);
  void drawBattery();
  void drawTopBar(const char* title, const char* detail);
  void drawKeys(const Key* keys, size_t count);
  void drawToastIfAny(uint32_t nowMs);
  void drawSplashWaves(int step);
  int text(const char* s, int x, int baseline, Face face, uint16_t fg, uint16_t bg,
           int maxWidth = 0, bool alignRight = false);
  int textWidth(const char* s, Face face);
  bool browseChanged(const BrowseSnapshot& b) const;
  bool playerChanged(const PlayerSnapshot& p) const;
  bool playerChromeChanged(const PlayerSnapshot& p) const;
  bool settingsChanged(const Settings& s) const;
  bool recentTimesChanged(const Settings& s) const;
  void rememberRecentTimes(const Settings& s);

  void rememberBrowse(const BrowseSnapshot& b);
  void rememberPlayer(const PlayerSnapshot& p);
  void rememberSettings(const Settings& s);
  void applyTheme(const Theme& t);

  Theme theme_{};
  Toast toast_{};
  Screen lastScreen_{Screen::Browse};
  bool hasLastBrowse_ = false;
  bool hasLastPlayer_ = false;
  bool hasLastSettings_ = false;
  bool displayOn_ = true;
  bool locked_ = false;
  BatteryReading battery_;
  uint32_t lastSystemMs_ = 0;

  char lastPath_[cfg::kMaxPathLen]{};
  DirEntry lastEntries_[cfg::kMaxDirEntries]{};
  size_t lastCount_ = 0;
  size_t lastCursor_ = 0;
  size_t lastScroll_ = 0;
  bool lastTruncated_ = false;
  bool lastSdOk_ = false;
  uint32_t lastRecentPos_[cfg::kRecentCount]{};
  size_t lastRecentCount_ = 0;

  PlayerSnapshot lastPlayer_{};
  SettingsSnapshot lastSettings_{};
  uint32_t lastToastExp_ = 0;
  char lastKeys_[80]{};
};
