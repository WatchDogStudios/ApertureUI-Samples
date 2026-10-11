// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// SDL2InputAdapter.cpp - SDL2 windowing and input backend for the samples.

#include "SDL2InputAdapter.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

using namespace aperture::input;

namespace
{
  constexpr uint32_t kSdlSubsystems
    = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
}

namespace apui::samples
{

  SDL2InputAdapter::SDL2InputAdapter()
    : m_config()
  {
  }

  SDL2InputAdapter::SDL2InputAdapter(const WindowConfig& config)
    : m_config(config)
  {
  }

  SDL2InputAdapter::~SDL2InputAdapter()
  {
    Shutdown();
  }

  bool SDL2InputAdapter::Initialize()
  {
    if (m_initialized)
      return true;

    // Start SDL unless the host already has; Shutdown() stops only what this started.
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
    {
      if (SDL_Init(kSdlSubsystems) != 0)
      {
        return false;
      }
      m_startedSdl = true;
    }

    // High-DPI support
    if (m_config.highDpi)
    {
      SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
#if SDL_VERSION_ATLEAST(2, 24, 0)
      SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    }

    // Create window if not attached
    if (!m_window)
    {
      uint32_t flags = SDL_WINDOW_SHOWN;

      if (m_config.resizable)
        flags |= SDL_WINDOW_RESIZABLE;
      if (m_config.borderless)
        flags |= SDL_WINDOW_BORDERLESS;
      if (m_config.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
      else if (m_config.fullscreenDesktop)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
      if (m_config.highDpi)
        flags |= SDL_WINDOW_ALLOW_HIGHDPI;

      int x = m_config.x >= 0 ? m_config.x : SDL_WINDOWPOS_CENTERED;
      int y = m_config.y >= 0 ? m_config.y : SDL_WINDOWPOS_CENTERED;

      m_window = SDL_CreateWindow(m_config.title.c_str(), x, y,
        static_cast<int>(m_config.width), static_cast<int>(m_config.height), flags);

      if (!m_window)
      {
        Shutdown(); // stops SDL if this started it
        return false;
      }

      m_ownsWindow = true;

      // Set size limits
      if (m_config.minWidth > 0 || m_config.minHeight > 0)
      {
        SDL_SetWindowMinimumSize(m_window, static_cast<int>(m_config.minWidth),
          static_cast<int>(m_config.minHeight));
      }

      if (m_config.maxWidth > 0 || m_config.maxHeight > 0)
      {
        SDL_SetWindowMaximumSize(m_window, static_cast<int>(m_config.maxWidth),
          static_cast<int>(m_config.maxHeight));
      }
    }

    RefreshDpiScale();

    LoadCursors();

    // Open any connected gamepads
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
      OpenGamepad(i);

    m_initialized = true;
    return true;
  }

  void SDL2InputAdapter::OpenGamepad(int deviceIndex)
  {
    if (!SDL_IsGameController(deviceIndex))
      return;
    // SDL also reports every controller present at start-up as a
    // SDL_CONTROLLERDEVICEADDED event; opening it again would take a second
    // reference that Shutdown() never releases.
    const int instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    if (m_gamepads.count(instanceId) != 0)
      return;
    SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
    if (!controller)
      return;
    GamepadInfo info;
    info.controller = controller;
    info.id = static_cast<uint32_t>(deviceIndex);
    m_gamepads[instanceId] = info;
  }

  void SDL2InputAdapter::Shutdown()
  {
    if (m_initialized)
    {
      // Close gamepads
      for (auto& [id, info] : m_gamepads)
      {
        if (info.controller)
        {
          SDL_GameControllerClose(static_cast<SDL_GameController*>(info.controller));
        }
      }
      m_gamepads.clear();

      FreeCursors();

      if (m_window && m_ownsWindow)
      {
        SDL_DestroyWindow(m_window);
      }
      m_window = nullptr;

      m_initialized = false;
    }

    if (m_startedSdl)
    {
      SDL_QuitSubSystem(kSdlSubsystems);
      m_startedSdl = false;
    }
  }

  void SDL2InputAdapter::SetRawSDLEventHook(RawSDLEventHook hook)
  {
    m_rawHook = std::move(hook);
  }

  void SDL2InputAdapter::PollEvents()
  {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      if (m_rawHook)
        m_rawHook(event);
      ProcessEvent(event);
    }
  }

