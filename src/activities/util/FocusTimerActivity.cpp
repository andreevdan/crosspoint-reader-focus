#include "FocusTimerActivity.h"

#include <Arduino.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long SIDEWAYS_HOLD_MS = 700;
constexpr uint16_t minutesLeft(uint32_t remaining, uint32_t elapsed) {
  return static_cast<uint16_t>((remaining - elapsed + 59999U) / 60000U);
}
static_assert(minutesLeft(120000, 60000) == 1 && minutesLeft(60000, 1) == 1);
constexpr int progressFillWidth(int width, int elapsedMinutes, int durationMinutes) {
  return width * elapsedMinutes / durationMinutes;
}
static_assert(progressFillWidth(100, 0, 10) == 0 && progressFillWidth(100, 5, 10) == 50 &&
              progressFillWidth(100, 10, 10) == 100);
constexpr uint8_t nextFlashFrame(uint8_t frame) { return frame == 6 ? 0 : frame + 1; }
static_assert(nextFlashFrame(5) == 6 && nextFlashFrame(6) == 0);
void drawPhaseBox(const GfxRenderer& renderer, Rect box, const char* label, int elapsedMinutes, int duration,
                  bool active) {
  renderer.drawRect(box.x, box.y, box.width, box.height, active ? 3 : 1, true);
  UITheme::drawCenteredText(renderer, box, UI_12_FONT_ID, box.y + 12, label, true,
                            active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  const int barX = box.x + 12;
  const int barY = box.y + box.height - 25;
  const int barWidth = box.width - 24;
  renderer.drawRect(barX, barY, barWidth, 14);
  if (elapsedMinutes > 0)
    renderer.fillRect(barX + 2, barY + 2, progressFillWidth(barWidth - 4, elapsedMinutes, duration), 10);
}
}  // namespace

uint8_t FocusTimerActivity::durationMinutes() const {
  return breakPhase ? SETTINGS.focusBreakMinutes : SETTINGS.focusMinutes;
}

void FocusTimerActivity::onEnter() {
  Activity::onEnter();
  previousOrientation = renderer.getOrientation();
  sideways = SETTINGS.focusSideways != 0;
  applySideways();
  remainingMs = static_cast<uint32_t>(durationMinutes()) * 60000U;
  shownMinutes = durationMinutes();
  requestUpdate();
}

void FocusTimerActivity::onExit() {
  sideways = false;
  applySideways();
  if (settingsChanged && !SETTINGS.saveToFile()) LOG_ERR("FOCUS", "Could not save timer durations");
  Activity::onExit();
}

// The only place that turns the screen, for entry, the hold and exit alike.
// Off means the orientation the UI had on entry, so nothing can be stranded.
void FocusTimerActivity::applySideways() {
  const GfxRenderer::Orientation target = sideways ? GfxRenderer::Orientation::LandscapeClockwise : previousOrientation;
  if (target == renderer.getOrientation()) return;
  renderer.setOrientation(target);
  // Turning rewrites every pixel, and this screen then sits static for a whole
  // phase: the differential waveform would leave the old frame on the panel.
  renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
}

// Session-only. SETTINGS.focusSideways stays the entry preference the Settings
// row owns; the hold never writes it back.
void FocusTimerActivity::toggleSideways() {
  sideways = !sideways;
  applySideways();
  requestUpdate();
}

