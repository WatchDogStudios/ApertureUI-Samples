// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// GamepadNavigator.cpp - gamepad-to-UI navigation.

#include "GamepadNavigator.h"

#include <APHTML/API/APUIView.h>

#include <cmath>

namespace apui::samples
{
  GamepadNavigator::GamepadNavigator() = default;
  GamepadNavigator::~GamepadNavigator() = default;

  void GamepadNavigator::SetView(aperture::UIView* view)
  {
    m_view = view;
  }

  void GamepadNavigator::OnGamepadEvent(const aperture::input::GamepadEvent& event)
  {
    if (!m_view)
      return;

    switch (event.type)
    {
      case aperture::input::GamepadEvent::Type::ButtonDown:
      {
        switch (event.button)
        {
          case kButtonA:
            ClickFocused();
            break;
          case kButtonB:
            SendEscape();
            break;
          case kButtonStart:
            // Focus first element by moving focus forward from nothing
            m_view->moveFocus(false);
            break;
          case kButtonLeftShoulder:
            ScrollView(-kScrollAmount);
            break;
          case kButtonRightShoulder:
            ScrollView(kScrollAmount);
            break;
          case kButtonDpadUp:
            NavigateDirection(Direction::Up);
            break;
          case kButtonDpadDown:
            NavigateDirection(Direction::Down);
            break;
          case kButtonDpadLeft:
            NavigateDirection(Direction::Left);
            break;
          case kButtonDpadRight:
            NavigateDirection(Direction::Right);
            break;
          default:
            break;
        }
        break;
      }

      case aperture::input::GamepadEvent::Type::AxisMove:
      {
        // Translate left stick to directional navigation
        if (event.axis == kAxisLeftX || event.axis == kAxisLeftY)
        {
          float axisX = 0.0f;
          float axisY = 0.0f;

          if (event.axis == kAxisLeftX)
            axisX = event.axisValue;
          else
            axisY = event.axisValue;

          // Determine dominant direction
          Direction newDir = Direction::None;
          if (std::fabs(axisX) > std::fabs(axisY))
          {
            if (axisX > kStickDeadzone)
              newDir = Direction::Right;
            else if (axisX < -kStickDeadzone)
              newDir = Direction::Left;
          }
          else
          {
            if (axisY > kStickDeadzone)
              newDir = Direction::Down;
            else if (axisY < -kStickDeadzone)
              newDir = Direction::Up;
          }

          if (newDir != m_stickDirection)
          {
            m_stickDirection = newDir;
            m_repeatTimer = 0.0f;
            m_repeatFired = false;

            // Fire immediately on direction change
            if (newDir != Direction::None)
            {
              NavigateDirection(newDir);
              m_repeatFired = true;
            }
          }
        }
        break;
      }

      default:
        break;
    }
  }

  void GamepadNavigator::Update(float deltaSeconds)
  {
    if (m_stickDirection == Direction::None || !m_view)
      return;

    m_repeatTimer += deltaSeconds;

    // The first navigation fired on the direction change; repeats start after
    // m_repeatDelay and then come every m_repeatRate seconds.
    if (!m_repeatFired)
      return;

    if (m_repeatTimer >= m_repeatDelay + m_repeatRate)
    {
      NavigateDirection(m_stickDirection);
      m_repeatTimer = m_repeatDelay; // Reset to just after delay for next repeat
    }
  }

  void GamepadNavigator::NavigateDirection(Direction dir)
  {
    if (!m_view)
      return;

    // Spatial, as a d-pad works on a console menu: the nearest focusable element on
    // that side of the focused one (the first one when nothing has focus yet).
    m_view->moveFocusSpatial(static_cast<int>(dir));
  }

  void GamepadNavigator::ClickFocused()
  {
    if (!m_view)
      return;

    if (!m_view->hasFocus())
    {
      // No element focused - try to focus the first element
      m_view->moveFocus(false);
      return;
    }

    // Enter activates the focused element (a button's click, a link's navigation).
    m_view->onKeyDown("Enter", "Enter");
    m_view->onKeyUp("Enter", "Enter");
  }

  void GamepadNavigator::SendEscape()
  {
    if (!m_view)
      return;

    m_view->onKeyDown("Escape", "Escape");
    m_view->onKeyUp("Escape", "Escape");
  }

  void GamepadNavigator::ScrollView(float deltaY)
  {
    if (!m_view)
      return;

    m_view->onWheel(0.0f, 0.0f, 0.0f, deltaY); // pixel deltas
  }
} // namespace apui::samples
