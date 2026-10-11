// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// App.cpp - APPlayer, the reference player. It opens an HTML document (a file, an
// http(s) URL, or the splash page) in a UIView and shows it in a Dear ImGui shell with
// a file browser, DOM inspector, live editor and log (D3D11), or full-window (Vulkan).
// Chrome DevTools attach through DevToolsHost at http://127.0.0.1:9222/.
//
// Command line (options are not case-sensitive):
//   <file or http(s) URL>         the document to open; --html <path>, --html=<path>,
//                                 --url <url> and --url=<url> say which it is
//   --vulkan, --d3d11             the render backend (D3D11 by default; the Vulkan
//                                 backend shows the page full-window, without the shell)
//   --watch                       reload when a file under the document's folder changes
//   --content-root=<folder>       a further folder pages may read files from
//   --gpu-font                    draw glyphs from their outlines on the GPU
//   --devtools-token <token>      DevTools clients must present this token (or =<token>)
//   --single-frame                render one frame, then exit
//   --capture-after=<n>[=<file>]  render n frames, save the last one as a BMP (by
//                                 default %TEMP%/apertureplayer-capture.bmp), then exit
//   --license-key, --project-id, --certificate: see common/LicenseArgs.h
// Keys: F11 fullscreen, F12 DevTools on and off.
// Not available: the F1 layout outlines and --no-layout-debug of earlier players. The
// public API has no switch for them (an unknown option is only logged). The nearest
// substitute is the "Box Model" overlay on the Inspector's Debug tab (Debug and Dev
// builds).

#include "D3D11Context.h"
#include "FileBrowser.h"
#include "GamepadNavigator.h"
#include "IPlayerRenderContext.h"
#include "Inspector.h"
#include "LicenseArgs.h"
#include "LiveEditor.h"
#include "LogPanel.h"
#include "Options.h"
#include "SDL2InputAdapter.h"
#include "Shell.h"
#include "State.h"
#include "Toolbar.h"
#include "VideoDecoder.h"
#ifdef APERTURE_HAS_VULKAN
#  include "VulkanContext.h"
#endif

#include <APHTML/APEngine.h>
#include <APHTML/API/APUIView.h>
#include <APHTML/Font/FontEngineBridge.h>
#include <APHTML/Input/InputAdapter.h>
#include <APHTML/Interfaces/APCFileSystem.h>
#include <APHTML/Interfaces/APCLoggingSystem.h>
#include <APHTML/Interfaces/APCMemoryAllocator.h>
#include <APHTML/Interfaces/APCPlatform.h>
#include <DeveloperEngine/DevToolsHost.h>
#include <Foundation/Logging/Log.h>
#include <NetworkEngine/HTTPS/DefaultNetworkProvider.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <memory>
#include <share.h>
#include <string>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <shellapi.h>

namespace fs = std::filesystem;

namespace apui::samples::player
{
  namespace
  {
    // ---- Crash log -------------------------------------------------------------------
    // On an unhandled exception, %TEMP%/APPlayer-crash.txt gets the exception code and
    // the faulting thread's stack as module+offset lines (resolve them with the modules'
    // .pdb files). Nothing here allocates: the heap may be what failed.

    wchar_t g_crashLogPath[MAX_PATH] = { };
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
    volatile LONG g_crashing = 0;

    void AppendCrashLine(HANDLE file, const char* format, ...)
    {
      char line[512];
      va_list args;
      va_start(args, format);
      const int n = std::vsnprintf(line, sizeof(line), format, args);
      va_end(args);
      if (n <= 0)
        return;
      DWORD written = 0;
      WriteFile(file, line,
        static_cast<DWORD>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1),
        &written, nullptr);
    }

    void AppendFrame(HANDLE file, DWORD64 address)
    {
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module))
      {
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
          const char* base = std::strrchr(path, '\\');
          strcpy_s(name, base ? base + 1 : path);
        }
      }
      const DWORD64 offset
        = module ? address - reinterpret_cast<DWORD64>(module) : address;
      AppendCrashLine(
        file, "  %s+0x%llx\r\n", name, static_cast<unsigned long long>(offset));
    }

    /// Walks the faulting thread's stack from the exception's context with the x64 unwind
    /// tables. Holds no C++ objects, so it may use __try.
    void AppendStack(HANDLE file, const CONTEXT* faulting)
    {
#if defined(_M_X64)
      CONTEXT ctx = *faulting;
      __try
      {
        for (int i = 0; i < 64 && ctx.Rip != 0; ++i)
        {
          AppendFrame(file, ctx.Rip);
          DWORD64 imageBase = 0;
          PRUNTIME_FUNCTION function
            = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
          if (!function)
          {
            // A leaf function: its return address is on top of the stack.
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += sizeof(DWORD64);
            continue;
          }
          void* handlerData = nullptr;
          DWORD64 establisherFrame = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx,
            &handlerData, &establisherFrame, nullptr);
        }
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        AppendCrashLine(file, "  (the stack could not be walked further)\r\n");
      }
#else
      AppendCrashLine(file, "  (stack walking is implemented for x64 only)\r\n");
      (void)faulting;
