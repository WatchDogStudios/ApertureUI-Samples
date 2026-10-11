// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// GamepadNavigator.h - translates gamepad input (D-pad, buttons, sticks) into
// UIView focus navigation and interaction events, so a page can be used with a
// controller.

#pragma once

#include <APHTML/Input/InputAdapter.h>
#include <cstdint>

namespace aperture
{
  class UIView;
}

namespace apui::samples
{
  /**
   * Translates gamepad events into UIView input actions.
   *
   * Button mapping (SDL GameController standard layout):
   *   D-pad / Left Stick  -> Move focus to the nearest element in that direction
   *   A (South)           -> Activate the focused element (Enter)
   *   B (East)            -> Escape key
   *   Start               -> Focus first element
   *   Left Shoulder       -> Scroll up
   *   Right Shoulder      -> Scroll down
   *
   * Call Update() each frame for stick-based repeat navigation.
   */
  class GamepadNavigator
  {
  public:
    GamepadNavigator();
    ~GamepadNavigator();

    /// Set the view to drive. Pass nullptr before the view is destroyed, and the
    /// new view after a host replaces it.
    void SetView(aperture::UIView* view);

    /// Process a raw gamepad event from the input adapter.
    void OnGamepadEvent(const aperture::input::GamepadEvent& event);

    /// Per-frame update for repeat navigation from held stick directions.
    /// @param deltaSeconds Frame delta time in seconds.
    void Update(float deltaSeconds);

  private:
    // SDL_GameControllerButton constants (matching SDL2 enum values)
    static constexpr uint32_t kButtonA = 0;
    static constexpr uint32_t kButtonB = 1;
    static constexpr uint32_t kButtonStart = 6;
    static constexpr uint32_t kButtonLeftShoulder = 9;
    static constexpr uint32_t kButtonRightShoulder = 10;
    static constexpr uint32_t kButtonDpadUp = 11;
    static constexpr uint32_t kButtonDpadDown = 12;
    static constexpr uint32_t kButtonDpadLeft = 13;
    static constexpr uint32_t kButtonDpadRight = 14;

    // SDL_GameControllerAxis constants
    static constexpr uint32_t kAxisLeftX = 0;
    static constexpr uint32_t kAxisLeftY = 1;

    // Deadzone for analog stick to count as a directional input
    static constexpr float kStickDeadzone = 0.5f;

    // Scroll amount per shoulder button press (in pixels)
    static constexpr float kScrollAmount = 120.0f;

    // The values are UIView::moveFocusSpatial's directions.
    enum class Direction : int8_t
    {
      None = -1,
      Left = 0,
      Up = 1,
      Right = 2,
      Down = 3
    };

    void NavigateDirection(Direction dir);
    void ClickFocused();
    void SendEscape();
    void ScrollView(float deltaY);

    aperture::UIView* m_view = nullptr;

    // Stick repeat state
    Direction m_stickDirection = Direction::None;
    float m_repeatTimer = 0.0f;
    float m_repeatDelay = 0.4f; // Initial delay before repeat starts
    float m_repeatRate = 0.15f; // Interval between repeats
    bool m_repeatFired = false; // Whether the initial navigation already fired
  };
} // namespace apui::samples
