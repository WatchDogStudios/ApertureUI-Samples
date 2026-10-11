// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "MinimalHost.h"

#include "LicenseArgs.h"
#include "StartSdk.h"
#include "D3D11Context.h"

#include <APHTML/APEngine.h>
#include <APHTML/Input/InputAdapter.h>
#include <SDL.h>
#include <windows.h>
#include <chrono>
#include <iostream>
#include <utility>

namespace apui::samples
{
  MinimalHost::MinimalHost() = default;

  MinimalHost::~MinimalHost()
  {
    Shutdown();
  }

  bool MinimalHost::Initialize(const char* title, const char* page, int argc, char** argv)
  {
    const LicenseArgs license = ParseLicenseArgs(argc, argv);
    for (const std::string& problem : license.problems)
      std::cerr << "Invalid licence option: " << problem << '\n';

    SDL2InputAdapter::WindowConfig window;
    window.title = title;
    window.width = 960;
    window.height = 640;
    m_input = std::make_unique<SDL2InputAdapter>(window);
    if (!m_input->Initialize())
    {
      std::cerr << "Could not create the SDL2 window.\n";
      return false;
    }

    char* basePath = SDL_GetBasePath();
    m_root = basePath ? basePath : "";
    SDL_free(basePath);
    if (m_root.empty())
    {
      std::cerr << "Could not determine the executable directory.\n";
      return false;
    }

    m_logging = std::make_unique<aperture::core::DefaultLoggingSystem>();
    m_fileSystem = std::make_unique<aperture::core::IAPCFileSystem>(m_root.c_str());
    m_memoryAllocator = std::make_unique<aperture::core::IAPCMemoryAllocator>();
    m_platform = aperture::core::IAPCPlatform::GetEnginePlatform();
    if (!m_platform)
    {
      std::cerr << "The SDK did not provide a platform.\n";
      return false;
    }
    m_platform->SetLoggingSystem(m_logging.get());
    m_platform->SetFileSystem(m_fileSystem.get());
    m_platform->SetMemoryAllocator(m_memoryAllocator.get());

    aperture::ApertureSDK::SetScriptThreadCount(2);
    aperture::ApertureSDK::SetPlatformName("ApertureMinimalSample");
    aperture::ApertureSDK::SetPlatformVersion("1.0");
    std::string error;
    if (!StartSdk(license, error))
    {
      std::cerr << error << '\n';
      return false;
    }
    if (!m_platform->InitializePlatform())
    {
      std::cerr << "The Aperture UI platform did not initialise.\n";
      return false;
    }
    m_platform->StartPlatform();
    const aperture::JSInitResult js = m_platform->InitializeJavaScriptEngine();
    if (!js.IsSuccess() && js.errorCode != aperture::EJSInitError::AlreadyInitialized)
    {
      std::cerr << "JavaScript initialization failed: " << js.errorMessage.GetData() << '\n';
      return false;
    }

    const nsVec2 client = m_input->GetClientSize();
    const auto width = static_cast<nsUInt32>(client.x);
    const auto height = static_cast<nsUInt32>(client.y);
    auto d3d11 = std::make_unique<D3D11Context>();
    if (!d3d11->Initialize(static_cast<HWND>(m_input->GetNativeWindowHandle()), width, height))
    {
      std::cerr << "Could not initialise the D3D11 renderer.\n";
      return false;
    }
    m_context = std::move(d3d11);

    m_view = std::make_unique<aperture::UIView>();
    m_view->setViewportSize(width, height);
    m_view->setRenderPathContext(m_context->GetRenderPathContext());
#if NS_ENABLED(APUI_ENABLE_ATLAS_UPDATE_CALLBACK)
    m_view->setAtlasUpdateCallback(
      [this](HarrlowVG::Atlas* atlas) { m_context->OnAtlasUpdated(atlas); });
#endif
    if (!m_view->loadHtmlFromFile(m_root + page))
    {
      std::cerr << "Could not load page: " << page << '\n';
      return false;
    }

    m_gamepad = std::make_unique<GamepadNavigator>();
    m_gamepad->SetView(m_view.get());
    WireInput();
    return true;
  }

