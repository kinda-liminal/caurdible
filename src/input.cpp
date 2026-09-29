#include "input.hpp"
#include <M5Cardputer.h>

void Input::begin() {}

Action Input::poll(Screen screen) {
  // The caller must call M5Cardputer.update() before poll. App::loop does this.
  const bool changed = M5Cardputer.Keyboard.isChange();
  const uint8_t pressed = M5Cardputer.Keyboard.isPressed();
  if (pressed == 0) {
    waitForRelease_ = false;
    return Action::None;
  }

  const Keyboard_Class::KeysState& st = M5Cardputer.Keyboard.keysState();
  if (waitForRelease_) return Action::None;
  if (pressed == 2 && st.fn && st.word.size() == 1 &&
      (st.word[0] == 'l' || st.word[0] == 'L')) {
    locked_ = !locked_;
    waitForRelease_ = true;
    return Action::ToggleLock;
  }
  if (locked_ || !changed) return Action::None;

  // Special keys are not part of st.word.
  if (st.enter) {
    return Action::Enter;
  }
  if (st.space) {
    return Action::Space;
  }
  if (st.del) {
    return Action::Back;
  }
  if (st.tab) {
    return Action::Settings;
  }

  // Every key has one meaning on every screen. App decides what an arrow does on each screen.
  //        ;            up
  //     ,  .  /   left down right
  (void)screen;
  for (char key : st.word) {
    switch (key) {
      case ';': return Action::Up;
      case '.': return Action::Down;
      case ',': return Action::Left;
      case '/': return Action::Right;
      case '[': return Action::PrevTrack;
      case ']': return Action::NextTrack;
      case '-':
      case '_': return Action::VolDown;
      case '=':
      case '+': return Action::VolUp;
      case '9': return Action::SpeedDown;
      case '0': return Action::SpeedUp;
      case '`':
      case '~': return Action::Back;
      case ' ': return Action::Space;
      case 'p':
      case 'P': return Action::TogglePlayer;
      case 'r':
      case 'R': return Action::Recent;
      case 's':
      case 'S': return Action::Settings;
      case 'i':
      case 'I': return Action::System;
      default: break;
    }
  }

  return Action::None;
}