#endif
    }

    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
    {
      // Already handling a crash: a filter this one passed it on to has called back in.
      if (InterlockedExchange(&g_crashing, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
      const HANDLE file = CreateFileW(g_crashLogPath, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE)
      {
        const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
        AppendCrashLine(file,
          "APPlayer crashed: exception 0x%08lX at 0x%p.\r\n"
          "Call stack, most recent first:\r\n",
          record ? record->ExceptionCode : 0ul,
          record ? record->ExceptionAddress : nullptr);
        if (info && info->ContextRecord)
          AppendStack(file, info->ContextRecord);
        CloseHandle(file);
      }
      return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    /// Installs the crash log. Called again once the SDK is running, because the SDK may
    /// install a filter of its own: this one then runs first and passes the crash on.
    void InstallCrashLog()
    {
      if (g_crashLogPath[0] == L'\0')
      {
        wchar_t dir[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, dir);
        if (n == 0 || n >= MAX_PATH
          || swprintf_s(g_crashLogPath, L"%lsAPPlayer-crash.txt", dir) < 0)
          return;
      }
      const LPTOP_LEVEL_EXCEPTION_FILTER previous
        = SetUnhandledExceptionFilter(&OnUnhandledException);
      if (previous != &OnUnhandledException)
        g_previousFilter = previous;
    }

    // ---- Text and paths --------------------------------------------------------------

    std::wstring Widen(const std::string& utf8)
    {
      if (utf8.empty())
        return { };
      const int n = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
      std::wstring out(static_cast<size_t>(n), L'\0');
      MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
      return out;
    }

    std::string Narrow(const wchar_t* wide)
    {
      const int n
        = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
      if (n <= 1)
        return { };
      std::string out(static_cast<size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
      out.pop_back(); // the terminator
      return out;
    }

    /// The command line as UTF-8, argv[0] first.
    std::vector<std::string> Utf8CommandLine()
    {
      int argc = 0;
      const std::unique_ptr<LPWSTR, decltype(&LocalFree)> argv(
        CommandLineToArgvW(GetCommandLineW(), &argc), &LocalFree);
      std::vector<std::string> args;
      if (argv)
        for (int i = 0; i < argc; ++i)
          args.push_back(Narrow(argv.get()[i]));
      return args;
    }

    /// The executable's folder, UTF-8 with forward slashes and a trailing slash.
    std::string ExeDir()
    {
      std::wstring path(MAX_PATH, L'\0');
      for (;;)
      {
        const DWORD n
          = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0)
          return "./";
        if (n < path.size())
        {
          path.resize(n);
          break;
        }
        path.resize(path.size() * 2);
      }
      return fs::path(path).parent_path().generic_u8string() + "/";
    }

    /// A path in the ANSI code page, for the render contexts' fopen. Empty when the path
    /// cannot be written in it.
    std::string AnsiPath(const fs::path& path)
    {
      try
      {
        return path.string();
      }
      catch (const std::exception&)
      {
        return { };
      }
    }

    /// SDK settings that have to be made before the SDK starts.
    void ConfigureSdk()
    {
      using aperture::ApertureSDK;
      ApertureSDK::SetScriptThreadCount(1);
      ApertureSDK::SetRenderThreadCount(1);
      ApertureSDK::SetCompositionThreadCount(1);
      // GPU vector primitives: rounded rectangles and borders drawn from curves.
      ApertureSDK::SetSlugPrimitivesEnabled(true);
      ApertureSDK::SetSDKCores(2);
      ApertureSDK::SetIncrementalLayoutEnabled(true);
      ApertureSDK::SetGeometryUploadCacheEnabled(true);
      ApertureSDK::SetParallelStyleEnabled(true);
    }

    /// A plain-text copy of the log beside the executable (apertureplayer.log), for after
    /// the window has closed.
    class FileLog
    {
    public:
      FileLog() = default;
      FileLog(const FileLog&) = delete;
      FileLog& operator=(const FileLog&) = delete;
      ~FileLog() { Close(); }

      void Open(const fs::path& path)
      {
        // Shared for reading, so the log can be followed while the player runs.
        m_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
        if (m_file)
          m_subscription
            = nsGlobalLog::AddLogWriter(nsMakeDelegate(&FileLog::Write, this));
      }

      bool IsOpen() const { return m_file != nullptr; }

      void Close()
      {
        if (m_subscription != 0)
          nsGlobalLog::RemoveLogWriter(m_subscription);
        if (m_file)
        {
          std::fclose(m_file);
          m_file = nullptr;
        }
      }

    private:
      void Write(const nsLoggingEventData& e)
      {
        const char* level = "info";
        bool flush = false;
        switch (e.m_EventType)
        {
          case nsLogMsgType::BeginGroup:
          case nsLogMsgType::EndGroup:
          case nsLogMsgType::None:
            return;
          case nsLogMsgType::ErrorMsg:
            level = "error";
            flush = true;
            break;
          case nsLogMsgType::SeriousWarningMsg:
          case nsLogMsgType::WarningMsg:
            level = "warning";
            flush = true;
            break;
          default:
            break;
        }
        std::fprintf(m_file, "%-7s %.*s\n", level,
          static_cast<int>(e.m_sText.GetElementCount()), e.m_sText.GetStartPointer());
        if (flush)
          std::fflush(m_file); // what led up to a failure survives a crash
      }

      std::FILE* m_file = nullptr;
      nsEventSubscriptionID m_subscription = 0;
    };

    aperture::ModifierKeys ToView(const aperture::input::ModifierKeys& m)
    {
      aperture::ModifierKeys k;
      k.alt = m.alt;
      k.ctrl = m.ctrl;
      k.shift = m.shift;
      k.meta = m.meta;
      return k;
    }

    bool ToView(aperture::MouseButton button, aperture::UIView::MouseButton& out)
    {
      switch (button)
      {
        case aperture::MouseButton::Left:
          out = aperture::UIView::MouseButton::Left;
          return true;
        case aperture::MouseButton::Middle:
          out = aperture::UIView::MouseButton::Middle;
          return true;
        case aperture::MouseButton::Right:
          out = aperture::UIView::MouseButton::Right;
          return true;
        case aperture::MouseButton::Back:
          out = aperture::UIView::MouseButton::Back;
          return true;
        case aperture::MouseButton::Forward:
          out = aperture::UIView::MouseButton::Forward;
          return true;
        default:
          return false;
      }
    }

    struct DevToolsHostDeleter
    {
      void operator()(aperture::developer::DevToolsHost* host) const
      {
        aperture::developer::DevToolsHost::Destroy(host); // stops the server first
      }
    };

    class App
    {
    public:
      explicit App(Options options)
        : m_options(std::move(options))
      {
      }
      ~App() { Shutdown(); }
      App(const App&) = delete;
      App& operator=(const App&) = delete;

      bool Initialize();
      int Run();

      /// The log file's native path, for a message to the user; empty when the log could
      /// not be opened.
      std::wstring LogPath() const
      {
        return m_fileLog.IsOpen()
          ? fs::u8path(m_exeDir + kLogName).make_preferred().wstring()
          : std::wstring();
      }

    private:
      static constexpr const char* kLogName = "apertureplayer.log";

      enum class Source
      {
        None,
        File,
        Url,
        Html
      };

      struct ContentArea
      {
        float x, y, width, height;
      };

      bool StartPlatform();
      void CreateView(nsUInt32 width, nsUInt32 height);
      void CreateShell();
      void WireInput();
      void LoadInitialDocument();
      void LoadSplash();
      bool LoadFile(const std::string& path);
      bool LoadUrl(const std::string& url);
      bool LoadHtml(const std::string& html);
      bool NavigateTo(const std::string& url);
      void ReloadDocument();
      void OnDocumentLoaded(Source kind, const std::string& source);
      void StartDevTools();
      void ToggleDevTools();
      static void OnDevToolsReload(void* userData, bool cssOnly);
      void UpdateFileWatcher();
      void StopFileWatcher();
      void PollFileWatcher();
      ContentArea GetContentArea() const;
      void HandleResize(nsUInt32 width, nsUInt32 height);
      void UpdateMediaEnvironment();
      void UpdateFrameStats(float deltaSeconds);
      void RenderFrame(float deltaSeconds);
      void Shutdown();

      Options m_options;
      std::string m_exeDir;      // UTF-8, with a trailing slash
      std::string m_capturePath; // ANSI, for the render context
      FileLog m_fileLog;

      std::unique_ptr<aperture::core::IAPCLoggingSystem> m_loggingSystem;
      std::unique_ptr<aperture::core::IAPCFileSystem> m_fileSystem;
      std::unique_ptr<aperture::core::IAPCMemoryAllocator> m_memoryAllocator;
      std::unique_ptr<VideoDecoder> m_videoDecoder;
      std::unique_ptr<aperture::networking::DefaultNetworkProvider> m_networkProvider;

      std::unique_ptr<SDL2InputAdapter> m_input;
      std::unique_ptr<IPlayerRenderContext> m_context;
      D3D11Context* m_d3d11 = nullptr; // m_context, when it is the D3D11 backend
      std::unique_ptr<aperture::UIView> m_view;
      std::unique_ptr<GamepadNavigator> m_gamepad;
      std::unique_ptr<aperture::developer::DevToolsHost, DevToolsHostDeleter> m_devTools;
      std::unique_ptr<Shell> m_shell;

      State m_state;
      std::string m_statePath;
      Source m_sourceKind = Source::None;
      std::string m_source; // the current document's path, URL or HTML, for reloading

      HANDLE m_watchHandle = INVALID_HANDLE_VALUE;
      std::wstring m_watchDir;
      bool m_reloadPending = false;
      std::chrono::steady_clock::time_point m_reloadAt;

      bool m_running = false;
      bool m_shutdown = false;
      bool m_fullscreen = false;
      unsigned m_pagePressed
        = 0; // bit per UIView::MouseButton whose press went to the page
      nsUInt32 m_frameCounter = 0;
      nsUInt32 m_contentW = 0;
      nsUInt32 m_contentH = 0;
      float m_fpsAccum = 0.0f;
      int m_fpsFrames = 0;
      float m_fps = 0.0f;
      float m_frameMs = 0.0f;
      size_t m_memoryMB = 0;
    };

    bool App::Initialize()
    {
      m_exeDir = ExeDir();
      m_fileLog.Open(fs::u8path(m_exeDir + kLogName));
      for (const std::string& warning : m_options.warnings)
        nsLog::Warning("APPlayer: {0}", warning.c_str());

      if (m_options.captureAfterFrames > 0)
      {
        std::error_code ec;
        const fs::path path = m_options.capturePath.empty()
          ? fs::temp_directory_path(ec) / "apertureplayer-capture.bmp"
          : fs::u8path(m_options.capturePath);
        m_capturePath = AnsiPath(path);
        if (m_capturePath.empty())
          nsLog::Warning(
            "APPlayer: the capture path cannot be written in this system's code "
            "page; no capture will be saved.");
      }

      if (!StartPlatform())
        return false;

      SDL2InputAdapter::WindowConfig window;
      window.title = "Aperture Player";
      window.width = 1280;
      window.height = 720;
      window.resizable = true;
      window.highDpi = true;
      m_input = std::make_unique<SDL2InputAdapter>(window);
      if (!m_input->Initialize())
      {
        nsLog::Error("APPlayer: the SDL2 window could not be created.");
        return false;
      }
      const HWND hwnd = static_cast<HWND>(m_input->GetNativeWindowHandle());
      if (!hwnd)
      {
        nsLog::Error("APPlayer: SDL2 gave no native window handle.");
        return false;
      }
      const nsVec2 client = m_input->GetClientSize();
      const auto width = static_cast<nsUInt32>(client.x);
      const auto height = static_cast<nsUInt32>(client.y);

      if (m_options.vulkan)
      {
#ifdef APERTURE_HAS_VULKAN
        m_context = std::make_unique<VulkanContext>();
        nsLog::Info("APPlayer: using the Vulkan renderer.");
#else
        nsLog::Error(
          "APPlayer: --vulkan was given, but this build has no Vulkan backend (the "
          "Vulkan SDK was not found when it was configured).");
        return false;
#endif
      }
      else
      {
        auto d3d11 = std::make_unique<D3D11Context>();
        m_d3d11 = d3d11.get();
        m_context = std::move(d3d11);
        nsLog::Info("APPlayer: using the D3D11 renderer.");
      }
      if (!m_context->Initialize(hwnd, width, height))
      {
        nsLog::Error("APPlayer: the render context could not be created.");
        return false;
      }

      m_statePath = m_exeDir + "aperture_player_state.json";
      m_state = State::Load(m_statePath);

      CreateView(width, height);
      m_contentW = width;
      m_contentH = height;
      m_gamepad = std::make_unique<GamepadNavigator>();
      m_gamepad->SetView(m_view.get()); // the view lives as long as the app: never re-set
      WireInput();
      if (m_d3d11)
        CreateShell();

      LoadInitialDocument();
      StartDevTools();
      return true;
    }

    bool App::StartPlatform()
    {
      m_loggingSystem = std::make_unique<aperture::core::DefaultLoggingSystem>();
      m_fileSystem = std::make_unique<aperture::core::IAPCFileSystem>(m_exeDir.c_str());
      for (const std::string& root : m_options.contentRoots)
        m_fileSystem->AddTrustedRoot(root.c_str());
      m_memoryAllocator = std::make_unique<aperture::core::IAPCMemoryAllocator>();
      m_videoDecoder = std::make_unique<VideoDecoder>();
      m_networkProvider
        = std::make_unique<aperture::networking::DefaultNetworkProvider>();

      const auto platform = aperture::core::IAPCPlatform::GetEnginePlatform();
      if (!platform)
      {
        nsLog::Error("APPlayer: the SDK has no platform object.");
        return false;
      }
      platform->SetLoggingSystem(m_loggingSystem.get());
      platform->SetMemoryAllocator(m_memoryAllocator.get());
      platform->SetFileSystem(m_fileSystem.get());
      platform->SetVideoDecoder(m_videoDecoder.get());
      platform->SetNetworkProvider(m_networkProvider.get());
      if (!platform->InitializePlatform())
      {
        nsLog::Error(
          "APPlayer: the Aperture UI platform did not initialise; check the systems "
          "handed to it.");
        return false;
      }
      platform->StartPlatform();
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
      m_loggingSystem->SetInternalEngineLogging(true);
#endif
      m_loggingSystem->LogInfo("Platform is ready.\n");
      return true;
    }

    void App::CreateView(nsUInt32 width, nsUInt32 height)
    {
      m_view = std::make_unique<aperture::UIView>();
      m_view->setInspectorViewMode(aperture::DevTools::IVM_EDIT);

      const aperture::JSInitResult js
        = aperture::core::IAPCPlatform::GetEnginePlatform()->InitializeJavaScriptEngine();
      if (!js.IsSuccess() && js.errorCode != aperture::EJSInitError::AlreadyInitialized)
        nsLog::Error("APPlayer: the JavaScript engine did not start: {0}",
          js.errorMessage.GetData());

      // CSS group effects (opacity groups, blend isolation, filters, masks) composite
      // through off-screen layers of this context.
      m_view->setRenderPathContext(m_context->GetRenderPathContext());
#if NS_ENABLED(APUI_ENABLE_ATLAS_UPDATE_CALLBACK)
      m_view->setAtlasUpdateCallback(
        [this](HarrlowVG::Atlas* atlas) { m_context->OnAtlasUpdated(atlas); });
#endif
      m_view->setViewportSize(width, height);
      UpdateMediaEnvironment();
    }

    void App::CreateShell()
    {
      Shell::Config cfg;
      cfg.device = m_d3d11->GetDevice();
      cfg.deviceContext = m_d3d11->GetDeviceContext();
      cfg.window = m_input->GetSDLWindow();
      cfg.state = &m_state;
      cfg.defaultWorkspace = m_exeDir + "Data";
      cfg.iniPath = m_exeDir + "aperture_player_imgui.ini"; // beside the state file
      m_shell = std::make_unique<Shell>();
      if (!m_shell->Initialize(cfg))
      {
        nsLog::Error(
          "APPlayer: the ImGui shell did not start; showing the page full-window.");
        m_shell.reset();
        return;
      }

      Toolbar* toolbar = m_shell->GetToolbar();
      toolbar->SetNavigateCallback([this](const std::string& url) { NavigateTo(url); });
      toolbar->SetWatchCallback(
        [this](bool active)
        {
          m_options.watch = active;
          UpdateFileWatcher();
        });
      toolbar->SetWatchActive(m_options.watch);
      m_shell->GetFileBrowser()->SetLoadCallback(
        [this](const std::string& path) { NavigateTo(path); });
      m_shell->GetInspector()->SetView(m_view.get());
      m_shell->GetLiveEditor()->SetApplyCallback(
        [this](const std::string& html) { LoadHtml(html); });
      m_input->SetRawSDLEventHook(
        [](const SDL_Event& e) { ImGui_ImplSDL2_ProcessEvent(&e); });
    }

    App::ContentArea App::GetContentArea() const
    {
      if (m_shell)
      {
        const ImVec2 pos = m_shell->GetContentViewportPos();
        const ImVec2 size = m_shell->GetContentViewportSize();
        return { pos.x, pos.y, size.x, size.y };
      }
      return { 0.0f, 0.0f, static_cast<float>(m_contentW),
        static_cast<float>(m_contentH) };
    }

    void App::WireInput()
    {
      using aperture::input::KeyEvent;
      using aperture::input::MouseEvent;
      using aperture::input::TouchEvent;
      using aperture::input::WindowEvent;

      m_input->SetWindowEventCallback(
        [this](const WindowEvent& event)
        {
          switch (event.type)
          {
            case WindowEvent::Type::Close:
              m_running = false;
              break;
            case WindowEvent::Type::Resize:
              HandleResize(event.width, event.height);
              break;
            case WindowEvent::Type::DpiChanged:
              UpdateMediaEnvironment(); // another monitor may have another DPI scale
              break;
            case WindowEvent::Type::Focus:
              // Dark mode and animation settings are changed in other windows; pick them
              // up when the player gets focus back.
              UpdateMediaEnvironment();
              break;
            case WindowEvent::Type::Blur:
              m_view->clearFocus();
              break;
            default:
              break;
          }
        });

      m_input->SetMouseEventCallback(
        [this](const MouseEvent& event)
        {
          const ContentArea area = GetContentArea();
          const float x = event.position.x - area.x;
          const float y = event.position.y - area.y;
          // Only events inside the content area reach the page. ImGui's WantCaptureMouse
          // cannot decide this: the docked Content window sets it for clicks on the page
          // too. Moves always go through, so a drag that leaves the area stays smooth.
          const bool inside
            = x >= 0.0f && y >= 0.0f && x <= area.width && y <= area.height;

          aperture::UIView::MouseButton button = aperture::UIView::MouseButton::Left;
          const bool isButton = event.type == MouseEvent::Type::ButtonDown
            || event.type == MouseEvent::Type::ButtonUp;
          const bool known = isButton && ToView(event.button, button);
          const unsigned bit = known ? 1u << static_cast<unsigned>(button) : 0u;
          // The release of a press the page received goes to the page wherever it happens
          // (a drag that ended over a panel, or outside the window); otherwise the page
          // would see that button held down for good.
          const bool pageRelease
            = event.type == MouseEvent::Type::ButtonUp && (m_pagePressed & bit) != 0;
          if (event.type != MouseEvent::Type::Move && !inside && !pageRelease)
            return;

          const aperture::ModifierKeys mods = ToView(event.modifiers);
          switch (event.type)
          {
            case MouseEvent::Type::Move:
              m_view->onMouseMove(x, y, mods);
              break;
            case MouseEvent::Type::ButtonDown:
              if (known)
              {
                m_pagePressed |= bit;
                m_view->onMouseDown(x, y, button, mods);
              }
              break;
            case MouseEvent::Type::ButtonUp:
              if (!known)
                break;
              m_pagePressed &= ~bit;
              m_view->onMouseUp(x, y, button, mods);
              if (!inside)
                break; // the page has its release; a click needs a place in the page
              if (event.button
                == aperture::MouseButton::Left) // click is for the primary button only
              {
                m_view->onClick(x, y, button, mods);
                if (event.clickCount >= 2)
                  m_view->onDoubleClick(x, y, button, mods);
              }
              else if (event.button == aperture::MouseButton::Right)
              {
                m_view->onContextMenu(x, y, mods);
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

      m_input->SetKeyEventCallback(
        [this](const KeyEvent& event)
        {
          using aperture::input::KeyCode;
          if (event.type == KeyEvent::Type::KeyDown)
          {
            if (event.keyCode == KeyCode::F11)
            {
              m_fullscreen = !m_fullscreen;
              m_input->SetFullscreen(m_fullscreen, true);
              return;
            }
            if (event.keyCode == KeyCode::F12)
            {
              ToggleDevTools();
              return;
            }
          }
          // ImGui keeps the keyboard while one of its fields has focus (the address
          // field, the editors). Key releases always reach the page, so that no key
          // stays down in it.
          if (event.type != KeyEvent::Type::KeyUp && ImGui::GetCurrentContext()
            && ImGui::GetIO().WantCaptureKeyboard)
            return;

          const aperture::ModifierKeys mods = ToView(event.modifiers);
          switch (event.type)
          {
            case KeyEvent::Type::KeyDown:
              m_view->onKeyDown(
                aperture::input::KeyCodeToKeyString(event.keyCode, mods.shift),
                aperture::input::KeyCodeToCodeString(event.keyCode), mods,
                event.isRepeat);
              break;
            case KeyEvent::Type::KeyUp:
              m_view->onKeyUp(
                aperture::input::KeyCodeToKeyString(event.keyCode, mods.shift),
                aperture::input::KeyCodeToCodeString(event.keyCode), mods);
              break;
            case KeyEvent::Type::Character:
              if (event.character != 0)
                m_view->onKeyPress(event.character, mods);
              break;
          }
        });

      m_input->SetTouchEventCallback(
        [this](const TouchEvent& event)
        {
          // A bounds check rather than WantCaptureMouse: SDL's synthetic mouse events for
          // touches can set that flag and would block every touch.
          const ContentArea area = GetContentArea();
          std::vector<aperture::UIView::TouchPoint> touches;
          touches.reserve(event.points.size());
          for (const auto& pt : event.points)
          {
            const float x = pt.x - area.x;
            const float y = pt.y - area.y;
            if (x < 0.0f || y < 0.0f || x > area.width || y > area.height)
              continue;
            aperture::UIView::TouchPoint t;
            t.id = pt.id;
            t.x = x;
            t.y = y;
            t.radiusX = pt.radiusX;
            t.radiusY = pt.radiusY;
            t.force = pt.force;
            touches.push_back(t);
          }
          if (touches.empty())
            return;

          const aperture::ModifierKeys mods = ToView(event.modifiers);
          switch (event.type)
          {
            case TouchEvent::Type::Begin:
              m_view->onTouchStart(touches, mods);
              break;
            case TouchEvent::Type::Move:
              m_view->onTouchMove(touches, mods);
              break;
            case TouchEvent::Type::End:
              m_view->onTouchEnd(touches, mods);
              break;
            case TouchEvent::Type::Cancel:
              m_view->onTouchCancel(touches);
              break;
          }
        });

      m_input->SetGamepadEventCallback([this](const aperture::input::GamepadEvent& event)
        { m_gamepad->OnGamepadEvent(event); });
    }

    // ---- Documents -------------------------------------------------------------------

    void App::LoadInitialDocument()
    {
      // A document named on the command line wins over the one remembered from the last
      // session: a stale remembered page must never load over what was asked for.
      if (!m_options.documentUrl.empty() || !m_options.documentPath.empty())
      {
        const bool url = !m_options.documentUrl.empty();
        const std::string& doc = url ? m_options.documentUrl : m_options.documentPath;
        if (url ? LoadUrl(doc) : LoadFile(doc))
        {
          nsLog::Info("APPlayer: loaded {0}", doc.c_str());
          if (m_shell)
            m_shell->GetToolbar()->OnNavigated(doc);
          return;
        }
        nsLog::Warning(
          "APPlayer: could not load {0}; showing the splash page.", doc.c_str());
      }
      else if (m_shell && !m_state.lastUrl.empty() && NavigateTo(m_state.lastUrl))
      {
        return;
      }
      LoadSplash();
    }

    void App::LoadSplash()
    {
      const std::string splash = m_exeDir + "Data/splash.html";
      if (LoadFile(splash))
        nsLog::Info("APPlayer: loaded the splash page {0}", splash.c_str());
      else
        nsLog::Warning("APPlayer: could not load the splash page {0}; the view is empty.",
          splash.c_str());
    }

    bool App::LoadFile(const std::string& path)
    {
      // A page may also read the folder above its own ("../video.webm"), which the
      // engine's per-document root alone refuses; deeper trees need --content-root. The
      // grant belongs to this document only and replaces the previous one; the engine
      // refuses it for a drive root or the user profile. It is in place before the load,
      // because subresources resolve during it, and a failed load keeps the old grant.
      const std::string grant
        = fs::u8path(path).parent_path().parent_path().generic_u8string();
      const std::string previous = m_fileSystem->GetDocumentTrustedRoot().GetData();
      m_fileSystem->SetDocumentTrustedRoot(grant.c_str());
      if (!m_view->loadHtmlFromFile(path))
      {
        m_fileSystem->SetDocumentTrustedRoot(previous.c_str());
        return false;
      }
      OnDocumentLoaded(Source::File, path);
      return true;
    }

    bool App::LoadUrl(const std::string& url)
    {
      // A remote document gets no local folder beyond the --content-root ones.
      const std::string previous = m_fileSystem->GetDocumentTrustedRoot().GetData();
      m_fileSystem->SetDocumentTrustedRoot(nullptr);
      if (!m_view->loadHtmlFromUrl(url))
      {
        m_fileSystem->SetDocumentTrustedRoot(previous.c_str());
        return false;
      }
      OnDocumentLoaded(Source::Url, url);
      return true;
    }

    bool App::LoadHtml(const std::string& html)
    {
      if (!m_view->loadHtmlFromString(html))
        return false;
      OnDocumentLoaded(Source::Html, html);
      return true;
    }

    bool App::NavigateTo(const std::string& url)
    {
      if (url.empty())
        return false;
      bool loaded = false;
      if (IsHttpUrl(url))
      {
        loaded = LoadUrl(url);
      }
      else
      {
        // A relative path is relative to the executable's folder, where Data/ is.
        fs::path path = fs::u8path(url);
        if (!path.has_root_path())
          path = fs::u8path(m_exeDir) / path;
        loaded = LoadFile(AbsolutePath(path.u8string()));
      }
      if (!loaded)
      {
        nsLog::Warning("APPlayer: could not load {0}", url.c_str());
        return false;
      }
      nsLog::Info("APPlayer: loaded {0}", url.c_str());
      m_state.lastUrl = url;
      m_state.AddRecent(url);
      if (m_shell)
        m_shell->GetToolbar()->OnNavigated(url);
      return true;
    }

    void App::ReloadDocument()
    {
      const std::string source = m_source; // the load replaces m_source
      bool loaded = true;
      switch (m_sourceKind)
      {
        case Source::File:
          loaded = LoadFile(source);
          break;
        case Source::Url:
          loaded = LoadUrl(source);
          break;
        case Source::Html:
          loaded = LoadHtml(source);
          break;
        case Source::None:
          LoadSplash();
          break;
      }
      if (!loaded)
        nsLog::Warning("APPlayer: the document did not reload.");
    }

    void App::OnDocumentLoaded(Source kind, const std::string& source)
    {
      m_sourceKind = kind;
      m_source = source;
      UpdateFileWatcher();
      if (m_shell)
        m_shell->GetInspector()->OnDocumentLoaded();
      if (m_devTools)
        m_devTools->NotifyDocumentUpdated();
    }

    // ---- DevTools --------------------------------------------------------------------

    void App::StartDevTools()
    {
      if (m_options.devToolsTokenGiven && !m_options.devToolsTokenProblem.empty())
      {
        nsLog::Error(
          "APPlayer: DevTools not started: --devtools-token {0}. Give it a token of at "
          "least 16 characters of A-Z, a-z, 0-9, '_' or '-' (--devtools-token=<token> "
          "or --devtools-token <token>). DevTools never runs without the token that "
          "was asked for.",
          m_options.devToolsTokenProblem.c_str());
        return;
      }

      if (!m_devTools)
      {
        m_devTools.reset(aperture::developer::DevToolsHost::Create());
        if (!m_devTools)
        {
          nsLog::Warning(
            "APPlayer: DevTools not started: another DevToolsHost is alive in this "
            "process.");
          return;
        }
      }

      aperture::developer::DevToolsHostConfig config;
      config.m_port = 9222;
      config.m_authToken
        = m_options.devToolsToken.c_str(); // empty without --devtools-token
      if (!m_devTools->Start(config))
      {
        nsLog::Warning(
          "APPlayer: DevTools not started: nothing is listening on 127.0.0.1:9222 "
          "(is the port in use?).");
        return;
      }
      m_devTools->SetInspectedView(m_view.get());
      m_devTools->SetReloadCallback(&App::OnDevToolsReload, this);

      // With a token, the URL that carries it (/t/<token>/) is not written to the log.
      char url[128];
      m_devTools->GetFrontendUrlRaw(url, sizeof(url));
      nsLog::Info("APPlayer: DevTools available at {0}{1}", url,
        m_options.devToolsToken.empty() ? "" : "t/<your --devtools-token>/");
    }

    void App::ToggleDevTools()
    {
      if (m_devTools && m_devTools->IsRunning())
      {
        m_devTools->Stop();
        nsLog::Info("APPlayer: DevTools stopped (F12).");
        return;
      }
      StartDevTools();
    }

    void App::OnDevToolsReload(void* userData, bool cssOnly)
    {
      auto* app = static_cast<App*>(userData);
      if (cssOnly)
        app->m_view->reloadStyleSheets();
      else
        app->ReloadDocument();
    }

    // ---- --watch ---------------------------------------------------------------------

    void App::UpdateFileWatcher()
    {
      std::wstring dir;
      if (m_options.watch && m_sourceKind == Source::File)
        dir = fs::u8path(m_source).parent_path().wstring();
      if (dir == m_watchDir && (m_watchHandle != INVALID_HANDLE_VALUE) == !dir.empty())
        return; // already watching that folder, or nothing to watch
      StopFileWatcher();
      if (dir.empty())
        return;
      m_watchDir = dir;
      // The subtree too: pages keep scripts and styles in subfolders.
      m_watchHandle
        = FindFirstChangeNotificationW(dir.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
      if (m_watchHandle == INVALID_HANDLE_VALUE)
        nsLog::Warning(
          "APPlayer: could not watch {0} for changes.", Narrow(dir.c_str()).c_str());
      else
        nsLog::Info("APPlayer: watching {0} for changes.", Narrow(dir.c_str()).c_str());
    }

    void App::StopFileWatcher()
    {
      if (m_watchHandle != INVALID_HANDLE_VALUE)
        FindCloseChangeNotification(m_watchHandle);
      m_watchHandle = INVALID_HANDLE_VALUE;
      m_watchDir.clear();
      m_reloadPending = false;
    }

    void App::PollFileWatcher()
    {
      const auto now = std::chrono::steady_clock::now();
      if (m_watchHandle != INVALID_HANDLE_VALUE
        && WaitForSingleObject(m_watchHandle, 0) == WAIT_OBJECT_0)
      {
        FindNextChangeNotification(m_watchHandle);
        // An editor's save is often several writes: reload once they have settled.
        m_reloadPending = true;
        m_reloadAt = now + std::chrono::milliseconds(300);
      }
      if (m_reloadPending && now >= m_reloadAt)
      {
        m_reloadPending = false;
        nsLog::Info("APPlayer: a file changed; reloading.");
        ReloadDocument();
      }
    }

    // ---- Frames ----------------------------------------------------------------------

    void App::HandleResize(nsUInt32 width, nsUInt32 height)
    {
      if (width == 0 || height == 0)
        return;
      m_context->Resize(width, height);
      if (!m_shell) // with the shell, the Content panel's size sets the viewport
      {
        m_contentW = width;
        m_contentH = height;
        m_view->setViewportSize(width, height);
      }
      // A resize can come from moving to a monitor with another DPI scale.
      UpdateMediaEnvironment();
    }

    /// Hands the view the window's DPI scale, Windows' app dark mode and its "Animation
    /// effects" setting, for CSS @media and matchMedia(). The view ignores a call that
    /// changes nothing, so this is cheap enough for every resize, display change and
    /// focus gain (the events that call it).
    void App::UpdateMediaEnvironment()
    {
      if (!m_view || !m_input)
        return;
      float dpr = 1.0f;
      if (const HWND hwnd = static_cast<HWND>(m_input->GetNativeWindowHandle()))
      {
        const UINT dpi = GetDpiForWindow(hwnd);
        if (dpi > 0)
          dpr = static_cast<float>(dpi) / 96.0f;
      }
      // Settings > Personalization > Colors > app mode: AppsUseLightTheme 0 is dark. No
      // value (before Windows 10 1809) is light.
      bool dark = false;
      DWORD appsUseLightTheme = 1;
      DWORD size = sizeof(appsUseLightTheme);
      if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLightTheme, &size)
        == ERROR_SUCCESS)
        dark = (appsUseLightTheme == 0);
      // Settings > Accessibility > Visual effects > Animation effects.
      bool reducedMotion = false;
      BOOL clientAreaAnimation = TRUE;
      if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAreaAnimation, 0))
        reducedMotion = (clientAreaAnimation == FALSE);
      m_view->setMediaEnvironment(dpr, dark, reducedMotion);
    }

    void App::UpdateFrameStats(float deltaSeconds)
    {
      m_fpsAccum += deltaSeconds;
      ++m_fpsFrames;
      if (m_fpsAccum < 0.25f)
        return;
      m_fps = static_cast<float>(m_fpsFrames) / m_fpsAccum;
      m_frameMs = (m_fpsAccum / static_cast<float>(m_fpsFrames)) * 1000.0f;
      m_fpsAccum = 0.0f;
      m_fpsFrames = 0;
      PROCESS_MEMORY_COUNTERS counters = { };
      if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        m_memoryMB = counters.WorkingSetSize / (1024 * 1024);
    }

    void App::RenderFrame(float deltaSeconds)
    {
      UpdateFrameStats(deltaSeconds);

      m_view->tick(deltaSeconds);
      if (m_devTools)
        m_devTools->Tick(); // runs the DevTools commands that must run on this thread

      const nsColor clearColor(0.05f, 0.06f, 0.10f, 1.0f);
      m_context->BeginFrame(clearColor);
      m_context->RenderCommands(m_view->buildCommandList());
      m_context->SubmitCommands();

      if (m_shell)
      {
        // BeginFrame bound the page's render target; ImGui draws to the backbuffer and
        // shows that target in its Content panel, so EndFrame skips its own copy.
        m_context->BindBackbuffer(clearColor);
        const nsVec2 client = m_input->GetClientSize();
        m_shell->Render(m_context->GetContentSRV(), { client.x, client.y });

        // The render target stays window-sized, so its depth-stencil always matches it
        // (a mismatch corrupts the stencil that clips glyphs); the shell crops it to the
        // Content panel, and the view lays out at the panel's size.
        const ImVec2 size = m_shell->GetContentViewportSize();
        const auto cw = static_cast<nsUInt32>(size.x);
        const auto ch = static_cast<nsUInt32>(size.y);
        if (cw > 0 && ch > 0 && (cw != m_contentW || ch != m_contentH))
        {
          m_contentW = cw;
          m_contentH = ch;
          m_view->setViewportSize(cw, ch);
        }
        m_shell->GetToolbar()->UpdateStats(m_fps, m_frameMs, m_memoryMB);
        m_shell->GetLogPanel()->UpdateStats(m_fps, m_frameMs, m_memoryMB);
      }

      const bool lastCaptureFrame = m_options.captureAfterFrames > 0
        && m_frameCounter + 1 >= m_options.captureAfterFrames;
      if (lastCaptureFrame && !m_capturePath.empty())
      {
        m_context->RequestBackbufferCapture(m_capturePath.c_str());
        nsLog::Info(
          "APPlayer: saving frame {0} to {1}", m_frameCounter + 1, m_capturePath.c_str());
      }

      m_context->EndFrame(true);

      ++m_frameCounter;
      if (m_options.singleFrame || lastCaptureFrame)
        m_running = false;
    }

    int App::Run()
    {
      // Again: the SDK may have installed a filter of its own since wWinMain did.
      InstallCrashLog();

      using clock = std::chrono::steady_clock;
      auto lastFrameTime = clock::now();
      m_running = true;
      while (m_running)
      {
        m_input->PollEvents(); // runs the input callbacks
        if (!m_running)
          break;
        PollFileWatcher();

        const auto now = clock::now();
        const float deltaSeconds
          = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        m_gamepad->Update(deltaSeconds); // held-stick repeat
        RenderFrame(deltaSeconds);
      }
      Shutdown();
      return 0;
    }

    void App::Shutdown()
    {
      if (m_shutdown)
        return;
      m_shutdown = true;

      StopFileWatcher();
      m_devTools.reset(); // stops the server; it must not outlive the view it inspects
      if (m_shell)
      {
        // The Inspector holds DOM elements; ImGui holds the D3D11 device and the window.
        m_shell->Shutdown();
        m_shell.reset();
      }
      if (!m_statePath.empty())
        m_state.Save(m_statePath);
      if (m_gamepad)
        m_gamepad->SetView(nullptr);
      m_gamepad.reset();
      m_view.reset();
      if (m_context)
      {
        m_context->Shutdown();
        m_context.reset();
        m_d3d11 = nullptr;
      }
      if (m_input)
      {
        m_input->Shutdown();
        m_input.reset();
      }

      // The platform outlives this App and holds raw pointers to the systems the App
      // owns. Clear the file system, the network provider and the video decoder, so that
      // none dangles once the App is gone. The memory allocator stays set: the engine
      // uses it without a null check. The logging system's setter ignores null, so the
      // bridge below is detached instead.
      //
      // Engine DLLs still log from their static destructors while they unload, after
      // this App (and the logging system it owns) is gone. Detach the nsLog bridge into
      // the logging system now, while the logging system is alive.
      if (const auto platform = aperture::core::IAPCPlatform::GetEnginePlatform())
      {
        platform->SetFileSystem(nullptr);
        platform->SetNetworkProvider(nullptr);
        platform->SetVideoDecoder(nullptr);
        if (auto* logging = platform->GetLoggingSystem())
          logging->SetInternalEngineLogging(false);
      }
      m_fileLog.Close();
    }
  } // namespace
} // namespace apui::samples::player

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  namespace player = apui::samples::player;
  player::InstallCrashLog();

  std::vector<std::string> args = player::Utf8CommandLine();
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args)
    argv.push_back(arg.data());
  const apui::samples::LicenseArgs license
    = apui::samples::ParseLicenseArgs(static_cast<int>(argv.size()), argv.data());

  player::Options options = player::ParseOptions(license.rest);
  // Licence flags that had no value: logged with the other command-line warnings, and
  // shown below if the SDK then refuses to start.
  options.warnings.insert(
    options.warnings.begin(), license.problems.begin(), license.problems.end());
  if (options.gpuFont)
    aperture::html::FontEngineBridge::SetVectorGlyphRenderingEnabled(true);
  player::ConfigureSdk();

  std::string error;
  if (!apui::samples::StartSdk(license, error))
  {
    if (!license.problems.empty())
      error += "\n";
    for (const std::string& problem : license.problems)
      error += "\n" + problem;
    MessageBoxW(
      nullptr, player::Widen(error).c_str(), L"Aperture Player", MB_OK | MB_ICONERROR);
    return 1;
  }

  std::wstring logPath;
  {
    player::App app(std::move(options));
    if (app.Initialize())
      return app.Run();
    logPath = app.LogPath();
  } // the window and the log file are closed before the message appears

  std::wstring message = L"Aperture Player could not start.\n\n";
  message += logPath.empty() ? L"The log file could not be written beside the program."
                             : L"The reason is in the log:\n" + logPath;
  MessageBoxW(nullptr, message.c_str(), L"Aperture Player", MB_OK | MB_ICONERROR);
  return 1;
}
