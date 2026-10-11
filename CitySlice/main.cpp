/*
 * main.cpp - CitySlice: a Cities: Skylines-style vertical slice.
 *
 * SDL3 owns the window and input, the Aperture Vulkan backend owns the device,
 * swapchain and render pass, CityRenderer draws the world into that pass via the
 * backend's scene hook, and an Aperture HTML document (a React app under
 * UI/dist) draws the HUD on top.
 *
 *   cityslice            run the game
 *   cityslice --selftest run the simulation checks and exit (no window, no GPU)
 */
#include "Camera.h"
#include "City.h"
#include "CityRenderer.h"
#include "CrashHandler.h"
#include "SelfTest.h"

#include <APHTML/APEngine.h>
#include <APHTML/API/APUIView.h>
#include <APHTML/DOM/DOMElement.h>
#include <APHTML/DevTools/CDPDataProvider.h>
#include <APHTML/Interfaces/APCFileSystem.h>
#include <APHTML/Interfaces/APCLoggingSystem.h>
#include <APHTML/Interfaces/APCMemoryAllocator.h>
#include <APHTML/Interfaces/APCPlatform.h>
#include <APHTML/Script/API/APUIBinder.h>
#include <APPlayerDevToolsBridge.h>
#include <APPlayerDevToolsProvider.h>
#include <APPlayerVulkanContext.h>
#include <DeveloperEngine/DeveloperEngine.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/StackTracer.h>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Foundation/Types/Delegate.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <Foundation/Logging/ConsoleWriter.h>
#include <Foundation/Logging/Log.h>
#include <Foundation/Profiling/Profiling.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace
{
  // Tool ids shared with the HUD (the React app sends the same names back).
  // 7..13 are the service kinds in City::ServiceKind order.
  enum Tool
  {
    ToolSelect = 0, ToolRoad, ToolRes, ToolCom, ToolInd, ToolBulldoze, ToolAvenue,
    ToolPower, ToolWater, ToolHealth, ToolEdu, ToolPolice, ToolPark, ToolTransit,
    ToolRaise, ToolLower, ToolFlatten, ToolTrees, ToolCount
  };
  static const char* const kToolNames[ToolCount] = { "select", "road", "res", "com", "ind", "bulldoze", "avenue",
    "power", "water", "health", "edu", "police", "park", "transit", "raise", "lower", "flatten", "trees" };

  int ToolFromName(const std::string& name)
  {
    for (int i = 0; i < ToolCount; ++i)
      if (name == kToolNames[i])
        return i;
    return ToolSelect;
  }

  const char* ToolName(int tool)
  {
    return kToolNames[tool >= 0 && tool < ToolCount ? tool : 0];
  }
  bool IsServiceTool(int tool) { return tool >= ToolPower && tool <= ToolTransit; }

  // The HUD's namespaced bindings write here; the game loop reads it each frame.
  // Bindings run on the tick thread (see Docs/embedding.md), so plain fields suffice.
  struct HudRequests
  {
    int tool = -1; ///< -1 = no change requested
    int speed = -1;
    int taxDelta = 0;
    bool newCity = false;
    bool lookAt = false;
    float lookX = 0.0f, lookY = 0.0f; ///< minimap click, 0..1
    bool clearSelection = false;
    bool undo = false;
    int brush = -1;      ///< terrain brush radius request
    int overlay = -2;    ///< -2 = no change, -1 = off, else service kind
    bool unlock = false;
    uint32_t policyToggle = 0; ///< OR of City::Policy bits the HUD flipped
    int saveLoad = 0;    ///< 1 = save, 2 = load
  } g_hud;
  // Binders run on the HUD thread (they are JS callbacks); the game loop drains
  // the struct once per frame under this lock.
  std::mutex g_hudMutex;

  void Bind_SetTool(const std::vector<std::string>& args)
  {
    if (!args.empty())
    {
      std::lock_guard<std::mutex> lock(g_hudMutex);
      g_hud.tool = ToolFromName(args[0]);
      nsLog::Info("[hud] setTool {0}", args[0].c_str());
    }
  }
  void Bind_SetSpeed(const std::vector<std::string>& args)
  {
    std::lock_guard<std::mutex> lock(g_hudMutex);
    if (!args.empty()) g_hud.speed = std::atoi(args[0].c_str());
  }
  void Bind_NewCity(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.newCity = true; }
  void Bind_AdjustTax(const std::vector<std::string>& args)
  {
    std::lock_guard<std::mutex> lock(g_hudMutex);
    if (!args.empty()) g_hud.taxDelta += std::atoi(args[0].c_str());
  }
  void Bind_LookAt(const std::vector<std::string>& args)
  {
    if (args.size() >= 2)
    {
      std::lock_guard<std::mutex> lock(g_hudMutex);
      g_hud.lookAt = true;
      g_hud.lookX = static_cast<float>(std::atof(args[0].c_str()));
      g_hud.lookY = static_cast<float>(std::atof(args[1].c_str()));
    }
  }
  void Bind_ClearSelection(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.clearSelection = true; }
  void Bind_Undo(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.undo = true; }
  void Bind_SetBrush(const std::vector<std::string>& args)
  {
    std::lock_guard<std::mutex> lock(g_hudMutex);
    if (!args.empty()) g_hud.brush = std::clamp(std::atoi(args[0].c_str()), 1, 6);
  }
  void Bind_SetOverlay(const std::vector<std::string>& args)
  {
    std::lock_guard<std::mutex> lock(g_hudMutex);
    if (!args.empty()) g_hud.overlay = std::clamp(std::atoi(args[0].c_str()), -1, City::ServiceCount - 1);
  }
  void Bind_Unlock(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.unlock = true; }
  void Bind_TogglePolicy(const std::vector<std::string>& args)
  {
    if (args.empty())
      return;
    std::lock_guard<std::mutex> lock(g_hudMutex);
    for (int i = 0; i < City::PolicyCount; ++i)
      if (args[0] == City::kPolicies[i].id)
        g_hud.policyToggle ^= 1u << i;
  }
  void Bind_SaveCity(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.saveLoad = 1; }
  void Bind_LoadCity(const std::vector<std::string>&) { std::lock_guard<std::mutex> lock(g_hudMutex); g_hud.saveLoad = 2; }
  void Bind_Echo(const std::vector<std::string>& args)
  {
    if (!args.empty()) std::printf("[hud] %s\n", args[0].c_str());
  }

  void BindGameModule(aperture::binder::APUIBinder* binder)
  {
    binder->BindNamespacedFunction("game", "setTool", &Bind_SetTool);
    binder->BindNamespacedFunction("game", "setSpeed", &Bind_SetSpeed);
    binder->BindNamespacedFunction("game", "newCity", &Bind_NewCity);
    binder->BindNamespacedFunction("game", "adjustTax", &Bind_AdjustTax);
    binder->BindNamespacedFunction("game", "lookAt", &Bind_LookAt);
    binder->BindNamespacedFunction("game", "clearSelection", &Bind_ClearSelection);
    binder->BindNamespacedFunction("game", "echo", &Bind_Echo);
    binder->BindNamespacedFunction("game", "undo", &Bind_Undo);
    binder->BindNamespacedFunction("game", "setBrush", &Bind_SetBrush);
    binder->BindNamespacedFunction("game", "setOverlay", &Bind_SetOverlay);
    binder->BindNamespacedFunction("game", "unlock", &Bind_Unlock);
    binder->BindNamespacedFunction("game", "togglePolicy", &Bind_TogglePolicy);
    binder->BindNamespacedFunction("game", "saveCity", &Bind_SaveCity);
    binder->BindNamespacedFunction("game", "loadCity", &Bind_LoadCity);
  }

  /// Where the quick save lives. SDL puts it in the platform's per-user app
  /// data directory, so the sample never writes next to the exe.
  const std::string& SavePath()
  {
    static const std::string path = [] {
      char* pref = SDL_GetPrefPath("ApertureUI", "CitySlice");
      std::string p = pref ? std::string(pref) + "quicksave.city" : std::string("quicksave.city");
      if (pref)
        SDL_free(pref);
      return p;
    }();
    return path;
  }

  /// True when the element under the cursor sits inside a HUD panel. Panels are
  /// tagged data-ui in the React app; the bare body means "the map".
  bool OverHud(aperture::UIView& view, float x, float y)
  {
    std::shared_ptr<aperture::dom::DOMElement> el = view.hitTest(x, y);
    while (el)
    {
      if (el->hasAttribute("data-ui"))
        return true;
      el = el->getParentElement();
    }
    return false;
  }

  /// Ray-march the camera ray against the heightfield. Returns false if it
  /// leaves the map without touching ground.
  bool PickGround(const Camera& cam, const City& city, float px, float py, float w, float h,
    Vec3& hit)
  {
    const Vec3 origin = cam.Eye();
    const Vec3 dir = cam.RayDir(px, py, w, h);
    const float extent = City::N * City::kCell;
    float t = 0.0f;
    Vec3 prev = origin;
    for (int i = 0; i < 1200; ++i)
    {
      t += 1.5f;
      const Vec3 p = origin + dir * t;
      if (p.y < -40.0f || t > 3000.0f)
        return false;
      const bool inside = p.x >= 0.0f && p.z >= 0.0f && p.x < extent && p.z < extent;
      if (inside && p.y <= city.HeightAt(p.x, p.z))
      {
        Vec3 a = prev, b = p;
        for (int k = 0; k < 8; ++k)
        {
          const Vec3 m = (a + b) * 0.5f;
          if (m.y <= city.HeightAt(m.x, m.z)) b = m; else a = m;
        }
        hit = b;
        return true;
      }
      prev = p;
    }
    return false;
  }

  const char* TileKindName(const City& city, int x, int y)
  {
    const City::Tile& t = city.At(x, y);
    if (t.type == City::Road) return city.IsBridge(x, y) ? "Bridge" : (t.road ? "Avenue" : "Road");
    if (t.type == City::Service) return "Service";
    if (city.IsWater(x, y)) return "Water";
    static const char* kind[5] = { "Grassland", "Road", "Residential", "Commercial", "Industrial" };
    return kind[t.type];
  }

  std::string HoverInfo(const City& city, int x, int y)
  {
    if (!City::InBounds(x, y)) return "";
    const City::Tile& t = city.At(x, y);
    if (t.type == City::Service)
      return City::kServices[t.service].name;
    std::string s = TileKindName(city, x, y);
    if (t.type >= City::Res && t.type <= City::Ind)
    {
      if (t.level == 0)
        s += city.HasRoadAccess(x, y) ? " lot (waiting for demand)" : " lot (no road access)";
      else
      {
        s += " lvl " + std::to_string(t.level);
        if (!t.powered) s += " · no power";
        if (!t.watered) s += " · no water";
      }
    }
    else if (t.type == City::Empty && t.planted)
      s += " (planted)";
    return s;
  }

  /// JSON fragment for the selected tile, or "null".
  std::string SelectionJson(const City& city, int x, int y)
  {
    if (!City::InBounds(x, y))
      return "null";
    const City::Tile& t = city.At(x, y);
    const bool zoned = t.type >= City::Res && t.type <= City::Ind;
    char buf[640];
    std::string service = "null";
    if (t.type == City::Service)
    {
      const City::ServiceStats& st = city.ServiceStatsFor(t.service);
      char sb[200];
      std::snprintf(sb, sizeof(sb), "{\"id\":\"%s\",\"name\":\"%s\",\"capacity\":%d,\"demand\":%d,\"covered\":%d,\"upkeep\":%.0f}",
        City::kServices[t.service].id, City::kServices[t.service].name, City::kServices[t.service].capacity,
        st.demand, st.covered, City::kServices[t.service].upkeep);
      service = sb;
    }
    std::snprintf(buf, sizeof(buf),
      "{\"x\":%d,\"y\":%d,\"kind\":\"%s\",\"level\":%d,\"desirability\":%.2f,\"population\":%d,"
      "\"jobs\":%d,\"access\":%s,\"water\":%s,\"coverage\":%d,\"powered\":%s,\"watered\":%s,"
      "\"service\":%s,\"planted\":%s,\"bridge\":%s}",
      x, y, TileKindName(city, x, y), t.level,
      zoned ? city.Desirability(x, y, t.type) : 0.0f, City::TilePopulation(t),
      City::TileJobs(t), city.HasRoadAccess(x, y) ? "true" : "false", city.IsWater(x, y) ? "true" : "false",
      t.coverage, t.powered ? "true" : "false", t.watered ? "true" : "false", service.c_str(),
      t.planted ? "true" : "false", city.IsBridge(x, y) ? "true" : "false");
    return buf;
  }

  /// Quote for JSON (double quotes) or for a JS single-quoted string literal.
  std::string Quote(const std::string& s, char q)
  {
    std::string out(1, q);
    for (char c : s)
    {
      if (c == q || c == '\\') out += '\\';
      if (c == '\n') { out += "\\n"; continue; }
      out += c;
    }
    out += q;
    return out;
  }
  std::string JsonQuote(const std::string& s) { return Quote(s, '"'); }
  std::string JsQuote(const std::string& s) { return Quote(s, '\''); }
} // namespace