  int MinimalHost::Run(const FrameCallback& beforeTick)
  {
    using clock = std::chrono::steady_clock;
    auto previous = clock::now();
    m_running = true;
    while (m_running)
    {
      m_input->PollEvents();
      if (!m_running)
        break;
      const auto now = clock::now();
      const float deltaSeconds = std::chrono::duration<float>(now - previous).count();
      previous = now;
      if (m_gamepad)
        m_gamepad->Update(deltaSeconds);
      if (beforeTick)
        beforeTick(deltaSeconds);
      m_view->tick(deltaSeconds);
      m_context->BeginFrame(nsColor(0.08f, 0.10f, 0.13f, 1.0f));
      m_context->RenderCommands(m_view->buildCommandList());
      m_context->SubmitCommands();
      m_context->EndFrame(true);
    }
    return 0;
  }

  void MinimalHost::WireInput()
  {
    using aperture::input::KeyEvent;
    using aperture::input::MouseEvent;
    using aperture::input::WindowEvent;

    m_input->SetWindowEventCallback([this](const WindowEvent& event) {
      if (event.type == WindowEvent::Type::Close)
        m_running = false;
      else if (event.type == WindowEvent::Type::Resize && event.width && event.height)
      {
        m_view->setViewportSize(event.width, event.height);
        m_context->Resize(event.width, event.height);
      }
      else if (event.type == WindowEvent::Type::Blur)
        m_view->clearFocus();
    });

    m_input->SetMouseEventCallback([this](const MouseEvent& event) {
      const aperture::ModifierKeys mods { event.modifiers.alt, event.modifiers.ctrl,
        event.modifiers.shift, event.modifiers.meta };
      const float x = event.position.x;
      const float y = event.position.y;
      switch (event.type)
      {
        case MouseEvent::Type::Move:
          m_view->onMouseMove(x, y, mods);
          break;
        case MouseEvent::Type::ButtonDown:
          if (event.button == aperture::MouseButton::Left)
            m_view->onMouseDown(x, y, aperture::UIView::MouseButton::Left, mods);
          break;
        case MouseEvent::Type::ButtonUp:
          if (event.button == aperture::MouseButton::Left)
          {
            m_view->onMouseUp(x, y, aperture::UIView::MouseButton::Left, mods);
            m_view->onClick(x, y, aperture::UIView::MouseButton::Left, mods);
          }
          break;
        case MouseEvent::Type::Wheel:
          m_view->onWheel(x, y, event.wheelDeltaX, event.wheelDeltaY,
            static_cast<aperture::UIView::WheelDeltaMode>(event.wheelMode), mods);
          break;
        default:
          break;
      }
    });

    m_input->SetKeyEventCallback([this](const KeyEvent& event) {
      const aperture::ModifierKeys mods { event.modifiers.alt, event.modifiers.ctrl,
        event.modifiers.shift, event.modifiers.meta };
      switch (event.type)
      {
        case KeyEvent::Type::KeyDown:
          m_view->onKeyDown(aperture::input::KeyCodeToKeyString(event.keyCode, mods.shift),
            aperture::input::KeyCodeToCodeString(event.keyCode), mods, event.isRepeat);
          break;
        case KeyEvent::Type::KeyUp:
          m_view->onKeyUp(aperture::input::KeyCodeToKeyString(event.keyCode, mods.shift),
            aperture::input::KeyCodeToCodeString(event.keyCode), mods);
          break;
        case KeyEvent::Type::Character:
          if (event.character)
            m_view->onKeyPress(event.character, mods);
          break;
      }
    });

    m_input->SetGamepadEventCallback(
      [this](const aperture::input::GamepadEvent& event) { m_gamepad->OnGamepadEvent(event); });
  }

  void MinimalHost::Shutdown()
  {
    if (m_gamepad)
      m_gamepad->SetView(nullptr);
    m_gamepad.reset();
    m_view.reset();
    if (m_context)
    {
      m_context->Shutdown();
      m_context.reset();
    }
    if (m_input)
    {
      m_input->Shutdown();
      m_input.reset();
    }
    if (m_platform)
    {
      m_platform->SetFileSystem(nullptr);
      if (auto* logging = m_platform->GetLoggingSystem())
        logging->SetInternalEngineLogging(false);
    }
    m_platform.reset();
  }
} // namespace apui::samples