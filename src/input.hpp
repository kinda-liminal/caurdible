#pragma once
#include "actions.hpp"
#include "types.hpp"

class Input {
 public:
  void begin();
  // Keys map to the same action on every screen.
  Action poll(Screen screen);
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
  bool waitForRelease_ = false;
};