/// --bench / CITYSLICE_WATCHDOG=1: a watchdog thread that, when a frame runs past
/// 40 ms, suspends the main thread, grabs its call stack and prints it. Finds
/// hitches that lap timers can only bracket.
static std::atomic<uint64_t> g_frameSeq { 0 };
static std::atomic<long long> g_frameStartNs { 0 };
static void StartMainThreadWatchdog()
{
  HANDLE mainThread = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread, 0, FALSE,
    DUPLICATE_SAME_ACCESS);
  std::thread([mainThread]() {
    uint64_t capturedSeq = ~0ull;
    for (;;)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      const uint64_t seq = g_frameSeq.load();
      if (seq == 0 || seq == capturedSeq)
        continue;
      const long long start = g_frameStartNs.load();
      const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now - start < 40'000'000)
        continue;
      capturedSeq = seq;
      void* frames[64] = {};
      nsArrayPtr<void*> trace(frames, 64);
      nsUInt32 count = 0;
      // Resume BEFORE walking: the walk (dbghelp) takes heap/stdio locks the
      // suspended thread may hold. A stalled thread's stack is still intact.
      CONTEXT ctx = {};
      ctx.ContextFlags = CONTEXT_FULL;
      bool haveCtx = false;
      if (SuspendThread(mainThread) != static_cast<DWORD>(-1))
      {
        haveCtx = GetThreadContext(mainThread, &ctx) != 0;
        ResumeThread(mainThread);
      }
      if (haveCtx)
        count = nsStackTracer::GetStackTrace(trace, &ctx);
      if (count == 0)
        continue;
      std::printf("[watchdog] main thread stalled %.0f ms, stack:\n", (now - start) / 1e6);
      nsArrayPtr<void*> got(frames, count);
      nsStackTracer::ResolveStackTrace(got, [](const char* text) { std::printf("%s", text); });
      std::fflush(stdout);
    }
  }).detach();
}

/// Developer console (backtick). Lines: 0 info, 1 warning, 2 error, 3 command
/// echo, 4 command output. The engine log is tapped through a log writer, which
/// may fire from worker threads, so entries queue under a mutex and are folded
/// into the visible list once per frame.
struct ConsoleState
{
  bool open = false;
  bool dirty = true;
  bool pushedOpen = false;
  std::string input;
  std::vector<std::string> history;
  int histPos = -1;
  std::deque<std::pair<int, std::string>> lines;
  std::mutex mutex;
  std::vector<std::pair<int, std::string>> pending;

  void Push(int level, std::string text)
  {
    std::lock_guard<std::mutex> g(mutex);
    pending.emplace_back(level, std::move(text));
  }
  void Add(int level, std::string text)
  {
    lines.emplace_back(level, std::move(text));
    while (lines.size() > 400)
      lines.pop_front();
    dirty = true;
  }
  void Flush()
  {
    std::lock_guard<std::mutex> g(mutex);
    for (auto& p : pending)
      Add(p.first, std::move(p.second));
    pending.clear();
  }
};
static ConsoleState g_console;

/// What the crash handler prints under "Game state" (pointers are set once the
/// objects exist; a crash during start-up just prints less).
static struct CrashContext
{
  const City* city = nullptr;
  const Camera* camera = nullptr;
  const int* tool = nullptr;
} g_crashContext;
static void WriteCrashContext(FILE* f)
{
  if (g_crashContext.city)
  {
    const City& c = *g_crashContext.city;
    std::fprintf(f, "day %.3f  population %d  money %.0f  speed %d  roads %d  buildings %d  vehicles %d\n",
      c.day, c.population, c.money, c.speed, c.roadTiles, c.buildings, static_cast<int>(c.vehicles.size()));
  }
  if (g_crashContext.camera)
    std::fprintf(f, "camera target %.1f %.1f %.1f  dist %.1f  yaw %.2f  pitch %.2f\n", g_crashContext.camera->sTarget.x,
      g_crashContext.camera->sTarget.y, g_crashContext.camera->sTarget.z, g_crashContext.camera->sDist,
      g_crashContext.camera->sYaw, g_crashContext.camera->sPitch);
  if (g_crashContext.tool)
    std::fprintf(f, "tool %d\n", *g_crashContext.tool);
  // Last console lines (engine warnings/errors land here too). Best effort:
  // the deque may be mid-update on another thread.
  std::fprintf(f, "\nConsole (last lines):\n");
  const size_t first = g_console.lines.size() > 30 ? g_console.lines.size() - 30 : 0;
  for (size_t i = first; i < g_console.lines.size(); ++i)
    std::fprintf(f, "  [%d] %s\n", g_console.lines[i].first, g_console.lines[i].second.c_str());
}

static void ConsoleLogHandler(const nsLoggingEventData& data)
{
  // Dev builds spew Debug-level engine tracing; keep the console to Info+.
  if (data.m_EventType == nsLogMsgType::BeginGroup || data.m_EventType == nsLogMsgType::EndGroup
    || data.m_EventType == nsLogMsgType::None || data.m_EventType == nsLogMsgType::DebugMsg)
    return;
  int level = 0;
  if (data.m_EventType == nsLogMsgType::ErrorMsg || data.m_EventType == nsLogMsgType::SeriousWarningMsg)
    level = 2;
  else if (data.m_EventType == nsLogMsgType::WarningMsg)
    level = 1;
  g_console.Push(level, std::string(data.m_sText.GetStartPointer(), data.m_sText.GetElementCount()));
}

