// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// SDL2InputAdapter.h - SDL2 windowing and input backend for the samples.

#pragma once

#include <APHTML/Input/InputAdapter.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

// Forward declarations for SDL types (avoid including SDL.h in header)
struct SDL_Window;
union SDL_Event;

namespace apui::samples
{
  /**
   * SDL2-based input adapter providing windowing and input.
   *
   * Features:
   * - Window creation and management
   * - Mouse, keyboard, touch, and gamepad input
   * - High-DPI support
   * - Clipboard access
   * - System cursor handling
   *
   * Initialize() starts SDL's video, events and game controller subsystems
   * when the host has not already done so, and Shutdown() then stops them.
   */
  class SDL2InputAdapter final : public aperture::input::IInputAdapter
  {
  public:
    using KeyCode = aperture::input::KeyCode;
    using MouseButton = aperture::input::MouseButton;
    using ModifierKeys = aperture::input::ModifierKeys;

    /**
     * Configuration for window creation.
     */
    struct WindowConfig
    {
      std::string title = "Aperture UI";
      int32_t x = -1; // -1 = centered
      int32_t y = -1;
      uint32_t width = 1280;
      uint32_t height = 720;
      bool resizable = true;
      bool borderless = false;
      bool fullscreen = false;
      bool fullscreenDesktop = false;
      bool highDpi = true;
      bool allowHighDpiScaling = true;
      uint32_t minWidth = 0;
      uint32_t minHeight = 0;
      uint32_t maxWidth = 0;
      uint32_t maxHeight = 0;
    };

    SDL2InputAdapter();
    explicit SDL2InputAdapter(const WindowConfig& config);
    ~SDL2InputAdapter() override;

    // Non-copyable
    SDL2InputAdapter(const SDL2InputAdapter&) = delete;
    SDL2InputAdapter& operator=(const SDL2InputAdapter&) = delete;

    // IInputAdapter implementation
    bool Initialize() override;
    void Shutdown() override;
    void PollEvents() override;

    void* GetNativeWindowHandle() const override;
    nsVec2 GetWindowSize() const override;
    nsVec2 GetClientSize() const override;
    float GetDpiScale() const override;

    void SetCursor(const std::string& cursorName) override;
    void SetCursorVisible(bool visible) override;
    void SetCursorPosition(const nsVec2& position) override;
    nsVec2 GetCursorPosition() const override;

    std::string GetClipboardText() const override;
    void SetClipboardText(const std::string& text) override;

    bool IsKeyDown(KeyCode key) const override;
    bool IsMouseButtonDown(MouseButton button) const override;
    ModifierKeys GetCurrentModifiers() const override;

    // SDL2-specific methods
    SDL_Window* GetSDLWindow() const { return m_window; }
    uint32_t GetWindowId() const;

    /**
     * Attach to an existing SDL_Window instead of creating one.
     * The adapter will not destroy the window on shutdown.
     */
    void AttachWindow(SDL_Window* window);

    /**
     * Register a hook that receives every raw SDL_Event before the adapter
     * dispatches it. Use this to feed events to ImGui (ImGui_ImplSDL2_ProcessEvent).
     */
    using RawSDLEventHook = std::function<void(const SDL_Event&)>;
    void SetRawSDLEventHook(RawSDLEventHook hook);

    /**
     * Set window title.
     */
    void SetWindowTitle(const std::string& title);

    /**
     * Set window size.
     */
    void SetWindowSize(uint32_t width, uint32_t height);

    /**
     * Set window position.
     */
    void SetWindowPosition(int32_t x, int32_t y);

    /**
     * Set fullscreen mode.
     */
    void SetFullscreen(bool fullscreen, bool desktop = true);

    /**
     * Check if window is focused.
     */
    bool HasFocus() const;

  private:
    void ProcessEvent(const SDL_Event& event);
    void ProcessMouseEvent(const SDL_Event& event);
    void ProcessKeyEvent(const SDL_Event& event);
    void ProcessWindowEvent(const SDL_Event& event);
    void ProcessTouchEvent(const SDL_Event& event);
    void ProcessGamepadEvent(const SDL_Event& event);

    void UpdateKeyState(KeyCode key, bool down);
    void UpdateMouseButtonState(MouseButton button, bool down);
    void UpdateModifiers(uint16_t sdlMods);
    void OpenGamepad(int deviceIndex);
    void RefreshDpiScale(); // drawable size / window size
    void LoadCursors();
    void FreeCursors();

    static KeyCode SDLKeyToKeyCode(int sdlKey);
    static MouseButton SDLButtonToMouseButton(uint8_t button);

    WindowConfig m_config;
    SDL_Window* m_window = nullptr;
    bool m_ownsWindow = true;
    bool m_initialized = false;
    bool m_startedSdl = false; // Initialize() called SDL_Init, so Shutdown() quits it
    float m_dpiScale = 1.0f;
    RawSDLEventHook m_rawHook;

    // Input state tracking
    std::unordered_map<KeyCode, bool> m_keyStates;
    std::unordered_map<MouseButton, bool> m_mouseButtonStates;
    ModifierKeys m_currentModifiers { };
    nsVec2 m_cursorPosition { 0.0f, 0.0f };

    // Cursors - use void* to avoid conflict with SDL's SDL_Cursor type
    std::unordered_map<std::string, void*> m_cursors;
    void* m_currentCursor = nullptr;
    bool m_cursorVisible = true;

    // Double-click detection
    struct ClickInfo
    {
      uint64_t lastClickTime = 0;
      nsVec2 lastClickPosition { 0.0f, 0.0f };
      uint32_t clickCount = 0;
    };
    std::unordered_map<MouseButton, ClickInfo> m_clickInfo;
    static constexpr uint64_t kDoubleClickTimeMs = 500;
    static constexpr float kDoubleClickDistanceSq = 25.0f; // 5 pixels squared

    // Gamepad tracking, keyed by joystick instance id
    struct GamepadInfo
    {
      void* controller = nullptr; // SDL_GameController*
      uint32_t id = 0;
    };
    std::unordered_map<int, GamepadInfo> m_gamepads;
  };

} // namespace apui::samples
