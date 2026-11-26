#pragma once

#include <cstdint>

#include "activities/Activity.h"

class FocusTimerActivity final : public Activity {
  bool breakPhase = false;
  bool running = false;
  bool completed = false;
  bool settingsChanged = false;
  bool sideways = false;
  uint8_t flashFrame = 0;
  uint32_t remainingMs = 0;
  uint32_t startedAt = 0;
  uint32_t flashFrameAt = 0;
  uint16_t shownMinutes = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;

  uint8_t durationMinutes() const;
  void applySideways();
  void toggleSideways();

 public:
  FocusTimerActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("FocusTimer", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return running || flashFrame != 0; }
};