  void* SDL2InputAdapter::GetNativeWindowHandle() const
  {
    if (!m_window)
      return nullptr;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(m_window, &info))
    {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
      return info.info.win.window;
#elif defined(SDL_VIDEO_DRIVER_X11)
      return reinterpret_cast<void*>(info.info.x11.window);
#elif defined(SDL_VIDEO_DRIVER_COCOA)
      return info.info.cocoa.window;
#endif
    }

    return nullptr;
  }

  nsVec2 SDL2InputAdapter::GetWindowSize() const
  {
    if (!m_window)
      return nsVec2(0.0f, 0.0f);

    int w, h;
    SDL_GetWindowSize(m_window, &w, &h);
    return nsVec2(static_cast<float>(w), static_cast<float>(h));
  }

  nsVec2 SDL2InputAdapter::GetClientSize() const
  {
    if (!m_window)
      return nsVec2(0.0f, 0.0f);

    int w, h;
    SDL_GL_GetDrawableSize(m_window, &w, &h);
    return nsVec2(static_cast<float>(w), static_cast<float>(h));
  }

  float SDL2InputAdapter::GetDpiScale() const
  {
    return m_dpiScale;
  }

  void SDL2InputAdapter::RefreshDpiScale()
  {
    int drawW = 0, drawH = 0, winW = 0, winH = 0;
    SDL_GL_GetDrawableSize(m_window, &drawW, &drawH);
    SDL_GetWindowSize(m_window, &winW, &winH);
    if (winW > 0)
    {
      m_dpiScale = static_cast<float>(drawW) / static_cast<float>(winW);
    }
  }

  void SDL2InputAdapter::SetCursor(const std::string& cursorName)
  {
    auto it = m_cursors.find(cursorName);
    if (it != m_cursors.end())
    {
      m_currentCursor = it->second;
      SDL_SetCursor(static_cast<SDL_Cursor*>(it->second));
    }
  }

  void SDL2InputAdapter::SetCursorVisible(bool visible)
  {
    m_cursorVisible = visible;
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
  }

  void SDL2InputAdapter::SetCursorPosition(const nsVec2& position)
  {
    if (m_window)
    {
      SDL_WarpMouseInWindow(
        m_window, static_cast<int>(position.x), static_cast<int>(position.y));
    }
  }

  nsVec2 SDL2InputAdapter::GetCursorPosition() const
  {
    return m_cursorPosition;
  }

  std::string SDL2InputAdapter::GetClipboardText() const
  {
    char* text = SDL_GetClipboardText();
    if (text)
    {
      std::string result(text);
      SDL_free(text);
      return result;
    }
    return "";
  }

  void SDL2InputAdapter::SetClipboardText(const std::string& text)
  {
    SDL_SetClipboardText(text.c_str());
  }

  bool SDL2InputAdapter::IsKeyDown(KeyCode key) const
  {
    auto it = m_keyStates.find(key);
    return it != m_keyStates.end() && it->second;
  }

  bool SDL2InputAdapter::IsMouseButtonDown(MouseButton button) const
  {
    auto it = m_mouseButtonStates.find(button);
    return it != m_mouseButtonStates.end() && it->second;
  }

  SDL2InputAdapter::ModifierKeys SDL2InputAdapter::GetCurrentModifiers() const
  {
    return m_currentModifiers;
  }

  uint32_t SDL2InputAdapter::GetWindowId() const
  {
    return m_window ? SDL_GetWindowID(m_window) : 0;
  }

  void SDL2InputAdapter::AttachWindow(SDL_Window* window)
  {
    if (m_initialized)
      return;

    m_window = window;
    m_ownsWindow = false;
  }

  void SDL2InputAdapter::SetWindowTitle(const std::string& title)
  {
    if (m_window)
    {
      SDL_SetWindowTitle(m_window, title.c_str());
    }
  }

  void SDL2InputAdapter::SetWindowSize(uint32_t width, uint32_t height)
  {
    if (m_window)
    {
      SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
    }
  }

  void SDL2InputAdapter::SetWindowPosition(int32_t x, int32_t y)
  {
    if (m_window)
    {
      SDL_SetWindowPosition(m_window, x, y);
    }
  }

  void SDL2InputAdapter::SetFullscreen(bool fullscreen, bool desktop)
  {
    if (m_window)
    {
      uint32_t flags = 0;
      if (fullscreen)
      {
        flags = desktop ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
      }
      SDL_SetWindowFullscreen(m_window, flags);
    }
  }

  bool SDL2InputAdapter::HasFocus() const
  {
    if (!m_window)
      return false;

    uint32_t flags = SDL_GetWindowFlags(m_window);
    return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
  }

  void SDL2InputAdapter::ProcessEvent(const SDL_Event& event)
  {
    switch (event.type)
    {
      case SDL_MOUSEMOTION:
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
      case SDL_MOUSEWHEEL:
        ProcessMouseEvent(event);
        break;

      case SDL_KEYDOWN:
      case SDL_KEYUP:
      case SDL_TEXTINPUT:
        ProcessKeyEvent(event);
        break;

      case SDL_WINDOWEVENT:
        ProcessWindowEvent(event);
        break;

      case SDL_FINGERDOWN:
      case SDL_FINGERUP:
      case SDL_FINGERMOTION:
        ProcessTouchEvent(event);
        break;

      case SDL_CONTROLLERDEVICEADDED:
      case SDL_CONTROLLERDEVICEREMOVED:
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP:
      case SDL_CONTROLLERAXISMOTION:
        ProcessGamepadEvent(event);
        break;

      case SDL_QUIT:
      {
        WindowEvent we;
        we.type = WindowEvent::Type::Close;
        we.timestamp = event.quit.timestamp;
        DispatchWindowEvent(we);
      }
      break;
    }
  }

  void SDL2InputAdapter::ProcessMouseEvent(const SDL_Event& event)
  {
    MouseEvent me;
    me.timestamp = SDL_GetTicks64();
    me.modifiers = m_currentModifiers;

    switch (event.type)
    {
      case SDL_MOUSEMOTION:
        me.type = MouseEvent::Type::Move;
        me.position.x = static_cast<float>(event.motion.x);
        me.position.y = static_cast<float>(event.motion.y);
        m_cursorPosition = me.position;
        break;

      case SDL_MOUSEBUTTONDOWN:
      {
        me.type = MouseEvent::Type::ButtonDown;
        me.position.x = static_cast<float>(event.button.x);
        me.position.y = static_cast<float>(event.button.y);
        me.button = SDLButtonToMouseButton(event.button.button);
        UpdateMouseButtonState(me.button, true);

        // Double-click detection
        auto& click = m_clickInfo[me.button];
        float distSq = (me.position.x - click.lastClickPosition.x)
            * (me.position.x - click.lastClickPosition.x)
          + (me.position.y - click.lastClickPosition.y)
            * (me.position.y - click.lastClickPosition.y);

        if (me.timestamp - click.lastClickTime < kDoubleClickTimeMs
          && distSq < kDoubleClickDistanceSq)
        {
          click.clickCount++;
        }
        else
        {
          click.clickCount = 1;
        }

        click.lastClickTime = me.timestamp;
        click.lastClickPosition = me.position;
        me.clickCount = click.clickCount;
      }
      break;

      case SDL_MOUSEBUTTONUP:
        me.type = MouseEvent::Type::ButtonUp;
        me.position.x = static_cast<float>(event.button.x);
        me.position.y = static_cast<float>(event.button.y);
        me.button = SDLButtonToMouseButton(event.button.button);
        UpdateMouseButtonState(me.button, false);
        break;

      case SDL_MOUSEWHEEL:
      {
        me.type = MouseEvent::Type::Wheel;
        me.position = m_cursorPosition;

        // SDL2 wheel events are in scroll lines by default
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (event.wheel.preciseX != 0.0f || event.wheel.preciseY != 0.0f)
        {
          me.wheelDeltaX = event.wheel.preciseX;
          me.wheelDeltaY = event.wheel.preciseY;
          me.wheelMode = WheelDeltaMode::Pixel;
        }
        else
#endif
        {
          me.wheelDeltaX = static_cast<float>(event.wheel.x);
          me.wheelDeltaY = static_cast<float>(event.wheel.y);
          me.wheelMode = WheelDeltaMode::Line;
        }

        // Handle flipped direction
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
        {
          me.wheelDeltaX = -me.wheelDeltaX;
          me.wheelDeltaY = -me.wheelDeltaY;
        }
      }
      break;
    }

    // Get global position
    int gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    me.globalPosition.x = static_cast<float>(gx);
    me.globalPosition.y = static_cast<float>(gy);

    DispatchMouseEvent(me);
  }

  void SDL2InputAdapter::ProcessKeyEvent(const SDL_Event& event)
  {
    if (event.type == SDL_TEXTINPUT)
    {
      KeyEvent ke;
      ke.type = KeyEvent::Type::Character;
      ke.textUtf8 = event.text.text;
      ke.timestamp = SDL_GetTicks64();
      ke.modifiers = m_currentModifiers;

      // Get first UTF-32 codepoint
      if (!ke.textUtf8.empty())
      {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ke.textUtf8.data());
        if ((*p & 0x80) == 0)
        {
          ke.character = *p;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
          ke.character = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
        }
        else if ((*p & 0xF0) == 0xE0)
        {
          ke.character
            = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
        }
        else if ((*p & 0xF8) == 0xF0)
        {
          ke.character = ((*p & 0x07) << 18) | ((*(p + 1) & 0x3F) << 12)
            | ((*(p + 2) & 0x3F) << 6) | (*(p + 3) & 0x3F);
        }
      }

      DispatchKeyEvent(ke);
      return;
    }

    KeyEvent ke;
    ke.type = event.type == SDL_KEYDOWN ? KeyEvent::Type::KeyDown : KeyEvent::Type::KeyUp;
    ke.keyCode = SDLKeyToKeyCode(event.key.keysym.sym);
    ke.nativeKeyCode = event.key.keysym.scancode;
    ke.isRepeat = event.key.repeat != 0;
    ke.timestamp = SDL_GetTicks64();

    UpdateModifiers(event.key.keysym.mod);
    ke.modifiers = m_currentModifiers;

    UpdateKeyState(ke.keyCode, event.type == SDL_KEYDOWN);

    DispatchKeyEvent(ke);
  }

  void SDL2InputAdapter::ProcessWindowEvent(const SDL_Event& event)
  {
    WindowEvent we;
    we.timestamp = SDL_GetTicks64();

    switch (event.window.event)
    {
      case SDL_WINDOWEVENT_RESIZED:
      case SDL_WINDOWEVENT_SIZE_CHANGED:
        we.type = WindowEvent::Type::Resize;
        we.width = static_cast<uint32_t>(event.window.data1);
        we.height = static_cast<uint32_t>(event.window.data2);
        RefreshDpiScale();
        break;

      case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        // The window moved to another monitor, which may have another DPI scale.
        we.type = WindowEvent::Type::DpiChanged;
        RefreshDpiScale();
        we.dpiScale = m_dpiScale;
        break;

      case SDL_WINDOWEVENT_MOVED:
        we.type = WindowEvent::Type::Move;
        we.x = event.window.data1;
        we.y = event.window.data2;
        break;

      case SDL_WINDOWEVENT_FOCUS_GAINED:
        we.type = WindowEvent::Type::Focus;
        break;

      case SDL_WINDOWEVENT_FOCUS_LOST:
        we.type = WindowEvent::Type::Blur;
        break;

      case SDL_WINDOWEVENT_CLOSE:
        we.type = WindowEvent::Type::Close;
        break;

      case SDL_WINDOWEVENT_MINIMIZED:
        we.type = WindowEvent::Type::Minimize;
        break;

      case SDL_WINDOWEVENT_MAXIMIZED:
        we.type = WindowEvent::Type::Maximize;
        break;

      case SDL_WINDOWEVENT_RESTORED:
        we.type = WindowEvent::Type::Restore;
        break;

      case SDL_WINDOWEVENT_ENTER:
      {
        MouseEvent me;
        me.type = MouseEvent::Type::Enter;
        me.position = m_cursorPosition;
        me.timestamp = we.timestamp;
        DispatchMouseEvent(me);
        return;
      }

      case SDL_WINDOWEVENT_LEAVE:
      {
        MouseEvent me;
        me.type = MouseEvent::Type::Leave;
        me.position = m_cursorPosition;
        me.timestamp = we.timestamp;
        DispatchMouseEvent(me);
        return;
      }

      default:
        return;
    }

    DispatchWindowEvent(we);
  }

  void SDL2InputAdapter::ProcessTouchEvent(const SDL_Event& event)
  {
    TouchEvent te;
    te.timestamp = SDL_GetTicks64();
    te.modifiers = m_currentModifiers;

    switch (event.type)
    {
      case SDL_FINGERDOWN:
        te.type = TouchEvent::Type::Begin;
        break;
      case SDL_FINGERUP:
        te.type = TouchEvent::Type::End;
        break;
      case SDL_FINGERMOTION:
        te.type = TouchEvent::Type::Move;
        break;
    }

    nsVec2 windowSize = GetWindowSize();

    TouchPoint point;
    point.id = event.tfinger.fingerId;
    point.x = event.tfinger.x * windowSize.x;
    point.y = event.tfinger.y * windowSize.y;
    point.force = event.tfinger.pressure;
    point.radiusX = 1.0f; // SDL2 doesn't provide touch radius
    point.radiusY = 1.0f;

    te.points.push_back(point);

    DispatchTouchEvent(te);
  }

  void SDL2InputAdapter::ProcessGamepadEvent(const SDL_Event& event)
  {
    GamepadEvent ge;
    ge.timestamp = SDL_GetTicks64();

    switch (event.type)
    {
      case SDL_CONTROLLERDEVICEADDED:
        // `which` is the device index here, the instance id in every other event.
        ge.type = GamepadEvent::Type::Connected;
        ge.gamepadId = static_cast<uint32_t>(event.cdevice.which);
        OpenGamepad(event.cdevice.which);
        break;

      case SDL_CONTROLLERDEVICEREMOVED:
      {
        ge.type = GamepadEvent::Type::Disconnected;

        auto it = m_gamepads.find(event.cdevice.which);
        if (it != m_gamepads.end())
        {
          ge.gamepadId = it->second.id;
          if (it->second.controller)
          {
            SDL_GameControllerClose(
              static_cast<SDL_GameController*>(it->second.controller));
          }
          m_gamepads.erase(it);
        }
      }
      break;

      case SDL_CONTROLLERBUTTONDOWN:
        ge.type = GamepadEvent::Type::ButtonDown;
        ge.gamepadId = static_cast<uint32_t>(event.cbutton.which);
        ge.button = event.cbutton.button;
        break;

      case SDL_CONTROLLERBUTTONUP:
        ge.type = GamepadEvent::Type::ButtonUp;
        ge.gamepadId = static_cast<uint32_t>(event.cbutton.which);
        ge.button = event.cbutton.button;
        break;

      case SDL_CONTROLLERAXISMOTION:
        ge.type = GamepadEvent::Type::AxisMove;
        ge.gamepadId = static_cast<uint32_t>(event.caxis.which);
        ge.axis = event.caxis.axis;
        ge.axisValue = static_cast<float>(event.caxis.value) / 32767.0f;
        break;
    }

    DispatchGamepadEvent(ge);
  }

  void SDL2InputAdapter::UpdateKeyState(KeyCode key, bool down)
  {
    m_keyStates[key] = down;
  }

  void SDL2InputAdapter::UpdateMouseButtonState(MouseButton button, bool down)
  {
    m_mouseButtonStates[button] = down;
  }

  void SDL2InputAdapter::UpdateModifiers(uint16_t sdlMods)
  {
    m_currentModifiers.shift = (sdlMods & KMOD_SHIFT) != 0;
    m_currentModifiers.ctrl = (sdlMods & KMOD_CTRL) != 0;
    m_currentModifiers.alt = (sdlMods & KMOD_ALT) != 0;
    m_currentModifiers.capsLock = (sdlMods & KMOD_CAPS) != 0;
    m_currentModifiers.numLock = (sdlMods & KMOD_NUM) != 0;
  }

  void SDL2InputAdapter::LoadCursors()
  {
    m_cursors["default"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    m_cursors["pointer"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    m_cursors["text"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    m_cursors["crosshair"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    m_cursors["wait"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
    m_cursors["progress"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAITARROW);
    m_cursors["move"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
    m_cursors["not-allowed"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NO);
    m_cursors["ew-resize"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    m_cursors["ns-resize"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
    m_cursors["nesw-resize"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);
    m_cursors["nwse-resize"] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);

    m_currentCursor = m_cursors["default"];
  }

  void SDL2InputAdapter::FreeCursors()
  {
    for (auto& [name, cursor] : m_cursors)
    {
      if (cursor)
      {
        SDL_FreeCursor(static_cast<SDL_Cursor*>(cursor));
      }
    }
    m_cursors.clear();
    m_currentCursor = nullptr;
  }

  SDL2InputAdapter::KeyCode SDL2InputAdapter::SDLKeyToKeyCode(int sdlKey)
  {
    // Map SDL keycodes to our KeyCode enum
    if (sdlKey >= SDLK_a && sdlKey <= SDLK_z)
      return static_cast<KeyCode>('A' + (sdlKey - SDLK_a));

    if (sdlKey >= SDLK_0 && sdlKey <= SDLK_9)
      return static_cast<KeyCode>('0' + (sdlKey - SDLK_0));

    if (sdlKey >= SDLK_F1 && sdlKey <= SDLK_F12)
      return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (sdlKey - SDLK_F1));

    switch (sdlKey)
    {
      case SDLK_ESCAPE:
        return KeyCode::Escape;
      case SDLK_TAB:
        return KeyCode::Tab;
      case SDLK_CAPSLOCK:
        return KeyCode::CapsLock;
      case SDLK_BACKSPACE:
        return KeyCode::Backspace;
      case SDLK_RETURN:
        return KeyCode::Enter;
      case SDLK_SPACE:
        return KeyCode::Space;

      case SDLK_INSERT:
        return KeyCode::Insert;
      case SDLK_DELETE:
        return KeyCode::Delete;
      case SDLK_HOME:
        return KeyCode::Home;
      case SDLK_END:
        return KeyCode::End;
      case SDLK_PAGEUP:
        return KeyCode::PageUp;
      case SDLK_PAGEDOWN:
        return KeyCode::PageDown;

      case SDLK_LEFT:
        return KeyCode::Left;
      case SDLK_RIGHT:
        return KeyCode::Right;
      case SDLK_UP:
        return KeyCode::Up;
      case SDLK_DOWN:
        return KeyCode::Down;

      case SDLK_LSHIFT:
        return KeyCode::LeftShift;
      case SDLK_RSHIFT:
        return KeyCode::RightShift;
      case SDLK_LCTRL:
        return KeyCode::LeftControl;
      case SDLK_RCTRL:
        return KeyCode::RightControl;
      case SDLK_LALT:
        return KeyCode::LeftAlt;
      case SDLK_RALT:
        return KeyCode::RightAlt;
      case SDLK_LGUI:
        return KeyCode::LeftSuper;
      case SDLK_RGUI:
        return KeyCode::RightSuper;

      case SDLK_KP_0:
        return KeyCode::Numpad0;
      case SDLK_KP_1:
        return KeyCode::Numpad1;
      case SDLK_KP_2:
        return KeyCode::Numpad2;
      case SDLK_KP_3:
        return KeyCode::Numpad3;
      case SDLK_KP_4:
        return KeyCode::Numpad4;
      case SDLK_KP_5:
        return KeyCode::Numpad5;
      case SDLK_KP_6:
        return KeyCode::Numpad6;
      case SDLK_KP_7:
        return KeyCode::Numpad7;
      case SDLK_KP_8:
        return KeyCode::Numpad8;
      case SDLK_KP_9:
        return KeyCode::Numpad9;
      case SDLK_KP_PERIOD:
        return KeyCode::NumpadDecimal;
      case SDLK_KP_ENTER:
        return KeyCode::NumpadEnter;
      case SDLK_KP_PLUS:
        return KeyCode::NumpadAdd;
      case SDLK_KP_MINUS:
        return KeyCode::NumpadSubtract;
      case SDLK_KP_MULTIPLY:
        return KeyCode::NumpadMultiply;
      case SDLK_KP_DIVIDE:
        return KeyCode::NumpadDivide;
      case SDLK_NUMLOCKCLEAR:
        return KeyCode::NumLock;

      case SDLK_BACKQUOTE:
        return KeyCode::Grave;
      case SDLK_MINUS:
        return KeyCode::Minus;
      case SDLK_EQUALS:
        return KeyCode::Equals;
      case SDLK_LEFTBRACKET:
        return KeyCode::LeftBracket;
      case SDLK_RIGHTBRACKET:
        return KeyCode::RightBracket;
      case SDLK_BACKSLASH:
        return KeyCode::Backslash;
      case SDLK_SEMICOLON:
        return KeyCode::Semicolon;
      case SDLK_QUOTE:
        return KeyCode::Apostrophe;
      case SDLK_COMMA:
        return KeyCode::Comma;
      case SDLK_PERIOD:
        return KeyCode::Period;
      case SDLK_SLASH:
        return KeyCode::Slash;

      case SDLK_PRINTSCREEN:
        return KeyCode::PrintScreen;
      case SDLK_SCROLLLOCK:
        return KeyCode::ScrollLock;
      case SDLK_PAUSE:
        return KeyCode::Pause;
      case SDLK_MENU:
        return KeyCode::Menu;

      default:
        return KeyCode::Unknown;
    }
  }

  SDL2InputAdapter::MouseButton SDL2InputAdapter::SDLButtonToMouseButton(uint8_t button)
  {
    switch (button)
    {
      case SDL_BUTTON_LEFT:
        return MouseButton::Left;
      case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
      case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
      case SDL_BUTTON_X1:
        return MouseButton::Back;
      case SDL_BUTTON_X2:
        return MouseButton::Forward;
      default:
        return MouseButton::None;
    }
  }

} // namespace apui::samples