void FocusTimerActivity::loop() {
  const uint32_t now = millis();
  if (running) {
    const uint32_t elapsed = now - startedAt;
    if (elapsed >= remainingMs) {
      RenderLock lock;
      remainingMs = 0;
      shownMinutes = 0;
      running = false;
      completed = true;
      flashFrame = 1;
      flashFrameAt = 0;
      requestUpdate();
      return;
    } else {
      const uint16_t minutes = minutesLeft(remainingMs, elapsed);
      if (minutes != shownMinutes) {
        RenderLock lock;
        shownMinutes = minutes;
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (flashFrame != 0) {
    RenderLock lock;
    if (flashFrameAt != 0 && now - flashFrameAt >= 120) {
      flashFrame = nextFlashFrame(flashFrame);
      flashFrameAt = 0;
      requestUpdate();
    }
    return;
  }
  // Must precede Confirm's release edge: a fired long press suppresses that
  // release, so the hold turns the screen without also starting the timer.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, SIDEWAYS_HOLD_MS)) {
    RenderLock lock;
    toggleSideways();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    RenderLock lock;
    if (running) {
      remainingMs -= now - startedAt;
      running = false;
    } else {
      if (completed) {
        breakPhase = !breakPhase;
        remainingMs = static_cast<uint32_t>(durationMinutes()) * 60000U;
        shownMinutes = durationMinutes();
      }
      startedAt = now;
      running = true;
      completed = false;
    }
    requestUpdate();
    return;
  }
  if (running) return;

  const bool decrease = mappedInput.wasReleased(MappedInputManager::Button::NavPrevious);
  const bool increase = mappedInput.wasReleased(MappedInputManager::Button::NavNext);
  if (!decrease && !increase) return;
  RenderLock lock;
  uint8_t& duration = breakPhase ? SETTINGS.focusBreakMinutes : SETTINGS.focusMinutes;
  const uint8_t limit = breakPhase ? 30 : 120;
  duration = std::clamp<int>(duration + (increase ? 1 : -1), 1, limit);
  remainingMs = static_cast<uint32_t>(duration) * 60000U;
  shownMinutes = duration;
  completed = false;
  settingsChanged = true;
  requestUpdate();
}

void FocusTimerActivity::render(RenderLock&&) {
  if (flashFrame != 0) {
    renderer.clearScreen((flashFrame & 1) ? 0x00 : 0xFF);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    flashFrameAt = millis();
    return;
  }
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  const int width = safe.width;
  const int height = safe.height;
  char time[12];
  snprintf(time, sizeof(time), "%u", shownMinutes);
  renderer.clearScreen();
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, safe.y + height / 10, tr(STR_FOCUS_TIMER), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, safe.y + height / 4,
                            breakPhase ? tr(STR_FOCUS_BREAK) : tr(STR_FOCUS_PHASE_FOCUS));
  UITheme::drawCenteredText(renderer, safe, NOTOSANS_18_FONT_ID, safe.y + height * 2 / 5, time, true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, safe.y + height / 2, tr(STR_FOCUS_MINUTES_LEFT));

  const int boxGap = 16;
  const int boxWidth = (width - 64 - boxGap) / 2;
  const int boxY = safe.y + height * 57 / 100;
  const int boxHeight = std::min(84, height / 5);
  const int focusDuration = std::max<int>(1, SETTINGS.focusMinutes);
  const int breakDuration = std::max<int>(1, SETTINGS.focusBreakMinutes);
  const int elapsedMinutes = std::max(0, static_cast<int>(durationMinutes()) - static_cast<int>(shownMinutes));
  drawPhaseBox(renderer, Rect{safe.x + (width - 2 * boxWidth - boxGap) / 2, boxY, boxWidth, boxHeight},
               tr(STR_FOCUS_PHASE_FOCUS), breakPhase ? focusDuration : elapsedMinutes, focusDuration, !breakPhase);
  drawPhaseBox(renderer, Rect{safe.x + (width + boxGap) / 2, boxY, boxWidth, boxHeight}, tr(STR_FOCUS_BREAK),
               breakPhase ? elapsedMinutes : 0, breakDuration, breakPhase);

  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, safe.y + height * 76 / 100,
                            breakPhase ? tr(STR_FOCUS_PROMPT_BREAK) : tr(STR_FOCUS_PROMPT_FOCUS));
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, safe.y + height * 84 / 100,
                            completed ? tr(STR_FOCUS_COMPLETE)
                            : running ? tr(STR_FOCUS_RUNNING)
                                      : tr(STR_FOCUS_READY));
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, safe.y + height * 92 / 100,
                            running ? tr(STR_FOCUS_HOLD_ROTATE) : tr(STR_FOCUS_ADJUST));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                            running     ? tr(STR_FOCUS_PAUSE)
                                            : completed ? tr(STR_FOCUS_NEXT)
                                                        : tr(STR_FOCUS_START),
                                            running ? "" : "-", running ? "" : "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
