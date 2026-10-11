/*
 * City.h - CitySlice simulation: heightfield terrain, road/zone/service grid,
 * RCI growth driven by demand, land desirability and service coverage, taxes,
 * happiness, milestones, traffic and boats. Pure C++, no engine dependencies,
 * so it can be self-tested from the command line (see --selftest in main.cpp).
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class City
{
public:
  static constexpr int N = 96;               ///< tiles per side
  static constexpr float kCell = 8.0f;       ///< metres per tile
  static constexpr float kWaterLevel = 1.6f;
  static constexpr float kBridgeDeck = 4.6f; ///< bridge road surface height (world Y)
  static constexpr float kRoadCost = 40.0f;
  static constexpr float kAvenueCost = 90.0f;
  static constexpr float kBridgeCost = 160.0f; ///< per road tile over water
  static constexpr float kRoadRefund = 15.0f;  ///< bulldozing a road gives some back
  static constexpr float kBulldozeCost = 10.0f;
  static constexpr float kTerrainCost = 6.0f;  ///< per metre of corner moved
  static constexpr float kTreeCost = 12.0f;    ///< per tile planted
  static constexpr float kDaySeconds = 48.0f;  ///< game seconds per in-game day
  static constexpr int kMaxLevel = 3;
  static constexpr float kSpawnAnimSeconds = 0.7f; ///< buildings rise over this (game time)

  enum Type : uint8_t { Empty = 0, Road, Res, Com, Ind, Service };

  /// City-wide policies. Each costs money per day and bends the simulation;
  /// the HUD toggles them and shows what they do.
  enum Policy : uint32_t
  {
    PolicyRecycling = 1 << 0,     ///< industry pollutes neighbours less, small happiness gain
    PolicyFreeTransit = 1 << 1,   ///< transit coverage counts double, commerce grows faster
    PolicyEducationGrant = 1 << 2,///< schooled tiles grow faster
    PolicyShopHoliday = 1 << 3,   ///< commercial tax halved, commercial demand up
    PolicyNightLighting = 1 << 4, ///< happiness gain, upkeep per road tile
    PolicyCount = 5
  };
  struct PolicyInfo
  {
    const char* id;
    const char* name;
    const char* blurb;
    float dailyCost;      ///< flat part
    float perBuilding;    ///< scaled part
  };
  static const PolicyInfo kPolicies[PolicyCount];

  /// One sample per in-game day, for the statistics charts.
  struct HistorySample
  {
    float population = 0.0f, money = 0.0f, happiness = 0.0f, income = 0.0f, jobs = 0.0f;
  };
  static constexpr int kHistoryLength = 120;

  /// Service buildings. Order matters: it indexes kServices and the coverage bits.
  enum ServiceKind : uint8_t { PowerPlant = 0, WaterTower, Clinic, School, PoliceStation, Park, BusDepot, ServiceCount };

  /// Coverage bits stored per tile (one per ServiceKind).
  enum CoverageBit : uint8_t
  {
    CovPower = 1 << PowerPlant, CovWater = 1 << WaterTower, CovHealth = 1 << Clinic,
    CovEdu = 1 << School, CovPolice = 1 << PoliceStation, CovPark = 1 << Park, CovTransit = 1 << BusDepot
  };

  struct ServiceInfo
  {
    const char* id;    ///< tool / HUD id
    const char* name;
    float cost;
    float upkeep;      ///< per day
    int radius;        ///< road-network reach in tiles
    int capacity;      ///< kW, water units, patients, pupils, officers, visitors, passengers
    int unlockMilestone; ///< index into the milestone table (0 = from the start)
  };
  static const ServiceInfo kServices[ServiceCount];

  struct Tile
  {
    Type type = Empty;
    uint8_t level = 0;    ///< 0 = zoned but vacant lot
    uint8_t variant = 0;  ///< cosmetic seed for the building generator
    uint8_t road = 0;     ///< Road tiles: 0 street, 1 avenue
    uint8_t service = 0;  ///< Service tiles: ServiceKind
    uint8_t planted = 0;  ///< Empty tiles: player-planted trees
    uint8_t coverage = 0; ///< CoverageBit mask, refreshed every growth step
    bool powered = false; ///< covered by power with capacity to spare
    bool watered = false;
    float growth = 0.0f;
    float desirability = 0.0f; ///< 0..1, refreshed every growth step (zoned tiles)
    float spawnTime = -100.0f; ///< gameSeconds when the level last changed (rise animation)
  };

  struct Vehicle
  {
    int x, y;   ///< current tile
    int nx, ny; ///< tile being driven towards
    float t;    ///< 0..1 progress between the two
    uint32_t color;
  };

  struct Boat
  {
    float x, z;     ///< world position
    float heading;  ///< radians, 0 = +x
    float speed;    ///< m/s
    uint32_t color;
    float bob;      ///< phase offset for the bobbing animation
  };

  struct Budget
  {
    float resTax = 0.0f, comTax = 0.0f, indTax = 0.0f, roadUpkeep = 0.0f, serviceUpkeep = 0.0f,
      policyCost = 0.0f;
  };

  /// Per-service totals for the HUD.
  struct ServiceStats
  {
    int buildings = 0;
    int capacity = 0;
    int demand = 0;   ///< consumption (power/water) or people covered (the rest)
    int covered = 0;  ///< zoned tiles covered
  };

  City();
  void Reset(uint32_t seed);

  // ---- queries -----------------------------------------------------------
  static bool InBounds(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
  const Tile& At(int x, int y) const { return m_tiles[y * N + x]; }
  /// Height of grid corner (cx,cy), 0..N inclusive.
  float CornerHeight(int cx, int cy) const { return m_height[cy * (N + 1) + cx]; }
  const std::vector<float>& Heights() const { return m_height; }
  /// Bilinear terrain height at a world position.
  float HeightAt(float wx, float wz) const;
  /// Height of the road surface at a world position (bridge decks and their ramps included).
  float RoadHeightAt(float wx, float wz) const;
  /// Road surface height at a tile corner: the deck where any road tile sharing
  /// the corner is a bridge, else the terrain.
  float RoadCornerHeight(int cx, int cy) const;
  /// Highest corner of a tile: buildings and roads sit on this.
  float TileTop(int x, int y) const;
  bool IsWater(int x, int y) const;
  bool IsBridge(int x, int y) const { return IsRoad(x, y) && IsWater(x, y); }
  bool HasRoadAccess(int x, int y) const;
  bool IsRoad(int x, int y) const { return InBounds(x, y) && At(x, y).type == Road; }
  /// Residents / jobs a single tile contributes (for the info card).
  static int TilePopulation(const Tile& t);
  static int TileJobs(const Tile& t);
  static int TilePowerDemand(const Tile& t);
  static int TileWaterDemand(const Tile& t);
  /// Land desirability for a zone type at a tile, 0..1 (road access, neighbours,
  /// water views, industrial pollution, services). Computed live: fine for hover/info.
  float Desirability(int x, int y, Type zone) const;
  /// 0..24 hour of the current in-game day.
  float Hour() const;
  /// One character per tile, row-major, for the HUD minimap:
  /// '.' grass '~' water '#' road '=' bridge/avenue 'r/c/i' vacant lot 'R/C/I' built 'S' service.
  void MinimapString(std::string& out) const;
  const ServiceStats& ServiceStatsFor(int kind) const { return m_serviceStats[kind]; }

  // ---- tools (all charge money and mark geometry dirty) ------------------
  /// L-shaped road (horizontal leg then vertical leg). Returns tiles placed.
  /// Water tiles become bridges (kBridgeCost). kind: 0 street, 1 avenue.
  int BuildRoad(int x0, int y0, int x1, int y1, int kind = 0);
  int ZoneRect(int x0, int y0, int x1, int y1, Type zone);
  int BulldozeRect(int x0, int y0, int x1, int y1);
  bool PlaceService(int x, int y, ServiceKind kind);
  /// Raise (delta > 0) or lower the terrain around a tile with a soft brush.
  int ModifyTerrain(int x, int y, int radius, float delta);
  /// Level every corner in the rectangle to its mean height.
  int FlattenRect(int x0, int y0, int x1, int y1);
  int PlantTrees(int x0, int y0, int x1, int y1);
  /// The cells an L-shaped road from a to b would cover (for the tool preview).
  static void RoadPath(int x0, int y0, int x1, int y1, std::vector<std::pair<int, int>>& out);
  void SetTaxRate(int percent);
  /// Toggles a policy. Returns the new state.
  bool TogglePolicy(uint32_t policy);
  bool HasPolicy(uint32_t policy) const { return (policies & policy) != 0; }
  /// Daily cost of everything currently enabled.
  float PolicyCost() const;

  /// Saves/loads the whole city (tiles, terrain, treasury, policies, history).
  /// Returns false when the file cannot be written or is not a CitySlice save.
  bool Save(const std::string& path) const;
  bool Load(const std::string& path);
  /// Undo: call BeginAction() before a tool, Undo() to roll the last action back.
  void BeginAction();
  bool Undo();
  /// Drop the snapshot BeginAction() took when the tool turned out to do nothing.
  void DiscardAction() { if (!m_undo.empty()) m_undo.pop_back(); }
  bool CanUndo() const { return !m_undo.empty(); }
  void UnlockAll() { milestone = 5; }

  // ---- simulation --------------------------------------------------------
  void Tick(float realSeconds);
  /// Recompute coverage/power/water now (tools call this so the HUD reacts at once).
  void RefreshServices();

  // ---- state read by the HUD/renderer ------------------------------------
  float money = 0.0f;
  float lastDayIncome = 0.0f;
  Budget budget;
  int taxRate = 10;      ///< percent, 5..20
  float happiness = 70.0f; ///< 0..100
  int population = 0;
  int jobs = 0;
  int roadTiles = 0;
  int buildings = 0;
  int unpowered = 0; ///< built tiles without power
  int unwatered = 0;
  int milestone = 0; ///< index into the milestone table reached so far
  float demandR = 1.0f, demandC = 0.5f, demandI = 0.5f;
  double day = 0.0;         ///< fractional in-game days since founding
  float gameSeconds = 0.0f; ///< total simulated seconds (animation clock)
  float animatingUntil = 0.0f; ///< gameSeconds until which buildings are still rising
  int speed = 1;    ///< 0 = paused, 1..3
  bool geometryDirty = true;  ///< tiles changed (roads/zones): renderer rebuilds roads, lots, prunes trees
  bool buildingsDirty = true; ///< a building changed level: renderer rebuilds the building layer only
  bool terrainDirty = true;   ///< heights changed: renderer rebuilds terrain, trees and the heightmap
  uint32_t coverageVersion = 0; ///< bumped whenever coverage bits change (overlay rebuild)
  uint32_t policies = 0;
  /// Ring buffer of daily samples; `historyCount` is how many are valid, the
  /// newest at (historyHead - 1) modulo the length.
  HistorySample history[kHistoryLength] = {};
  int historyHead = 0, historyCount = 0;
  std::vector<Vehicle> vehicles;
  std::vector<Boat> boats;
  /// Notifications for the HUD (milestones, refusals); the host pops them.
  std::vector<std::string> events;
  /// Water splashes the renderer should inject this frame: x, z, radius, strength.
  struct Splash { float x, z, radius, strength; };
  std::vector<Splash> splashes;

private:
  struct Snapshot
  {
    std::vector<Tile> tiles;
    std::vector<float> height;
    float money;
    int roadTiles;
  };
  void GrowthStep();
  void EconomyStep();
  void UpdateVehicles(float gameSeconds);
  void UpdateBoats(float gameDt);
  bool NextRoadNeighbour(int x, int y, int fromX, int fromY, int& nx, int& ny);
  int CountNear(int x, int y, int radius, Type type, bool builtOnly) const;
  void CoverFrom(int sx, int sy, int radius, uint8_t bit);
  void SplashTile(int x, int y, float strength);
  uint32_t Rand() // xorshift32: an LCG's low bits cycle every 32 draws and never hit some tiles
  {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
  }
  float Rand01() { return (Rand() >> 8) * (1.0f / 16777216.0f); }

  std::vector<Tile> m_tiles;
  std::vector<float> m_height;
  std::vector<Snapshot> m_undo;
  ServiceStats m_serviceStats[ServiceCount];
  uint32_t m_rng = 1;
  float m_growthAccum = 0.0f;
  float m_dayAccum = 0.0f;
  float m_spawnAccum = 0.0f;
  float m_brokeCooldown = 0.0f;
};
