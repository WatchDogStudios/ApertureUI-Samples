// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
#pragma once

#include "GamepadNavigator.h"
#include "IPlayerRenderContext.h"
#include "SDL2InputAdapter.h"

#include <APHTML/API/APUIView.h>
#include <APHTML/Interfaces/APCFileSystem.h>
#include <APHTML/Interfaces/APCLoggingSystem.h>
#include <APHTML/Interfaces/APCMemoryAllocator.h>
#include <APHTML/Interfaces/APCPlatform.h>
#include <functional>
#include <memory>
#include <string>

namespace apui::samples
{
  class MinimalHost
  {
  public:
    using FrameCallback = std::function<void(float)>;

    MinimalHost();
    ~MinimalHost();

    MinimalHost(const MinimalHost&) = delete;
    MinimalHost& operator=(const MinimalHost&) = delete;

    bool Initialize(const char* title, const char* page, int argc, char** argv);
    int Run(const FrameCallback& beforeTick = {});
    aperture::UIView& View() { return *m_view; }

  private:
    void WireInput();
    void Shutdown();

    std::string m_root;
    std::unique_ptr<SDL2InputAdapter> m_input;
    std::unique_ptr<IPlayerRenderContext> m_context;
    std::unique_ptr<aperture::UIView> m_view;
    std::unique_ptr<GamepadNavigator> m_gamepad;
    std::unique_ptr<aperture::core::DefaultLoggingSystem> m_logging;
    std::unique_ptr<aperture::core::IAPCFileSystem> m_fileSystem;
    std::unique_ptr<aperture::core::IAPCMemoryAllocator> m_memoryAllocator;
    std::shared_ptr<aperture::core::IAPCPlatform> m_platform;
    bool m_running = false;
  };
} // namespace apui::samples