/// The HUD (Aperture view: input dispatch, JavaScript, style, layout, paint
/// command list) runs on this thread, overlapping the game thread's CPU work:
///
///   game thread | poll -> Kick(N) | sim, camera, world meshes, stats | WaitDone | BeginFrame..Present
///   HUD thread  |                 | input jobs + tick + buildCommandList(N) -> Done
///
/// Every UIView call after start-up happens here. The game thread only ever
/// talks to it through Post() (input events, scripts, resizes) and the
/// published command list. The build occasionally records into the Vulkan
/// frame (layer render targets), which the backend serialises with its API
/// lock: that is why it runs while the game thread is NOT inside
/// BeginFrame..EndFrame. If the HUD misses the handoff window the game reuses
/// the previous list and the thread catches up, so a slow script never stalls
/// the world.
struct HudThread
{
  aperture::UIView* view = nullptr;
  aperture::developer::DeveloperEngine* devServer = nullptr;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool requested = false, done = true, quit = false;
  float dt = 0.0f;
  std::vector<std::function<void()>> pending, jobs;
  std::atomic<bool> overHud { false }; ///< hit test result for the last mouse position
  std::atomic<float> mouseX { 0.0f }, mouseY { 0.0f };
  std::atomic<double> lastMs { 0.0 }, lastTickMs { 0.0 };
  const aperture::harrlow::renderpath::HarrlowCommandList* commands = nullptr; ///< valid while done

  void Post(std::function<void()> job)
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending.push_back(std::move(job));
  }
  bool Done()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
  }
  /// Waits up to `ms` for the frame in flight; false when it is still running.
  bool WaitDone(double ms)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::duration<double, std::milli>(ms), [this]() { return done; });
  }
  void Kick(float deltaSeconds)
  {
    std::lock_guard<std::mutex> lock(mutex);
    jobs.swap(pending);
    pending.clear();
    dt = deltaSeconds;
    requested = true;
    done = false;
    cv.notify_all();
  }
  void Start()
  {
    thread = std::thread([this]() { Loop(); });
  }
  void Stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      quit = true;
      cv.notify_all();
    }
    if (thread.joinable())
      thread.join();
  }
  void Loop()
  {
    SetThreadDescription(GetCurrentThread(), L"HUD Thread");
#if defined(TRACY_ENABLE) && NS_ENABLED(NS_USE_PROFILING)
    tracy::SetThreadName("HUD Thread");
#endif
    std::vector<std::function<void()>> frameJobs;
    for (;;)
    {
      float frameDt = 0.0f;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return requested || quit; });
        if (quit)
          return;
        requested = false;
        frameJobs.swap(jobs);
        jobs.clear();
        frameDt = dt;
      }
      const auto t0 = std::chrono::steady_clock::now();
      {
        NS_PROFILE_SCOPE("HUD::Frame");
        {
          NS_PROFILE_SCOPE("HUD::Input+Scripts");
          for (auto& job : frameJobs)
            job();
          frameJobs.clear();
        }
        const auto t1 = std::chrono::steady_clock::now();
        {
          NS_PROFILE_SCOPE("HUD::Tick");
          view->tick(frameDt);
          if (devServer)
            devServer->Tick();
        }
        lastTickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        {
          NS_PROFILE_SCOPE("HUD::BuildCommandList");
          commands = &view->buildCommandList();
        }
        {
          NS_PROFILE_SCOPE("HUD::HitTest");
          overHud = OverHud(*view, mouseX, mouseY);
        }
      }
      lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_all();
      }
    }
  }
};

static const char* const kConsoleCommands[] = { "help", "clear", "echo", "money", "speed", "time",
  "tax", "sim", "demo", "newcity", "cam", "blur", "anim", "stats", "js", "fps", "quit", "undo", "unlock",
  "policy", "save", "load",
  "overlay", "brush", "crash" };

/// `--demo` / CITYSLICE_DEMO=1: lay out a small town on the driest flat patch
/// near the map centre and simulate a few days so the first frame already shows
/// grown buildings, traffic and the milestone chirps.
static void BuildDemoCity(City& city, Camera& camera)
{
  constexpr int W = 27, H = 13;
  int bestX = -1, bestY = -1;
  float bestScore = 1e9f;
  for (int oy = 8; oy + H < City::N - 8; oy += 2)
    for (int ox = 8; ox + W < City::N - 8; ox += 2)
    {
      bool ok = true;
      float lo = 1e9f, hi = -1e9f;
      for (int y = oy; y < oy + H && ok; ++y)
        for (int x = ox; x < ox + W; ++x)
        {
          if (city.IsWater(x, y))
          {
            ok = false;
            break;
          }
          lo = std::min(lo, city.TileTop(x, y));
          hi = std::max(hi, city.TileTop(x, y));
        }
      if (!ok)
        continue;
      const float cx = ox + W * 0.5f - City::N * 0.5f, cy = oy + H * 0.5f - City::N * 0.5f;
      const float score = (hi - lo) * 3.0f + std::sqrt(cx * cx + cy * cy) * 0.15f;
      if (score < bestScore)
      {
        bestScore = score;
        bestX = ox;
        bestY = oy;
      }
    }
  if (bestX < 0)
    return;
  const int x0 = bestX, y0 = bestY;
  // Main street with three cross streets; blocks zoned around them.
  city.BuildRoad(x0, y0 + 6, x0 + W - 1, y0 + 6);
  for (int k = 0; k < 4; ++k)
    city.BuildRoad(x0 + 1 + k * 8, y0 + 1, x0 + 1 + k * 8, y0 + H - 2);
  for (int k = 0; k < 3; ++k)
  {
    const int bx = x0 + 2 + k * 8, ex = x0 + 8 + k * 8;
    city.ZoneRect(bx, y0 + 5, ex, y0 + 5, k == 1 ? City::Com : City::Res);  // north kerb
    city.ZoneRect(bx, y0 + 7, ex, y0 + 7, k == 1 ? City::Com : City::Res);  // south kerb
    city.ZoneRect(bx, y0 + 2, ex, y0 + 2, k == 2 ? City::Ind : City::Res);  // north cross-street lots
    city.ZoneRect(bx, y0 + H - 3, ex, y0 + H - 3, k == 0 ? City::Ind : City::Res);
  }
  // Civic services on the free rows, each with a kerb on a cross street.
  city.UnlockAll();
  city.money = 60000.0f;
  city.PlaceService(x0 + 2, y0 + 3, City::PowerPlant);
  city.PlaceService(x0 + 2, y0 + 9, City::WaterTower);
  city.PlaceService(x0 + 10, y0 + 3, City::Clinic);
  city.PlaceService(x0 + 10, y0 + 9, City::School);
  city.PlaceService(x0 + 18, y0 + 3, City::PoliceStation);
  city.PlaceService(x0 + 18, y0 + 9, City::Park);
  city.PlaceService(x0 + 24, y0 + 4, City::BusDepot);
  city.PlaceService(x0 + 24, y0 + 8, City::PowerPlant);
  city.PlaceService(x0 + 26, y0 + 8, City::WaterTower);
  city.speed = 3;
  for (int i = 0; i < 1200; ++i)
    city.Tick(0.1f);
  city.speed = 1;
  city.money = 40000.0f;
  city.events.clear();
  camera.target = { (x0 + W * 0.5f) * City::kCell, 0.0f, (y0 + 6.5f) * City::kCell };
  camera.target.y = city.HeightAt(camera.target.x, camera.target.z);
  camera.dist = 150.0f;
  camera.pitch = 0.72f;
  camera.yaw = 0.55f;
  camera.Snap();
}

int main(int argc, char** argv)
{
  // stdout is flushed once per frame (see the loop): unbuffered writes made
  // every log line a synchronous console/pipe stall on the main thread.
  static char s_stdoutBuf[1 << 16];
  std::setvbuf(stdout, s_stdoutBuf, _IOFBF, sizeof(s_stdoutBuf));
  // Crash reporting from the first instruction on: minidump + text report.
  InstallCitySliceCrashHandler(&WriteCrashContext);
  float crashTestAt = -1.0f; // --crashtest [seconds]: deliberate access violation to exercise the handler
  bool demo = std::getenv("CITYSLICE_DEMO") != nullptr;
  bool bench = false; // --bench: demo town, camera orbits by itself for 30 s, then quits
  // Screenshot helpers (no input needed): --hour H, --dist D, --pitch P, --overlay <service id>.
  float startHour = -1.0f, startDist = -1.0f, startPitch = -1.0f;
  int startOverlay = -1;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--selftest") == 0)
      return RunCitySelfTest();
    if (std::strcmp(argv[i], "--demo") == 0)
      demo = true;
    if (std::strcmp(argv[i], "--bench") == 0)
      demo = bench = true;
    if (std::strcmp(argv[i], "--crashtest") == 0)
      crashTestAt = (i + 1 < argc && argv[i + 1][0] != '-') ? static_cast<float>(std::atof(argv[++i])) : 2.0f;
    if (std::strcmp(argv[i], "--hour") == 0 && i + 1 < argc)
      startHour = static_cast<float>(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--dist") == 0 && i + 1 < argc)
      startDist = static_cast<float>(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--pitch") == 0 && i + 1 < argc)
      startPitch = static_cast<float>(std::atof(argv[++i]));
    if (std::strcmp(argv[i], "--overlay") == 0 && i + 1 < argc)
    {
      ++i;
      for (int k = 0; k < City::ServiceCount; ++k)
        if (std::strcmp(argv[i], City::kServices[k].id) == 0)
          startOverlay = k;
    }
  }

  // ---- window ---------------------------------------------------------------
  if (!SDL_Init(SDL_INIT_VIDEO))
  {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("CitySlice", 1600, 900,
    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
  HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  int pixelW = 0, pixelH = 0;
  SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);

  // ---- Aperture platform (order matters: see Docs/embedding.md) --------------
  // Dev/Debug engine tracing is dropped at the source; Info lines reach the
  // in-game console, while the OS console only sees warnings and errors
  // (console writes are slow and synchronous) unless CITYSLICE_VERBOSE is set.
  nsLog::SetDefaultLogLevel(nsLogMsgType::InfoMsg);
  static const bool s_verbose = std::getenv("CITYSLICE_VERBOSE") != nullptr;
  nsGlobalLog::AddLogWriter([](const nsLoggingEventData& d) {
    if (s_verbose || d.m_EventType == nsLogMsgType::ErrorMsg || d.m_EventType == nsLogMsgType::SeriousWarningMsg
      || d.m_EventType == nsLogMsgType::WarningMsg)
      nsLogWriter::Console::LogMessageHandler(d);
  });
  aperture::ApertureSDK::SetScriptThreadCount(3);
  aperture::ApertureSDK::SetParallelStyleEnabled(true);
  aperture::ApertureSDK::SetSlugPrimitivesEnabled(true);
  // The geometry-upload cache (#3.5) skips re-uploads into PERSISTENT GPU
  // buffers (D3D11). The Vulkan backend streams through a per-frame ring, so
  // a "hit" still copies and the per-batch content hash is pure overhead.
  aperture::ApertureSDK::SetGeometryUploadCacheEnabled(false);
  aperture::JSEngineConfig jsConfig; // must precede Initialize()
  // The HUD's handlers run a handful of times a second and then stop, which is
  // exactly the shape V8's default tier-up budget never promotes. Measured a
  // third less JS time per second with this (see JSEngineConfig::tierUpBudget).
  jsConfig.tierUpBudget = 500;
  jsConfig.enableSnapshotCaching = false;
  jsConfig.validateResourcesOnInit = true;
  aperture::ApertureSDK::ConfigureJavaScriptEngine(jsConfig);
  aperture::ApertureSDK::Initialize();

  auto logging = std::make_unique<aperture::core::DefaultLoggingSystem>();
  auto fileSystem = std::make_unique<aperture::core::IAPCFileSystem>(
    nsOSFile::GetApplicationDirectory().GetStartPointer());
  auto allocator = std::make_unique<aperture::core::IAPCMemoryAllocator>();
  auto platform = aperture::core::IAPCPlatform::GetEnginePlatform(); // shared_ptr singleton
  platform->SetLoggingSystem(logging.get());
  platform->SetFileSystem(fileSystem.get());
  platform->SetMemoryAllocator(allocator.get());
  if (!platform->InitializePlatform())
  {
    std::fprintf(stderr, "Aperture platform failed to initialise\n");
    return 1;
  }
  platform->StartPlatform();
  // V8 is opt-in; the DevTools bridge below initialises it when it creates the view.

  // ---- GPU ------------------------------------------------------------------
  auto context = std::make_unique<applayer::APPlayerVulkanContext>();
  // The world renders into its own 4x MSAA HDR target; the swapchain pass only
  // holds the post pass and the (vector, self-antialiased) HUD.
  context->SetMSAASampleCount(nsGALMSAASampleCount::None);
  if (!context->Initialize(hwnd, static_cast<nsUInt32>(pixelW), static_cast<nsUInt32>(pixelH)))
  {
    std::fprintf(stderr, "Vulkan backend failed to initialise (are Shaders/Vulkan/*.spv next to the exe?)\n");
    return 1;
  }

  const std::string appDir = nsOSFile::GetApplicationDirectory().GetStartPointer();
  CityRenderer renderer;
  if (!renderer.Init(*context, appDir + "Shaders/City"))
    return 1;
  // Models authored in Blender (Assets/blender/build_city_assets.py). Loaded
  // from the source tree so re-exporting only needs a restart; absent is fine,
  // the renderer keeps its procedural geometry.
  renderer.LoadAssets(std::string(CITYSLICE_ASSET_DIR) + "/models/city_assets.glb");

  // ---- world ----------------------------------------------------------------
  City city;
  Camera camera;
  camera.target.y = city.HeightAt(camera.target.x, camera.target.z);
  camera.Snap();
  if (demo)
    BuildDemoCity(city, camera);
  if (startHour >= 0.0f)
    city.day = std::floor(city.day) + Clamp(startHour, 0.0f, 23.99f) / 24.0;
  if (startDist > 0.0f)
    camera.dist = Clamp(startDist, 20.0f, 900.0f);
  if (startPitch > 0.0f)
    camera.pitch = Clamp(startPitch, 0.25f, 1.45f);
  camera.Snap();
  // Opt-in only: the watchdog's stack prints go through stdout and can
  // themselves back-pressure the frame when the pipe reader is slow.
  if (std::getenv("CITYSLICE_WATCHDOG"))
    StartMainThreadWatchdog();
  const Vec3 benchCentre = camera.target;
  float benchTime = 0.0f;
  CityRenderer::ToolPreview tool;
  g_crashContext.city = &city;
  g_crashContext.camera = &camera;
  g_crashContext.tool = &tool.kind;
  float realTime = 0.0f;
  context->SetPreSceneCallback([&](const CityRenderer::Frame& frame) {
    renderer.Prepare(frame, city, camera, tool, realTime);
  });
  context->SetSceneCallback([&](const CityRenderer::Frame& frame) {
    renderer.Record(frame, city, camera, tool, realTime);
  });

  // ---- HUD ------------------------------------------------------------------
  // The player's DevTools bridge owns the view: CitySlice gets the same Chrome
  // DevTools Protocol server (DOM/CSS inspector, console) as APPlayer for free.
  auto bridge = std::make_shared<applayer::APPlayerDevToolsBridge>();
  aperture::UIView* view = bridge->GetUIView();
  auto binder = std::make_unique<aperture::binder::APUIBinder>();
  view->setViewportSize(static_cast<nsUInt32>(pixelW), static_cast<nsUInt32>(pixelH));
  view->setRenderPathContext(context->GetRenderPathContext());
#if NS_ENABLED(APUI_ENABLE_ATLAS_UPDATE_CALLBACK)
  view->setAtlasUpdateCallback([&](HarrlowVG::Atlas* atlas) { context->OnAtlasUpdated(atlas); });
#endif
  binder->addUIView(view);
  binder->RequestModuleToBind(&BindGameModule);
  view->assignBinder(binder.get());
  if (!view->loadHtmlFromFile(std::string(CITYSLICE_UI_DIR) + "/index.html"))
  {
    std::fprintf(stderr, "Could not load %s/index.html (run `npm run build` in the UI folder)\n", CITYSLICE_UI_DIR);
    return 1;
  }

  // ---- DevTools (CDP on 127.0.0.1:9222; chrome://inspect -> configure that target)
  std::unique_ptr<aperture::devtools::ICdpDataProvider> devProvider
    = applayer::CreateDevToolsProvider(*bridge, nullptr);
  auto devServer = std::make_unique<aperture::developer::DeveloperEngine>();
  if (devProvider && devServer->Initialize(platform, devProvider.get()))
    std::printf("DevTools available at http://%s:%u/\n", devServer->GetHost().c_str(),
      static_cast<unsigned>(devServer->GetPort()));
  else
  {
    std::fprintf(stderr, "DevTools server failed to start\n");
    devServer.reset();
  }

  // ---- loop -----------------------------------------------------------------
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  std::string hudPanelsJson; ///< CITYSLICE_PANELS, pushed once the page is up
  float statsTimer = 0.0f, mapTimer = 10.0f, historyTimer = 10.0f;
  int lastHistoryDay = -1;
  // Frame timing: per-phase accumulators printed every 2 s and fed to the HUD.
  struct Timing { double sim = 0, ui = 0, build = 0, world = 0, submit = 0, present = 0; int frames = 0; float window = 0;
    double maxFrame = 0, maxBuild = 0, maxWorld = 0, maxSubmit = 0, js = 0, maxJs = 0; } timing;
  float fps = 0.0f, frameMs = 0.0f;
  bool hudNoAnim = false, hudBlur = false, pushStats = std::getenv("CITYSLICE_NOPUSH") == nullptr;
  bool running = true;
  nsGlobalLog::AddLogWriter(&ConsoleLogHandler);
  g_console.Add(4, "CitySlice developer console. Type help for commands.");

  // ---- HUD thread ------------------------------------------------------------
  // The document was loaded above on this thread; from here on every view call
  // goes through the HUD thread (see HudThread).
  HudThread hud;
  hud.view = view;
  hud.devServer = devServer.get();
  hud.Start();
  auto postScript = [&](const char* name, std::string script) {
    hud.Post([view, name, script = std::move(script)]() {
      view->executeJavaScriptRaw(name, reinterpret_cast<const uint8_t*>(script.data()), script.size(), true);
    });
  };
  // Preferred transport: hand the payload to the page's handler directly. No
  // source string is generated, so V8 never parses or compiles anything, the
  // JSON is parsed once (by V8, in C++) instead of being escaped here and
  // JSON.parse'd there, and the realm-proof #bridge attribute mirror is not
  // needed because the call resolves the function on the page's own global.
  auto postCall = [&](const char* fn, std::string json) {
    hud.Post([view, fn, json = std::move(json)]() { view->callJavaScriptFunctionJson(fn, json.c_str()); });
  };
  // CITYSLICE_PANELS=stats,policies opens HUD panels at startup so a capture
  // can show them without driving the keyboard.
  if (const char* panels = std::getenv("CITYSLICE_PANELS"))
  {
    std::string list;
    for (const char* p = panels; *p; )
    {
      const char* end = std::strchr(p, ',');
      const std::string one(p, end ? end - p : std::strlen(p));
      if (!one.empty())
        list += (list.empty() ? "\"" : ",\"") + one + "\"";
      p = end ? end + 1 : p + std::strlen(p);
    }
    hudPanelsJson = "{\"panels\":[" + list + "]}";
  }
  // 1 = the old compile-a-script-per-push path, kept for A/B measurement.
  static const int pushMode = std::getenv("CITYSLICE_PUSH_MODE") ? std::atoi(std::getenv("CITYSLICE_PUSH_MODE")) : 0;
  std::decay_t<decltype(view->buildCommandList())> hudCommands; // last list handed over
  int hudLateFrames = 0;
  // How long the game thread waits at the handoff before drawing the previous
  // HUD list instead. Generous: a normal HUD frame is ~2 ms, only script
  // bursts exceed this, and those are exactly what must not stall the world.
  const double hudWaitBudgetMs = std::getenv("CITYSLICE_HUD_WAIT") ? std::atof(std::getenv("CITYSLICE_HUD_WAIT")) : 8.0;


  auto runConsoleCommand = [&](const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && line.back() == ' ') line.pop_back();
    size_t st = 0;
    while (st < line.size() && line[st] == ' ') ++st;
    line = line.substr(st);
    if (line.empty())
      return;
    g_console.Add(3, "] " + line);
    if (g_console.history.empty() || g_console.history.back() != line)
      g_console.history.push_back(line);
    g_console.histPos = -1;
    std::vector<std::string> args;
    {
      size_t i = 0;
      while (i < line.size())
      {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        if (j > i) args.push_back(line.substr(i, j - i));
        i = j;
      }
    }
    const std::string& cmd = args[0];
    const std::string rest = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
    auto out = [&](const std::string& s) { g_console.Add(4, s); };
    auto err = [&](const std::string& s) { g_console.Add(2, s); };
    auto num = [&](size_t i, float def) { return args.size() > i ? static_cast<float>(std::atof(args[i].c_str())) : def; };
    auto runJs = [&](const std::string& js) { postScript("console", js); };
    if (cmd == "help")
    {
      out("help            this list");
      out("clear           clear the console");
      out("echo <text>     print text");
      out("money [n]       show or set the treasury");
      out("speed <0-3>     game speed (0 pauses)");
      out("time <hour>     set the time of day (0-24)");
      out("tax <5-20>      set the tax rate");
      out("sim <days>      fast-forward the simulation");
      out("demo            build the demo town here");
      out("newcity         start a new map");
      out("cam <dist>      camera distance");
      out("blur | anim | stats   HUD stress toggles (F2 / F1 / F3)");
      out("js <code>       run JavaScript in the HUD document");
      out("fps             frame timing");
      out("undo            undo the last tool action");
      out("unlock          unlock every service");
      out("overlay <id|off>  coverage overlay (power water health edu police park transit)");
      out("brush <1-6>     terrain brush radius");
      out("policy <id|list>  toggle a city policy");
      out("save | load     quick save / load");
      out("crash           deliberate access violation (tests the crash report)");
      out("quit            exit");
    }
    else if (cmd == "crash")
    {
      out(std::string("crashing on purpose; report goes to ") + CitySliceCrashReportPath());
      std::fflush(stdout);
      volatile int* nowhere = nullptr;
      *nowhere = 1;
    }
    else if (cmd == "undo") out(city.Undo() ? "undone" : "nothing to undo");
    else if (cmd == "unlock") city.UnlockAll(), out("all services unlocked");
    else if (cmd == "overlay")
    {
      int kind = -1;
      for (int k = 0; k < City::ServiceCount; ++k)
        if (args.size() > 1 && args[1] == City::kServices[k].id) kind = k;
      g_hud.overlay = kind;
      out(kind < 0 ? "overlay off" : std::string("overlay ") + City::kServices[kind].id);
    }
    else if (cmd == "brush")
    {
      if (args.size() > 1) tool.brush = std::clamp(static_cast<int>(num(1, 2)), 1, 6);
      out("brush = " + std::to_string(tool.brush));
    }
    else if (cmd == "policy")
    {
      if (args.size() < 2)
      {
        for (int i = 0; i < City::PolicyCount; ++i)
          out(std::string(city.HasPolicy(1u << i) ? "[x] " : "[ ] ") + City::kPolicies[i].id + "  "
            + City::kPolicies[i].name);
      }
      else
      {
        bool found = false;
        for (int i = 0; i < City::PolicyCount; ++i)
          if (args[1] == City::kPolicies[i].id)
          {
            found = true;
            out(std::string(City::kPolicies[i].name) + (city.TogglePolicy(1u << i) ? ": on" : ": off"));
          }
        if (!found) err("Unknown policy '" + args[1] + "'. Type policy for the list.");
      }
    }
    else if (cmd == "save") out(city.Save(SavePath()) ? "saved to " + SavePath() : "could not write " + SavePath());
    else if (cmd == "load")
    {
      if (city.Load(SavePath()))
      {
        renderer.InvalidateTerrain();
        tool = CityRenderer::ToolPreview {};
        mapTimer = 10.0f;
        historyTimer = 10.0f;
        lastHistoryDay = -1;
        out("loaded " + SavePath());
      }
      else
        err("could not load " + SavePath());
    }
    else if (cmd == "clear") g_console.lines.clear(), g_console.dirty = true;
    else if (cmd == "echo") out(rest);
    else if (cmd == "money")
    {
      if (args.size() > 1) city.money = num(1, city.money);
      out("money = " + std::to_string(static_cast<int>(city.money)));
    }
    else if (cmd == "speed")
    {
      if (args.size() > 1) city.speed = std::clamp(static_cast<int>(num(1, 1)), 0, 3);
      out("speed = " + std::to_string(city.speed));
    }
    else if (cmd == "time")
    {
      if (args.size() > 1) city.day = std::floor(city.day) + Clamp(num(1, 12.0f), 0.0f, 23.99f) / 24.0;
      out("time = " + std::to_string(city.Hour()));
    }
    else if (cmd == "tax")
    {
      if (args.size() > 1) city.SetTaxRate(static_cast<int>(num(1, 10)));
      out("tax = " + std::to_string(city.taxRate) + "%");
    }
    else if (cmd == "sim")
    {
      const int days = std::clamp(static_cast<int>(num(1, 1)), 1, 60);
      const int saved = city.speed;
      city.speed = 3;
      for (int i = 0; i < days * 160; ++i)
        city.Tick(0.1f);
      city.speed = saved;
      out("simulated " + std::to_string(days) + " day(s): population " + std::to_string(city.population));
    }
    else if (cmd == "demo") BuildDemoCity(city, camera), out("demo town built");
    else if (cmd == "newcity") g_hud.newCity = true;
    else if (cmd == "cam")
    {
      if (args.size() > 1) camera.dist = Clamp(num(1, camera.dist), 20.0f, 900.0f);
      out("cam = " + std::to_string(static_cast<int>(camera.dist)));
    }
    else if (cmd == "blur") runJs("document.body.classList.toggle('blur')"), hudBlur = !hudBlur, out(hudBlur ? "backdrop blur on" : "backdrop blur off");
    else if (cmd == "anim") runJs("document.body.classList.toggle('no-anim')"), hudNoAnim = !hudNoAnim, out(hudNoAnim ? "animations off" : "animations on");
    else if (cmd == "stats") pushStats = !pushStats, out(pushStats ? "stats pushes on" : "stats pushes off");
    else if (cmd == "js") runJs(rest), out("ok");
    else if (cmd == "fps")
    {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%.0f fps, %.1f ms/frame", fps, frameMs);
      out(buf);
    }
    else if (cmd == "quit") running = false;
    else err("Unknown command '" + cmd + "'. Type help.");
  };
  auto consoleComplete = [&]() {
    std::vector<std::string> matches;
    for (const char* c : kConsoleCommands)
      if (std::strncmp(c, g_console.input.c_str(), g_console.input.size()) == 0)
        matches.push_back(c);
    if (matches.size() == 1)
      g_console.input = matches[0] + " ";
    else if (matches.size() > 1)
    {
      std::string s;
      for (const auto& m : matches) s += (s.empty() ? "" : "  ") + m;
      g_console.Add(4, s);
    }
    g_console.dirty = true;
  };
  auto msSince = [](std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  };
  float mouseX = 0.0f, mouseY = 0.0f;
  bool orbiting = false, panning = false, leftHeld = false, leftOnMap = false;
  bool terrainHeld = false;
  float terrainRepeat = 0.0f;
  Vec3 hoverHit;
  bool hoverOnWater = false;
  bool keys[SDL_SCANCODE_COUNT] = {};
  std::string mapString;

  while (running)
  {
    float scale = SDL_GetWindowPixelDensity(window);
    if (scale <= 0.0f) scale = 1.0f;

    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
      switch (e.type)
      {
        case SDL_EVENT_QUIT:
          running = false;
          break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
          if (pixelW > 0 && pixelH > 0)
          {
            context->Resize(static_cast<nsUInt32>(pixelW), static_cast<nsUInt32>(pixelH));
            const nsUInt32 vw = static_cast<nsUInt32>(pixelW), vh = static_cast<nsUInt32>(pixelH);
            hud.Post([view, vw, vh]() { view->setViewportSize(vw, vh); });
          }
          break;

        case SDL_EVENT_MOUSE_MOTION:
          mouseX = e.motion.x * scale;
          mouseY = e.motion.y * scale;
          hud.mouseX = mouseX;
          hud.mouseY = mouseY;
          {
            const float px = mouseX, py = mouseY;
            hud.Post([view, px, py]() { view->onMouseMove(px, py); });
          }
          if (orbiting)
            camera.Orbit(e.motion.xrel * scale, e.motion.yrel * scale);
          if (panning)
          {
            const float metresPerPixel = camera.dist * 0.0018f;
            camera.Pan(-e.motion.xrel * scale * metresPerPixel, e.motion.yrel * scale * metresPerPixel);
          }
          break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
          const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
          mouseX = e.button.x * scale;
          mouseY = e.button.y * scale;
          aperture::UIView::MouseButton btn = aperture::UIView::MouseButton::Left;
          if (e.button.button == SDL_BUTTON_RIGHT) btn = aperture::UIView::MouseButton::Right;
          else if (e.button.button == SDL_BUTTON_MIDDLE) btn = aperture::UIView::MouseButton::Middle;

          // Routing uses the HUD thread's last hit test (at most a frame old).
          const bool overHud = hud.overHud.load();
          {
            const float px = mouseX, py = mouseY;
            hud.Post([view, px, py, btn, down]() {
              if (down)
                view->onMouseDown(px, py, btn);
              else
              {
                view->onMouseUp(px, py, btn);
                view->onClick(px, py, btn);
              }
            });
          }

          if (e.button.button == SDL_BUTTON_RIGHT)
          {
            orbiting = down && !overHud;
            if (down && !overHud && tool.dragging)
              tool.dragging = false; // right-click cancels a drag in progress
            else if (down && !overHud && tool.kind != ToolSelect && !leftHeld)
              tool.kind = ToolSelect; // ...or deselects the tool, like the original
          }
          else if (e.button.button == SDL_BUTTON_MIDDLE)
            panning = down && !overHud;
          else if (e.button.button == SDL_BUTTON_LEFT)
          {
            if (down)
            {
              leftHeld = true;
              leftOnMap = !overHud;
              if (leftOnMap && City::InBounds(tool.hoverX, tool.hoverY))
              {
                if (tool.kind == ToolSelect)
                {
                  const bool same = tool.selX == tool.hoverX && tool.selY == tool.hoverY;
                  tool.selX = same ? -1 : tool.hoverX;
                  tool.selY = same ? -1 : tool.hoverY;
                  if (city.IsWater(tool.hoverX, tool.hoverY))
                    renderer.AddSplash(hoverHit.x, hoverHit.z, 4.0f, 0.9f); // poke the lake
                }
                else if (IsServiceTool(tool.kind))
                {
                  city.BeginAction();
                  if (!city.PlaceService(tool.hoverX, tool.hoverY, static_cast<City::ServiceKind>(tool.kind - ToolPower)))
                    city.DiscardAction();
                  mapTimer = 10.0f;
                }
                else if (tool.kind == ToolRaise || tool.kind == ToolLower)
                {
                  city.BeginAction();
                  terrainHeld = true;
                  terrainRepeat = 0.0f;
                }
                else
                {
                  tool.dragging = true;
                  tool.x0 = tool.x1 = tool.hoverX;
                  tool.y0 = tool.y1 = tool.hoverY;
                }
              }
            }
            else
            {
              leftHeld = false;
              terrainHeld = false;
              if (tool.dragging)
              {
                tool.dragging = false;
                city.BeginAction();
                int changed = 0;
                switch (tool.kind)
                {
                  case ToolRoad: changed = city.BuildRoad(tool.x0, tool.y0, tool.x1, tool.y1, 0); break;
                  case ToolAvenue: changed = city.BuildRoad(tool.x0, tool.y0, tool.x1, tool.y1, 1); break;
                  case ToolRes: changed = city.ZoneRect(tool.x0, tool.y0, tool.x1, tool.y1, City::Res); break;
                  case ToolCom: changed = city.ZoneRect(tool.x0, tool.y0, tool.x1, tool.y1, City::Com); break;
                  case ToolInd: changed = city.ZoneRect(tool.x0, tool.y0, tool.x1, tool.y1, City::Ind); break;
                  case ToolBulldoze: changed = city.BulldozeRect(tool.x0, tool.y0, tool.x1, tool.y1); break;
                  case ToolFlatten: changed = city.FlattenRect(tool.x0, tool.y0, tool.x1, tool.y1); break;
                  case ToolTrees: changed = city.PlantTrees(tool.x0, tool.y0, tool.x1, tool.y1); break;
                  default: break;
                }
                if (!changed)
                  city.DiscardAction(); // nothing happened: keep the undo stack meaningful
                mapTimer = 10.0f; // refresh the minimap right away
              }
            }
          }
          break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
          const float wx = e.wheel.mouse_x * scale, wy = e.wheel.mouse_y * scale;
          if (hud.overHud.load())
          {
            const float dx = -e.wheel.x * 40.0f, dy = -e.wheel.y * 40.0f;
            hud.Post([view, wx, wy, dx, dy]() { view->onWheel(wx, wy, dx, dy); });
          }
          else
            camera.Zoom(e.wheel.y);
          break;
        }

        case SDL_EVENT_TEXT_INPUT:
        {
          if (g_console.open && e.text.text)
          {
            for (const char* c = e.text.text; *c; ++c)
              if (*c != '`')
                g_console.input.push_back(*c);
            g_console.dirty = true;
          }
          break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
          const bool down = e.type == SDL_EVENT_KEY_DOWN;
          if (down && !e.key.repeat && e.key.key == SDLK_GRAVE)
          {
            g_console.open = !g_console.open;
            g_console.dirty = true;
            if (g_console.open)
              SDL_StartTextInput(window);
            else
              SDL_StopTextInput(window);
            std::memset(keys, 0, sizeof(keys));
            break;
          }
          if (g_console.open)
          {
            // The console owns the keyboard: edit keys here, text via TEXT_INPUT.
            if (down)
            {
              switch (e.key.key)
              {
                case SDLK_BACKSPACE:
                  if (!g_console.input.empty())
                  {
                    // drop one UTF-8 code point
                    size_t n = g_console.input.size() - 1;
                    while (n > 0 && (static_cast<unsigned char>(g_console.input[n]) & 0xC0) == 0x80) --n;
                    g_console.input.resize(n);
                  }
                  break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                {
                  const std::string cmd = g_console.input;
                  g_console.input.clear();
                  runConsoleCommand(cmd);
                  break;
                }
                case SDLK_UP:
                  if (!g_console.history.empty())
                  {
                    if (g_console.histPos < 0) g_console.histPos = static_cast<int>(g_console.history.size());
                    g_console.histPos = std::max(0, g_console.histPos - 1);
                    g_console.input = g_console.history[g_console.histPos];
                  }
                  break;
                case SDLK_DOWN:
                  if (g_console.histPos >= 0)
                  {
                    ++g_console.histPos;
                    if (g_console.histPos >= static_cast<int>(g_console.history.size()))
                    {
                      g_console.histPos = -1;
                      g_console.input.clear();
                    }
                    else
                      g_console.input = g_console.history[g_console.histPos];
                  }
                  break;
                case SDLK_TAB: consoleComplete(); break;
                case SDLK_ESCAPE:
                  g_console.open = false;
                  SDL_StopTextInput(window);
                  break;
                default: break;
              }
              g_console.dirty = true;
            }
            break;
          }
          if (e.key.scancode < SDL_SCANCODE_COUNT)
            keys[e.key.scancode] = down;
          if (down && !e.key.repeat)
          {
            switch (e.key.key)
            {
              case SDLK_ESCAPE:
                if (tool.dragging) tool.dragging = false;
                else if (tool.kind != ToolSelect) tool.kind = ToolSelect;
                else { tool.selX = tool.selY = -1; }
                break;
              case SDLK_SPACE: city.speed = city.speed == 0 ? 1 : 0; break;
              case SDLK_1: city.speed = 1; break;
              case SDLK_2: city.speed = 2; break;
              case SDLK_3: city.speed = 3; break;
              case SDLK_R: tool.kind = ToolRoad; break;
              case SDLK_X: tool.kind = ToolCom; break;
              case SDLK_C: tool.kind = ToolInd; break;
              case SDLK_B: tool.kind = ToolBulldoze; break;
              case SDLK_V: tool.kind = ToolAvenue; break;
              case SDLK_T: tool.kind = ToolTrees; break;
              case SDLK_G: tool.kind = ToolRaise; break;
              case SDLK_H: tool.kind = ToolLower; break;
              case SDLK_F: tool.kind = ToolFlatten; break;
              case SDLK_LEFTBRACKET: tool.brush = std::max(1, tool.brush - 1); break;
              case SDLK_RIGHTBRACKET: tool.brush = std::min(6, tool.brush + 1); break;
              case SDLK_Z:
                if (e.key.mod & SDL_KMOD_CTRL) { city.Undo(); mapTimer = 10.0f; }
                else tool.kind = ToolRes;
                break;
              // Stress-test toggles: attribute HUD cost to animations / blur / pushes.
              case SDLK_F1:
              {
                postScript("toggle", "document.body.classList.toggle('no-anim')");
                hudNoAnim = !hudNoAnim;
                break;
              }
              case SDLK_F2:
              {
                postScript("toggle", "document.body.classList.toggle('blur')");
                hudBlur = !hudBlur;
                break;
              }
              case SDLK_F3: pushStats = !pushStats; break;
              case SDLK_HOME:
                camera.target = { City::N * City::kCell * 0.5f, 0.0f, City::N * City::kCell * 0.5f };
                camera.dist = 260.0f;
                break;
              default: break;
            }
          }
          break;
        }
        default:
          break;
      }
    }

    const auto now = clock::now();
    g_frameStartNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    ++g_frameSeq;
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    dt = Clamp(dt, 0.0f, 0.1f);
    if (bench)
    {
      benchTime += dt;
      city.speed = 3;
      camera.yaw = 0.55f + benchTime * 0.25f;
      camera.target = benchCentre + Vec3 { std::cos(benchTime * 0.4f) * 50.0f, 0.0f, std::sin(benchTime * 0.4f) * 50.0f };
      camera.target.y = city.HeightAt(camera.target.x, camera.target.z);
      camera.dist = 120.0f + 60.0f * std::sin(benchTime * 0.3f);
      tool.hoverX = static_cast<int>(camera.target.x / City::kCell);
      tool.hoverY = static_cast<int>(camera.target.z / City::kCell);
      if (benchTime > 30.0f)
        running = false;
    }
    realTime += dt;
    if (crashTestAt >= 0.0f && realTime >= crashTestAt)
    {
      std::printf("--crashtest: raising an access violation now (report: %s)\n", CitySliceCrashReportPath());
      std::fflush(stdout);
      volatile int* nowhere = nullptr;
      *nowhere = 1;
    }

    // ---- HUD frame N starts now, in parallel with the CPU work below --------
    // (only when the previous one finished: a late frame is skipped, not queued)
    if (hud.Done())
    {
      if (hud.commands)
        hudCommands = *hud.commands;
      timing.ui += hud.lastTickMs.load();
      timing.build += hud.lastMs.load() - hud.lastTickMs.load();
      timing.maxBuild = std::max(timing.maxBuild, hud.lastMs.load());
      hud.Kick(dt);
    }
    else
      ++hudLateFrames;

    // ---- HUD requests (binders wrote them on the HUD thread) ----------------
    HudRequests req;
    {
      std::lock_guard<std::mutex> lock(g_hudMutex);
      req = g_hud;
      g_hud = HudRequests {};
    }
    if (req.tool >= 0) { tool.kind = req.tool; tool.dragging = false; }
    if (req.speed >= 0) city.speed = std::clamp(req.speed, 0, 3);
    if (req.taxDelta != 0) city.SetTaxRate(city.taxRate + req.taxDelta);
    if (req.clearSelection) tool.selX = tool.selY = -1;
    if (req.undo) { city.Undo(); mapTimer = 10.0f; }
    if (req.brush > 0) tool.brush = req.brush;
    if (req.unlock) city.UnlockAll();
    for (int i = 0; i < City::PolicyCount; ++i)
      if (req.policyToggle & (1u << i))
        city.TogglePolicy(1u << i);
    if (req.saveLoad == 1)
      city.events.push_back(city.Save(SavePath()) ? "City saved." : "Could not write the save file.");
    else if (req.saveLoad == 2 && city.Load(SavePath()))
    {
      renderer.InvalidateTerrain();
      tool = CityRenderer::ToolPreview {};
      mapTimer = 10.0f;
      historyTimer = 10.0f;
      lastHistoryDay = -1;
    }
    static int s_overlayRequest = startOverlay;
    if (req.overlay >= -1) s_overlayRequest = req.overlay;
    tool.overlay = IsServiceTool(tool.kind) ? tool.kind - ToolPower : s_overlayRequest;
    if (req.lookAt)
    {
      const float extent = City::N * City::kCell;
      camera.target.x = Clamp(req.lookX, 0.0f, 1.0f) * extent;
      camera.target.z = Clamp(req.lookY, 0.0f, 1.0f) * extent;
    }
    if (req.newCity)
    {
      city.Reset(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
      renderer.InvalidateTerrain();
      tool = CityRenderer::ToolPreview {};
      s_overlayRequest = -1;
      mapTimer = 10.0f;
      historyTimer = 10.0f;
      lastHistoryDay = -1;
    }

    // ---- camera keys ---------------------------------------------------------
    {
      const float panSpeed = camera.dist * 0.9f * dt;
      float dx = 0.0f, dz = 0.0f;
      if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dz += panSpeed;
      if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dz -= panSpeed;
      if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx += panSpeed;
      if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx -= panSpeed;
      if (dx != 0.0f || dz != 0.0f) camera.Pan(dx, dz);
      if (keys[SDL_SCANCODE_Q]) camera.yaw += 1.5f * dt;
      if (keys[SDL_SCANCODE_E]) camera.yaw -= 1.5f * dt;
      const float extent = City::N * City::kCell;
      camera.target.x = Clamp(camera.target.x, 0.0f, extent);
      camera.target.z = Clamp(camera.target.z, 0.0f, extent);
      camera.target.y = city.HeightAt(camera.target.x, camera.target.z);
      camera.Update(dt);
    }

    // ---- hover cell ----------------------------------------------------------
    {
      Vec3 hit;
      const bool overHud = hud.overHud.load();
      if (!overHud && pixelW > 0 && pixelH > 0
        && PickGround(camera, city, mouseX, mouseY, static_cast<float>(pixelW), static_cast<float>(pixelH), hit))
      {
        tool.hoverX = static_cast<int>(hit.x / City::kCell);
        tool.hoverY = static_cast<int>(hit.z / City::kCell);
        const bool water = city.IsWater(tool.hoverX, tool.hoverY);
        // The cursor drags a little wake across the lake.
        if (water && (hit - hoverHit).Length() > 0.3f)
          renderer.AddSplash(hit.x, hit.z, 1.8f, 0.05f);
        hoverHit = hit;
        hoverOnWater = water;
      }
      else if (!tool.dragging && !terrainHeld)
      {
        tool.hoverX = tool.hoverY = -1;
      }
      if (terrainHeld && City::InBounds(tool.hoverX, tool.hoverY))
      {
        terrainRepeat -= dt;
        if (terrainRepeat <= 0.0f)
        {
          terrainRepeat = 0.12f;
          city.ModifyTerrain(tool.hoverX, tool.hoverY, tool.brush, tool.kind == ToolRaise ? 0.8f : -0.8f);
          mapTimer = 10.0f;
        }
      }
      if (tool.dragging && City::InBounds(tool.hoverX, tool.hoverY))
      {
        tool.x1 = tool.hoverX;
        tool.y1 = tool.hoverY;
      }
    }

    // ---- simulate + HUD ------------------------------------------------------
    auto tPhase = clock::now();
    {
      NS_PROFILE_SCOPE("Game::Simulate");
      city.Tick(dt);
    }
    timing.sim += msSince(tPhase); tPhase = clock::now();

    if (!hudPanelsJson.empty() && city.gameSeconds > 1.0f)
    {
      postCall("__cityPanels", hudPanelsJson);
      hudPanelsJson.clear();
    }
    statsTimer += dt;
    mapTimer += dt;
    const auto tJs = clock::now();
    if (statsTimer >= 0.2f && pushStats)
    {
      statsTimer = 0.0f;
      std::string events = "[";
      for (size_t i = 0; i < city.events.size(); ++i)
        events += (i ? "," : "") + JsonQuote(city.events[i]);
      events += "]";
      city.events.clear();
      const std::string hover = HoverInfo(city, tool.hoverX, tool.hoverY);
      const std::string sel = SelectionJson(city, tool.selX, tool.selY);
      const float extent = City::N * City::kCell;
      std::string services = "[";
      for (int k = 0; k < City::ServiceCount; ++k)
      {
        const City::ServiceStats& st = city.ServiceStatsFor(k);
        char sb[160];
        std::snprintf(sb, sizeof(sb), "%s{\"id\":\"%s\",\"buildings\":%d,\"capacity\":%d,\"demand\":%d,\"covered\":%d}",
          k ? "," : "", City::kServices[k].id, st.buildings, st.capacity, st.demand, st.covered);
        services += sb;
      }
      services += "]";
      char json[4096];
      std::snprintf(json, sizeof(json),
        "{\"money\":%.0f,\"income\":%.0f,\"population\":%d,\"jobs\":%d,\"roads\":%d,\"buildings\":%d,"
        "\"day\":%.3f,\"hour\":%.2f,\"speed\":%d,\"demandR\":%.2f,\"demandC\":%.2f,\"demandI\":%.2f,"
        "\"tax\":%d,\"happiness\":%.0f,\"budget\":{\"res\":%.0f,\"com\":%.0f,\"ind\":%.0f,\"upkeep\":%.0f,\"services\":%.0f,\"policy\":%.0f},"
        "\"tool\":\"%s\",\"hover\":%s,\"cars\":%d,\"camX\":%.3f,\"camY\":%.3f,\"camYaw\":%.3f,"
        "\"fps\":%.0f,\"frameMs\":%.1f,\"milestone\":%d,\"unpowered\":%d,\"unwatered\":%d,\"canUndo\":%s,"
        "\"brush\":%d,\"overlay\":%d,\"policies\":%u,\"policyCost\":%.0f,"
        "\"services\":%s,\"selection\":%s,\"events\":%s}",
        city.money, city.lastDayIncome, city.population, city.jobs, city.roadTiles, city.buildings,
        city.day, city.Hour(), city.speed, city.demandR, city.demandC, city.demandI, city.taxRate,
        city.happiness, city.budget.resTax, city.budget.comTax, city.budget.indTax, city.budget.roadUpkeep,
        city.budget.serviceUpkeep, city.budget.policyCost, ToolName(tool.kind), JsonQuote(hover).c_str(), static_cast<int>(city.vehicles.size()),
        camera.sTarget.x / extent, camera.sTarget.z / extent, camera.sYaw, fps, frameMs, city.milestone,
        city.unpowered, city.unwatered, city.CanUndo() ? "true" : "false", tool.brush, tool.overlay,
        city.policies, city.budget.policyCost,
        services.c_str(), sel.c_str(), events.c_str());
      if (pushMode == 1)
      {
        const std::string quoted = JsQuote(json);
        postScript("stats", "var b=document.getElementById('bridge');if(b)b.setAttribute('data-stats',"
          + quoted + ");if(window.__cityStats)window.__cityStats(JSON.parse(" + quoted + "));");
      }
      else
        postCall("__cityStats", json);
    }
    // Charts only move once per in-game day, so they get their own slow push
    // instead of riding along with the 5 Hz stats blob.
    historyTimer += dt;
    if (historyTimer >= 1.0f && pushStats && static_cast<int>(city.day) != lastHistoryDay)
    {
      historyTimer = 0.0f;
      lastHistoryDay = static_cast<int>(city.day);
      std::string series[5];
      for (int i = 0; i < city.historyCount; ++i)
      {
        const int idx = (city.historyHead - city.historyCount + i + City::kHistoryLength * 2) % City::kHistoryLength;
        const City::HistorySample& h = city.history[idx];
        const float v[5] = { h.population, h.money, h.happiness, h.income, h.jobs };
        for (int k = 0; k < 5; ++k)
        {
          char b[24];
          std::snprintf(b, sizeof(b), "%s%.0f", i ? "," : "", v[k]);
          series[k] += b;
        }
      }
      postCall("__cityHistory", "{\"population\":[" + series[0] + "],\"money\":[" + series[1]
        + "],\"happiness\":[" + series[2] + "],\"income\":[" + series[3]
        + "],\"jobs\":[" + series[4] + "]}");
    }
    if (mapTimer >= 1.0f && pushStats)
    {
      mapTimer = 0.0f;
      city.MinimapString(mapString);
      const std::string n = std::to_string(City::N);
      if (pushMode == 1)
        postScript("map", "var b=document.getElementById('bridge');if(b)b.setAttribute('data-map','" + n + ":"
          + mapString + "');if(window.__cityMap)window.__cityMap(" + n + ",'" + mapString + "');");
      else
        postCall("__cityMapJson", "{\"size\":" + n + ",\"cells\":" + JsonQuote(mapString) + "}");
    }
    g_console.Flush();
    if (g_console.dirty && (g_console.open || g_console.pushedOpen))
    {
      g_console.dirty = false;
      g_console.pushedOpen = g_console.open;
      std::string json = std::string("{\"open\":") + (g_console.open ? "true" : "false") + ",\"input\":"
        + JsonQuote(g_console.input) + ",\"lines\":[";
      const size_t first = g_console.lines.size() > 60 ? g_console.lines.size() - 60 : 0;
      for (size_t i = first; i < g_console.lines.size(); ++i)
        json += (i > first ? "," : "") + std::string("[") + std::to_string(g_console.lines[i].first) + ","
          + JsonQuote(g_console.lines[i].second) + "]";
      json += "]}";
      if (pushMode == 1)
        postScript("console", "if(window.__cityConsole)window.__cityConsole(JSON.parse(" + JsQuote(json) + "));");
      else
        postCall("__cityConsole", json);
    }

    { const double _j = msSince(tJs); timing.js += _j; timing.maxJs = std::max(timing.maxJs, _j); }
    tPhase = clock::now();
    // ---- world CPU work while the HUD thread builds ----------------------------
    {
      NS_PROFILE_SCOPE("World::BuildCpu");
      renderer.BuildCpu(city, camera, tool, realTime);
    }
    // ---- HUD handoff: this frame's list if it made it in time -----------------
    {
      NS_PROFILE_SCOPE("HUD::Handoff");
      if (hud.WaitDone(hudWaitBudgetMs) && hud.commands)
        hudCommands = *hud.commands;
    }
    const auto& commands = hudCommands;
    tPhase = clock::now();

    // ---- present -------------------------------------------------------------
    const Vec3 sky = CityRenderer::HorizonColor(city.day);
    {
      NS_PROFILE_SCOPE("Frame::BeginFrame+World");
      context->BeginFrame(nsColor(sky.x, sky.y, sky.z, 1.0f)); // scene callbacks fire in here
    }
    { const double _m = msSince(tPhase); timing.world += _m; timing.maxWorld = std::max(timing.maxWorld, _m); tPhase = clock::now(); }
    if (!context->IsFrameOpen())
      continue; // swapchain out of date; recreated on the next BeginFrame
    {
      NS_PROFILE_SCOPE("HUD::Submit");
      context->RenderCommands(commands);
      context->SubmitCommands();
    }
    { const double _m = msSince(tPhase); timing.submit += _m; timing.maxSubmit = std::max(timing.maxSubmit, _m); tPhase = clock::now(); }
    {
      NS_PROFILE_SCOPE("Frame::Present");
      context->EndFrame(true);
    }
    timing.present += msSince(tPhase);
    NS_PROFILER_FRAME_MARKER();

    std::fflush(stdout);
    ++timing.frames;
    timing.window += dt;
    timing.maxFrame = std::max(timing.maxFrame, static_cast<double>(dt) * 1000.0);
    if (timing.window >= 2.0f)
    {
      const double n = timing.frames;
      fps = static_cast<float>(n / timing.window);
      frameMs = static_cast<float>(1000.0 * timing.window / n);
      std::printf("frame %.1f ms (%.0f fps) max %.1f [build %.1f world %.1f submit %.1f js %.1f/avg %.2f] [anim=%d blur=%d push=%d]: sim %.2f  ui-tick %.2f  build %.2f  world %.2f  ui-submit %.2f  present %.2f\n",
        frameMs, fps, timing.maxFrame, timing.maxBuild, timing.maxWorld, timing.maxSubmit, timing.maxJs, timing.js / n,
        hudNoAnim ? 0 : 1, hudBlur ? 1 : 0, pushStats ? 1 : 0, timing.sim / n, timing.ui / n,
        timing.build / n, timing.world / n, timing.submit / n, timing.present / n);
      timing = Timing {};
    }
  }

  // ---- teardown (HUD thread first, then the view, then the platform) ---------
  hud.Stop();
  if (hudLateFrames)
    std::printf("HUD thread: %d late frame(s) reused the previous command list\n", hudLateFrames);
  if (devServer)
    devServer->Shutdown();
  devServer.reset();
  devProvider.reset();
  binder.reset();
  bridge.reset();
  nsGlobalLog::RemoveLogWriter(&ConsoleLogHandler);
  renderer.Shutdown(); // waits for the device to go idle first
  context->Shutdown();
  context.reset();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
