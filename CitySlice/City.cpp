/*
 * City.cpp - CitySlice simulation
 */
#include "City.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <cmath>
#include <deque>

namespace
{
  // Hash-based value noise; deterministic per seed so a city can be rebuilt.
  float Hash(int x, int y, uint32_t seed)
  {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u
      + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) * (1.0f / 16777216.0f);
  }

  float SmoothNoise(float x, float y, uint32_t seed)
  {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed);
    const float c = Hash(ix, iy + 1, seed), d = Hash(ix + 1, iy + 1, seed);
    const float top = a + (b - a) * fx;
    const float bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
  }

  // Population / jobs contributed by a building of a given level (index 0 = vacant).
  constexpr int kResPop[4] = { 0, 6, 14, 30 };
  constexpr int kComJobs[4] = { 0, 4, 10, 22 };
  constexpr int kIndJobs[4] = { 0, 6, 16, 36 };
  // Packed as the renderer's R8G8B8A8_UNORM vertex colour: R in the low byte.
  constexpr uint32_t kCarColors[] = { 0xFFE0E0E0, 0xFF3A3A3A, 0xFF4545D6, 0xFFDE862E,
    0xFF3FD0F4, 0xFF9B9B9B, 0xFF60AE27 };
  constexpr uint32_t kBoatColors[] = { 0xFFF0F0F0, 0xFF2E58C8, 0xFFD84A3C, 0xFFE8D8A8 };

  struct Milestone
  {
    int population;
    const char* name;
  };
  constexpr Milestone kMilestones[] = { { 40, "Hamlet" }, { 120, "Village" }, { 350, "Town" },
    { 900, "City" }, { 2000, "Metropolis" } };

  float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
} // namespace

// id, name, cost, upkeep/day, road reach (tiles), capacity, unlock milestone index
const City::ServiceInfo City::kServices[City::ServiceCount] = {
  { "power", "Coal power plant", 3000.0f, 120.0f, 26, 900, 0 },
  { "water", "Water tower", 1500.0f, 60.0f, 24, 600, 0 },
  { "health", "Medical clinic", 2200.0f, 90.0f, 12, 400, 1 },
  { "edu", "Elementary school", 2600.0f, 110.0f, 12, 300, 1 },
  { "police", "Police station", 1800.0f, 80.0f, 14, 500, 2 },
  { "park", "City park", 600.0f, 20.0f, 6, 250, 2 },
  { "transit", "Bus depot", 2400.0f, 100.0f, 20, 800, 3 },
};

// id, name, blurb, flat daily cost, per-building daily cost
const City::PolicyInfo City::kPolicies[City::PolicyCount] = {
  { "recycling", "Recycling programme",
    "Kerbside collection. Industry upsets its neighbours less and everyone is a little happier.",
    40.0f, 0.6f },
  { "transit", "Free public transport",
    "Fares abolished. Bus coverage counts double and shops draw more custom.", 60.0f, 0.5f },
  { "education", "Education grant",
    "Extra funding per pupil. Homes near a school grow faster.", 50.0f, 0.4f },
  { "holiday", "Small business holiday",
    "Commercial tax halved while it stands. Shops open faster in return.", 0.0f, 0.9f },
  { "lighting", "Night lighting",
    "Lit streets everywhere. Safer nights, paid for by the metre of road.", 20.0f, 0.0f },
};

City::City()
{
  Reset(7u);
}

bool City::TogglePolicy(uint32_t policy)
{
  policies ^= policy;
  const bool on = HasPolicy(policy);
  for (int i = 0; i < PolicyCount; ++i)
    if (policy == (1u << i))
      events.push_back(std::string(on ? "Policy enacted: " : "Policy repealed: ") + kPolicies[i].name);
  return on;
}

float City::PolicyCost() const
{
  float cost = 0.0f;
  for (int i = 0; i < PolicyCount; ++i)
  {
    if (!HasPolicy(1u << i))
      continue;
    cost += kPolicies[i].dailyCost + kPolicies[i].perBuilding * buildings;
    if ((1u << i) == PolicyNightLighting)
      cost += roadTiles * 0.12f;
  }
  return cost;
}

void City::Reset(uint32_t seed)
{
  m_rng = seed * 747796405u + 1u; // never zero: xorshift would stick there
  m_tiles.assign(N * N, Tile {});
  m_height.assign((N + 1) * (N + 1), 0.0f);
  m_undo.clear();
  vehicles.clear();
  boats.clear();
  events.clear();
  splashes.clear();
  for (ServiceStats& s : m_serviceStats)
    s = ServiceStats {};

  for (int cy = 0; cy <= N; ++cy)
    for (int cx = 0; cx <= N; ++cx)
    {
      const float x = cx * 0.055f, y = cy * 0.055f;
      float h = SmoothNoise(x, y, seed) * 9.0f + SmoothNoise(x * 2.3f, y * 2.3f, seed + 1) * 3.5f
        + SmoothNoise(x * 5.1f, y * 5.1f, seed + 2) * 1.2f;
      // A shallow bowl towards the centre so the natural build site is flat.
      const float dx = (cx - N * 0.5f) / (N * 0.5f), dy = (cy - N * 0.5f) / (N * 0.5f);
      h += (dx * dx + dy * dy) * 4.0f - 3.2f;
      m_height[cy * (N + 1) + cx] = h;
    }

  // Boats on open water (every neighbour is water too).
  for (int tries = 0; tries < 400 && boats.size() < 6; ++tries)
  {
    const int x = 2 + static_cast<int>(Rand() % (N - 4)), y = 2 + static_cast<int>(Rand() % (N - 4));
    bool open = true;
    for (int dy = -1; dy <= 1 && open; ++dy)
      for (int dx = -1; dx <= 1; ++dx)
        if (!IsWater(x + dx, y + dy) || TileTop(x + dx, y + dy) > kWaterLevel - 0.6f)
        {
          open = false;
          break;
        }
    if (!open)
      continue;
    Boat b;
    b.x = (x + 0.5f) * kCell;
    b.z = (y + 0.5f) * kCell;
    b.heading = Rand01() * 6.2831853f;
    b.speed = 2.5f + Rand01() * 2.0f;
    b.color = kBoatColors[Rand() % 4];
    b.bob = Rand01() * 6.28f;
    boats.push_back(b);
  }

  money = 25000.0f;
  lastDayIncome = 0.0f;
  budget = Budget {};
  taxRate = 10;
  happiness = 70.0f;
  population = jobs = roadTiles = buildings = unpowered = unwatered = 0;
  milestone = 0;
  demandR = 1.0f;
  demandC = 0.5f;
  demandI = 0.5f;
  day = 0.0;
  gameSeconds = 0.0f;
  animatingUntil = 0.0f;
  speed = 1;
  policies = 0;
  historyHead = historyCount = 0;
  for (HistorySample& h : history)
    h = HistorySample {};
  m_growthAccum = m_dayAccum = m_spawnAccum = m_brokeCooldown = 0.0f;
  geometryDirty = buildingsDirty = terrainDirty = true;
  ++coverageVersion;
  events.push_back("Welcome, Mayor. Lay a road, zone beside it, and watch the town grow.");
}

// ============================================================================
// Queries
// ============================================================================

float City::HeightAt(float wx, float wz) const
{
  const float gx = std::clamp(wx / kCell, 0.0f, static_cast<float>(N) - 0.001f);
  const float gz = std::clamp(wz / kCell, 0.0f, static_cast<float>(N) - 0.001f);
  const int ix = static_cast<int>(gx), iz = static_cast<int>(gz);
  const float fx = gx - ix, fz = gz - iz;
  const float h00 = CornerHeight(ix, iz), h10 = CornerHeight(ix + 1, iz);
  const float h01 = CornerHeight(ix, iz + 1), h11 = CornerHeight(ix + 1, iz + 1);
  const float near = h00 + (h10 - h00) * fx;
  const float far = h01 + (h11 - h01) * fx;
  return near + (far - near) * fz;
}

float City::RoadCornerHeight(int cx, int cy) const
{
  for (int dy = -1; dy <= 0; ++dy)
    for (int dx = -1; dx <= 0; ++dx)
      if (IsBridge(cx + dx, cy + dy))
        return kBridgeDeck;
  return CornerHeight(cx, cy);
}

float City::RoadHeightAt(float wx, float wz) const
{
  const float gx = std::clamp(wx / kCell, 0.0f, static_cast<float>(N) - 0.001f);
  const float gz = std::clamp(wz / kCell, 0.0f, static_cast<float>(N) - 0.001f);
  const int ix = static_cast<int>(gx), iz = static_cast<int>(gz);
  const float fx = gx - ix, fz = gz - iz;
  const float h00 = RoadCornerHeight(ix, iz), h10 = RoadCornerHeight(ix + 1, iz);
  const float h01 = RoadCornerHeight(ix, iz + 1), h11 = RoadCornerHeight(ix + 1, iz + 1);
  const float near = h00 + (h10 - h00) * fx;
  const float far = h01 + (h11 - h01) * fx;
  return near + (far - near) * fz + 0.15f;
}

float City::TileTop(int x, int y) const
{
  return std::max(std::max(CornerHeight(x, y), CornerHeight(x + 1, y)),
    std::max(CornerHeight(x, y + 1), CornerHeight(x + 1, y + 1)));
}

bool City::IsWater(int x, int y) const
{
  if (!InBounds(x, y))
    return false;
  return std::min(std::min(CornerHeight(x, y), CornerHeight(x + 1, y)),
           std::min(CornerHeight(x, y + 1), CornerHeight(x + 1, y + 1)))
    < kWaterLevel;
}

bool City::HasRoadAccess(int x, int y) const
{
  return IsRoad(x - 1, y) || IsRoad(x + 1, y) || IsRoad(x, y - 1) || IsRoad(x, y + 1);
}

int City::TilePopulation(const Tile& t)
{
  return t.type == Res ? kResPop[t.level] : 0;
}

int City::TileJobs(const Tile& t)
{
  if (t.type == Service)
    return 8;
  return t.type == Com ? kComJobs[t.level] : (t.type == Ind ? kIndJobs[t.level] : 0);
}

int City::TilePowerDemand(const Tile& t)
{
  if (t.level == 0)
    return 0;
  return t.type == Res ? 8 * t.level : t.type == Com ? 12 * t.level : t.type == Ind ? 25 * t.level : 0;
}

int City::TileWaterDemand(const Tile& t)
{
  if (t.level == 0)
    return 0;
  return t.type == Res ? 6 * t.level : t.type == Com ? 8 * t.level : t.type == Ind ? 14 * t.level : 0;
}

float City::Hour() const
{
  return static_cast<float>((day - std::floor(day)) * 24.0);
}

int City::CountNear(int x, int y, int radius, Type type, bool builtOnly) const
{
  int n = 0;
  for (int dy = -radius; dy <= radius; ++dy)
    for (int dx = -radius; dx <= radius; ++dx)
    {
      if ((dx == 0 && dy == 0) || !InBounds(x + dx, y + dy))
        continue;
      const Tile& t = At(x + dx, y + dy);
      if (t.type == type && (!builtOnly || t.level > 0))
        ++n;
    }
  return n;
}

float City::Desirability(int x, int y, Type zone) const
{
  if (!InBounds(x, y) || IsWater(x, y))
    return 0.0f;
  if (!HasRoadAccess(x, y))
    return 0.0f;
  const Tile& tile = At(x, y);
  const uint8_t cov = tile.coverage;
  float d = 0.45f;
  const bool waterView = IsWater(x - 1, y) || IsWater(x + 1, y) || IsWater(x, y - 1) || IsWater(x, y + 1);
  const int industryNear = CountNear(x, y, 3, Ind, true);
  switch (zone)
  {
    case Res:
      d += 0.12f * std::min(3, CountNear(x, y, 3, Com, true)) / 3.0f;
      d += waterView ? 0.15f : 0.0f;
      d -= (HasPolicy(PolicyRecycling) ? 0.07f : 0.12f) * std::min(4, industryNear);
      d += 0.08f * std::min(4, CountNear(x, y, 2, Res, true)) / 4.0f; // neighbours = a street
      d += (cov & CovHealth) ? 0.1f : 0.0f;
      d += (cov & CovEdu) ? 0.1f : 0.0f;
      d += (cov & CovPolice) ? 0.08f : 0.0f;
      d += (cov & CovPark) ? 0.12f : 0.0f;
      d += (cov & CovTransit) ? (HasPolicy(PolicyFreeTransit) ? 0.10f : 0.05f) : 0.0f;
      break;
    case Com:
      d += 0.35f * std::min(6, CountNear(x, y, 4, Res, true)) / 6.0f; // customers
      d -= 0.05f * std::min(3, industryNear);
      d += (cov & CovTransit) ? (HasPolicy(PolicyFreeTransit) ? 0.22f : 0.12f) : 0.0f;
      d += (cov & CovPolice) ? 0.06f : 0.0f;
      d += (cov & CovPark) ? 0.05f : 0.0f;
      break;
    case Ind:
      d += 0.25f * std::min(4, CountNear(x, y, 3, Ind, true)) / 4.0f; // clusters
      d += 0.15f * std::min(6, CountNear(x, y, 6, Res, true)) / 6.0f; // workers
      d += (cov & CovTransit) ? 0.06f : 0.0f;
      d += (cov & CovPolice) ? 0.05f : 0.0f;
      break;
    default:
      return 0.0f;
  }
  return Clamp01(d);
}

void City::MinimapString(std::string& out) const
{
  out.resize(static_cast<size_t>(N) * N);
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x)
    {
      const Tile& t = At(x, y);
      char c = IsWater(x, y) ? '~' : '.';
      switch (t.type)
      {
        case Road: c = (t.road || IsWater(x, y)) ? '=' : '#'; break;
        case Res: c = t.level ? 'R' : 'r'; break;
        case Com: c = t.level ? 'C' : 'c'; break;
        case Ind: c = t.level ? 'I' : 'i'; break;
        case Service: c = 'S'; break;
        default: break;
      }
      out[static_cast<size_t>(y) * N + x] = c;
    }
}

// ============================================================================
// Tools
// ============================================================================

void City::RoadPath(int x0, int y0, int x1, int y1, std::vector<std::pair<int, int>>& out)
{
  out.clear();
  const int sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
  for (int x = x0;; x += sx)
  {
    out.emplace_back(x, y0);
    if (x == x1)
      break;
  }
  for (int y = y0 + sy; (sy > 0) ? y <= y1 : y >= y1; y += sy)
    out.emplace_back(x1, y);
}

void City::SplashTile(int x, int y, float strength)
{
  bool nearWater = false;
  for (int dy = -1; dy <= 1 && !nearWater; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
      if (IsWater(x + dx, y + dy))
      {
        nearWater = true;
        break;
      }
  if (nearWater)
    splashes.push_back({ (x + 0.5f) * kCell, (y + 0.5f) * kCell, kCell * 0.6f, strength });
}

int City::BuildRoad(int x0, int y0, int x1, int y1, int kind)
{
  std::vector<std::pair<int, int>> path;
  RoadPath(x0, y0, x1, y1, path);
  int placed = 0;
  bool broke = false;
  for (auto [x, y] : path)
  {
    if (!InBounds(x, y))
      continue;
    Tile& t = m_tiles[y * N + x];
    const bool water = IsWater(x, y);
    float cost = water ? kBridgeCost : (kind ? kAvenueCost : kRoadCost);
    if (t.type == Road)
    {
      if (t.road == kind || water)
        continue; // already there
      cost = kind ? kAvenueCost - kRoadCost : 0.0f; // upgrade / downgrade
    }
    if (t.type == Service)
      continue; // never pave over a service building
    if (money < cost)
    {
      broke = true;
      continue;
    }
    if (t.type != Road)
    {
      t = Tile {};
      t.type = Road;
      ++roadTiles;
    }
    t.road = static_cast<uint8_t>(kind);
    money -= cost;
    ++placed;
    if (water)
      SplashTile(x, y, 1.0f);
  }
  if (broke && m_brokeCooldown <= 0.0f)
  {
    events.push_back("Not enough money for that road.");
    m_brokeCooldown = 3.0f;
  }
  if (placed)
  {
    geometryDirty = true;
    RefreshServices();
  }
  return placed;
}

int City::ZoneRect(int x0, int y0, int x1, int y1, Type zone)
{
  int changed = 0;
  for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
    {
      if (!InBounds(x, y) || IsWater(x, y))
        continue;
      Tile& t = m_tiles[y * N + x];
      if (t.type == Road || t.type == Service || t.type == zone)
        continue;
      t = Tile {};
      t.type = zone;
      t.variant = static_cast<uint8_t>(Rand() & 0xFF);
      ++changed;
    }
  if (changed)
  {
    geometryDirty = true;
    RefreshServices();
  }
  return changed;
}

int City::BulldozeRect(int x0, int y0, int x1, int y1)
{
  int changed = 0;
  for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
    {
      if (!InBounds(x, y))
        continue;
      Tile& t = m_tiles[y * N + x];
      if ((t.type == Empty && !t.planted) || money < kBulldozeCost)
        continue;
      if (t.type == Road)
      {
        --roadTiles;
        money += kRoadRefund;
        vehicles.erase(std::remove_if(vehicles.begin(), vehicles.end(),
                         [x, y](const Vehicle& v) {
                           return (v.x == x && v.y == y) || (v.nx == x && v.ny == y);
                         }),
          vehicles.end());
      }
      else if (t.type == Service)
        money += kServices[t.service].cost * 0.25f;
      t = Tile {};
      money -= kBulldozeCost;
      ++changed;
      SplashTile(x, y, 0.8f);
    }
  if (changed)
  {
    geometryDirty = true;
    RefreshServices();
  }
  return changed;
}

bool City::PlaceService(int x, int y, ServiceKind kind)
{
  if (!InBounds(x, y) || kind >= ServiceCount)
    return false;
  const ServiceInfo& info = kServices[kind];
  if (milestone < info.unlockMilestone)
  {
    events.push_back(std::string(info.name) + " unlocks at " + kMilestones[info.unlockMilestone - 1].name + ".");
    return false;
  }
  if (IsWater(x, y))
  {
    events.push_back("Cannot build on water. Raise the land first.");
    return false;
  }
  Tile& t = m_tiles[y * N + x];
  if (t.type == Road || t.type == Service || (t.type != Empty && t.level > 0))
  {
    events.push_back("That tile is occupied. Bulldoze it first.");
    return false;
  }
  if (money < info.cost)
  {
    if (m_brokeCooldown <= 0.0f)
    {
      events.push_back(std::string("Not enough money for a ") + info.name + ".");
      m_brokeCooldown = 3.0f;
    }
    return false;
  }
  t = Tile {};
  t.type = Service;
  t.service = static_cast<uint8_t>(kind);
  t.level = 1;
  t.variant = static_cast<uint8_t>(Rand() & 0xFF);
  t.spawnTime = gameSeconds;
  animatingUntil = gameSeconds + kSpawnAnimSeconds;
  money -= info.cost;
  geometryDirty = buildingsDirty = true;
  if (!HasRoadAccess(x, y))
    events.push_back(std::string(info.name) + " needs a road next to it to serve anyone.");
  SplashTile(x, y, 0.6f);
  RefreshServices();
  return true;
}

int City::ModifyTerrain(int x, int y, int radius, float delta)
{
  if (!InBounds(x, y))
    return 0;
  const float cxc = x + 0.5f, cyc = y + 0.5f;
  // Cost first: all or nothing.
  float total = 0.0f;
  std::vector<std::pair<int, float>> moves;
  for (int cy = std::max(0, y - radius); cy <= std::min(N, y + radius + 1); ++cy)
    for (int cx = std::max(0, x - radius); cx <= std::min(N, x + radius + 1); ++cx)
    {
      const float dx = cx - cxc, dy = cy - cyc;
      const float d = std::sqrt(dx * dx + dy * dy) / std::max(0.5f, radius + 0.5f);
      if (d >= 1.0f)
        continue;
      const float w = 1.0f - d * d * (3.0f - 2.0f * d);
      const float h = m_height[cy * (N + 1) + cx];
      const float target = std::clamp(h + delta * w, -6.0f, 40.0f);
      if (std::fabs(target - h) < 0.01f)
        continue;
      moves.emplace_back(cy * (N + 1) + cx, target);
      total += std::fabs(target - h) * kTerrainCost;
    }
  if (moves.empty())
    return 0;
  if (money < total)
  {
    if (m_brokeCooldown <= 0.0f)
    {
      events.push_back("Not enough money to move that much earth.");
      m_brokeCooldown = 3.0f;
    }
    return 0;
  }
  for (auto& [idx, target] : moves)
    m_height[idx] = target;
  money -= total;
  terrainDirty = geometryDirty = buildingsDirty = true;
  splashes.push_back({ (x + 0.5f) * kCell, (y + 0.5f) * kCell, (radius + 0.5f) * kCell, delta > 0.0f ? 1.2f : 0.9f });
  RefreshServices();
  return static_cast<int>(moves.size());
}

int City::FlattenRect(int x0, int y0, int x1, int y1)
{
  const int ax = std::max(0, std::min(x0, x1)), bx = std::min(N - 1, std::max(x0, x1));
  const int ay = std::max(0, std::min(y0, y1)), by = std::min(N - 1, std::max(y0, y1));
  float sum = 0.0f;
  int n = 0;
  for (int cy = ay; cy <= by + 1; ++cy)
    for (int cx = ax; cx <= bx + 1; ++cx)
    {
      sum += m_height[cy * (N + 1) + cx];
      ++n;
    }
  if (n == 0)
    return 0;
  const float mean = sum / n;
  float total = 0.0f;
  for (int cy = ay; cy <= by + 1; ++cy)
    for (int cx = ax; cx <= bx + 1; ++cx)
      total += std::fabs(m_height[cy * (N + 1) + cx] - mean) * kTerrainCost;
  if (total < 0.5f)
    return 0;
  if (money < total)
  {
    if (m_brokeCooldown <= 0.0f)
    {
      events.push_back("Not enough money to level that ground.");
      m_brokeCooldown = 3.0f;
    }
    return 0;
  }
  int moved = 0;
  for (int cy = ay; cy <= by + 1; ++cy)
    for (int cx = ax; cx <= bx + 1; ++cx)
    {
      float& h = m_height[cy * (N + 1) + cx];
      if (std::fabs(h - mean) > 0.005f)
        ++moved;
      h = mean;
    }
  money -= total;
  terrainDirty = geometryDirty = buildingsDirty = true;
  splashes.push_back({ (ax + bx + 1) * 0.5f * kCell, (ay + by + 1) * 0.5f * kCell,
    (std::max(bx - ax, by - ay) + 1) * kCell * 0.6f, 0.8f });
  RefreshServices();
  return moved;
}

int City::PlantTrees(int x0, int y0, int x1, int y1)
{
  int planted = 0;
  for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
    {
      if (!InBounds(x, y) || IsWater(x, y))
        continue;
      Tile& t = m_tiles[y * N + x];
      if (t.type != Empty || t.planted || money < kTreeCost)
        continue;
      t.planted = 1;
      t.variant = static_cast<uint8_t>(Rand() & 0xFF);
      money -= kTreeCost;
      ++planted;
    }
  if (planted)
    terrainDirty = geometryDirty = true; // the tree layer is rebuilt with the terrain
  return planted;
}

void City::SetTaxRate(int percent)
{
  taxRate = std::clamp(percent, 5, 20);
}

void City::BeginAction()
{
  Snapshot s;
  s.tiles = m_tiles;
  s.height = m_height;
  s.money = money;
  s.roadTiles = roadTiles;
  m_undo.push_back(std::move(s));
  if (m_undo.size() > 24)
    m_undo.erase(m_undo.begin());
}

bool City::Undo()
{
  if (m_undo.empty())
    return false;
  Snapshot& s = m_undo.back();
  const bool heightsChanged = s.height != m_height;
  m_tiles = std::move(s.tiles);
  m_height = std::move(s.height);
  money = s.money;
  roadTiles = s.roadTiles;
  m_undo.pop_back();
  // Keep the cars whose road survived the rollback.
  vehicles.erase(std::remove_if(vehicles.begin(), vehicles.end(),
                   [this](const Vehicle& v) { return !IsRoad(v.x, v.y) || !IsRoad(v.nx, v.ny); }),
    vehicles.end());
  geometryDirty = buildingsDirty = true;
  terrainDirty = terrainDirty || heightsChanged;
  RefreshServices();
  return true;
}

// ============================================================================
// Services
// ============================================================================

void City::CoverFrom(int sx, int sy, int radius, uint8_t bit)
{
  // Breadth-first along roads from the building's kerb; every tile touching a
  // reached road is served. Stamps avoid clearing a visited array per call.
  static std::vector<uint32_t> s_stamp;
  static uint32_t s_gen = 0;
  if (s_stamp.size() != static_cast<size_t>(N) * N)
    s_stamp.assign(static_cast<size_t>(N) * N, 0);
  ++s_gen;
  std::deque<std::pair<int, int>> open;
  const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
  for (auto& d : dirs)
  {
    const int x = sx + d[0], y = sy + d[1];
    if (IsRoad(x, y) && s_stamp[y * N + x] != s_gen)
    {
      s_stamp[y * N + x] = s_gen;
      open.emplace_back(x, y * 1000 + 1); // pack distance in the second field
    }
  }
  m_tiles[sy * N + sx].coverage |= bit;
  while (!open.empty())
  {
    const int x = open.front().first, y = open.front().second / 1000, dist = open.front().second % 1000;
    open.pop_front();
    m_tiles[y * N + x].coverage |= bit;
    for (auto& d : dirs)
    {
      const int nx = x + d[0], ny = y + d[1];
      if (!InBounds(nx, ny))
        continue;
      Tile& n = m_tiles[ny * N + nx];
      if (n.type != Road)
      {
        n.coverage |= bit;
        continue;
      }
      if (dist < radius && s_stamp[ny * N + nx] != s_gen)
      {
        s_stamp[ny * N + nx] = s_gen;
        open.emplace_back(nx, ny * 1000 + dist + 1);
      }
    }
  }
}

void City::RefreshServices()
{
  std::vector<uint8_t> before(m_tiles.size());
  for (size_t i = 0; i < m_tiles.size(); ++i)
  {
    before[i] = m_tiles[i].coverage;
    m_tiles[i].coverage = 0;
    m_tiles[i].powered = m_tiles[i].watered = false;
  }
  for (ServiceStats& s : m_serviceStats)
    s = ServiceStats {};

  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x)
    {
      const Tile& t = At(x, y);
      if (t.type != Service)
        continue;
      ServiceStats& st = m_serviceStats[t.service];
      ++st.buildings;
      if (!HasRoadAccess(x, y))
        continue; // built, but nobody can reach it
      st.capacity += kServices[t.service].capacity;
      CoverFrom(x, y, kServices[t.service].radius, static_cast<uint8_t>(1u << t.service));
    }

  // Power and water are pools: covered buildings draw until the plants run dry.
  int powerLeft = m_serviceStats[PowerPlant].capacity, waterLeft = m_serviceStats[WaterTower].capacity;
  unpowered = unwatered = 0;
  for (Tile& t : m_tiles)
  {
    if (t.type < Res || t.type > Ind)
      continue;
    const int pd = TilePowerDemand(t), wd = TileWaterDemand(t);
    if (t.coverage & CovPower)
    {
      ++m_serviceStats[PowerPlant].covered;
      m_serviceStats[PowerPlant].demand += pd;
      if (pd <= powerLeft)
      {
        powerLeft -= pd;
        t.powered = true;
      }
    }
    if (t.coverage & CovWater)
    {
      ++m_serviceStats[WaterTower].covered;
      m_serviceStats[WaterTower].demand += wd;
      if (wd <= waterLeft)
      {
        waterLeft -= wd;
        t.watered = true;
      }
    }
    if (t.level > 0)
    {
      if (!t.powered) ++unpowered;
      if (!t.watered) ++unwatered;
      const int people = TilePopulation(t) + TileJobs(t);
      for (int k = Clinic; k < ServiceCount; ++k)
        if (t.coverage & (1u << k))
        {
          ++m_serviceStats[k].covered;
          m_serviceStats[k].demand += people;
        }
    }
  }
  for (size_t i = 0; i < m_tiles.size(); ++i)
    if (before[i] != m_tiles[i].coverage)
    {
      ++coverageVersion;
      break;
    }
}

// ============================================================================
// Simulation
// ============================================================================

void City::Tick(float realSeconds)
{
  if (m_brokeCooldown > 0.0f)
    m_brokeCooldown -= realSeconds;
  if (speed <= 0)
    return;
  const float dt = realSeconds * static_cast<float>(speed);
  day += dt / kDaySeconds;
  gameSeconds += dt;

  m_growthAccum += dt;
  while (m_growthAccum >= 1.0f)
  {
    m_growthAccum -= 1.0f;
    GrowthStep();
  }
  m_dayAccum += dt;
  while (m_dayAccum >= kDaySeconds)
  {
    m_dayAccum -= kDaySeconds;
    EconomyStep();
  }
  UpdateVehicles(dt);
  UpdateBoats(dt);
}

void City::GrowthStep()
{
  RefreshServices();
  // Taxes above 10% cool demand; below 10% heat it.
  const float taxFactor = Clamp01(1.25f - taxRate * 0.025f);
  int pop = 0, jobsC = 0, jobsI = 0, jobsS = 0, built = 0, indTiles = 0;
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x)
    {
      Tile& t = m_tiles[y * N + x];
      if (t.type == Service)
      {
        jobsS += TileJobs(t);
        continue;
      }
      if (t.type != Res && t.type != Com && t.type != Ind)
        continue;
      if (t.level > 0)
        ++built;
      if (t.type == Ind && t.level > 0)
        ++indTiles;

      t.desirability = Desirability(x, y, t.type);
      if (t.level < kMaxLevel && t.desirability > 0.0f)
      {
        const float demand = (t.type == Res ? demandR : (t.type == Com ? demandC : demandI)) * taxFactor;
        // Level-ups need progressively better land; the first building only
        // needs a road. Growing past level 1 needs power and water, and the
        // top level needs the matching service (school / bus line / power).
        static const float kNeed[4] = { 0.0f, 0.35f, 0.6f, 1.0f };
        bool serviced = true;
        if (t.level >= 1)
          serviced = t.powered && t.watered;
        if (t.level == 2)
        {
          if (t.type == Res) serviced = serviced && (t.coverage & CovEdu);
          if (t.type == Com) serviced = serviced && (t.coverage & CovTransit);
        }
        if (serviced && t.desirability >= kNeed[t.level])
        {
          float rate = (t.level == 0 ? 0.22f : 0.07f) * demand * (0.4f + t.desirability)
            * (0.6f + Rand01() * 0.8f);
          if (HasPolicy(PolicyEducationGrant) && (t.coverage & CovEdu))
            rate *= 1.35f;
          if (HasPolicy(PolicyShopHoliday) && t.type == Com)
            rate *= 1.30f;
          t.growth += rate;
          if (t.growth >= 1.0f)
          {
            t.growth = 0.0f;
            ++t.level;
            t.variant = static_cast<uint8_t>(Rand() & 0xFF);
            t.spawnTime = gameSeconds;
            animatingUntil = gameSeconds + kSpawnAnimSeconds;
            buildingsDirty = true; // NOT geometryDirty: that regenerated every tree on the map per spawn
          }
        }
      }
      if (t.type == Res)
        pop += kResPop[t.level];
      else if (t.type == Com)
        jobsC += kComJobs[t.level];
      else
        jobsI += kIndJobs[t.level];
    }
  population = pop;
  jobs = jobsC + jobsI + jobsS;
  buildings = built;

  // RCI: residents want jobs, shops want customers, industry wants workers.
  demandR = std::clamp(0.35f + (jobs + 40 - population) / 120.0f, 0.05f, 1.0f);
  demandC = std::clamp(0.2f + (population * 0.35f + 10.0f - jobsC) / 60.0f, 0.05f, 1.0f);
  demandI = std::clamp(0.2f + (population * 0.5f + 10.0f - jobsI) / 80.0f, 0.05f, 1.0f);

  // Happiness: employment balance, taxes, industrial share of the skyline,
  // services reaching people, blackouts and dry taps.
  const float employment = std::clamp(static_cast<float>(jobs - population) / (population + 40.0f), -1.0f, 1.0f);
  const float indShare = built > 0 ? static_cast<float>(indTiles) / built : 0.0f;
  const float denom = std::max(1, built);
  auto share = [&](ServiceKind k) {
    const ServiceStats& s = m_serviceStats[k];
    const float cap = s.demand > 0 ? std::min(1.0f, static_cast<float>(s.capacity) / s.demand) : 1.0f;
    return (s.covered / denom) * cap;
  };
  float target = 62.0f + employment * 20.0f - (taxRate - 10) * 2.4f - indShare * 18.0f;
  target += 6.0f * share(Clinic) + 5.0f * share(School) + 5.0f * share(PoliceStation) + 6.0f * share(Park)
    + 3.0f * share(BusDepot);
  if (HasPolicy(PolicyRecycling)) target += 3.0f;
  if (HasPolicy(PolicyNightLighting)) target += 4.0f;
  if (HasPolicy(PolicyFreeTransit)) target += 3.0f;
  target -= 25.0f * (unpowered / denom) + 18.0f * (unwatered / denom);
  happiness += (std::clamp(target, 5.0f, 100.0f) - happiness) * 0.08f;

  // Milestones.
  const int count = static_cast<int>(sizeof(kMilestones) / sizeof(kMilestones[0]));
  while (milestone < count && population >= kMilestones[milestone].population)
  {
    events.push_back(std::string("Milestone: ") + kMilestones[milestone].name + " (population "
      + std::to_string(kMilestones[milestone].population) + ")");
    ++milestone;
  }
}

void City::EconomyStep()
{
  const float rate = taxRate / 10.0f;
  budget.resTax = population * 1.2f * rate;
  int jobsC = 0, jobsI = 0;
  float upkeep = 0.0f;
  for (const Tile& t : m_tiles)
  {
    if (t.type == Com) jobsC += kComJobs[t.level];
    else if (t.type == Ind) jobsI += kIndJobs[t.level];
    else if (t.type == Service) upkeep += kServices[t.service].upkeep;
  }
  budget.comTax = jobsC * 0.9f * rate;
  budget.indTax = jobsI * 0.7f * rate;
  if (HasPolicy(PolicyShopHoliday))
    budget.comTax *= 0.5f;
  budget.roadUpkeep = roadTiles * 0.35f;
  budget.serviceUpkeep = upkeep;
  budget.policyCost = PolicyCost();
  lastDayIncome = budget.resTax + budget.comTax + budget.indTax
    - budget.roadUpkeep - budget.serviceUpkeep - budget.policyCost;
  money += lastDayIncome;

  // One statistics sample per day for the charts.
  HistorySample sample;
  sample.population = static_cast<float>(population);
  sample.money = money;
  sample.happiness = happiness;
  sample.income = lastDayIncome;
  sample.jobs = static_cast<float>(jobs);
  history[historyHead] = sample;
  historyHead = (historyHead + 1) % kHistoryLength;
  if (historyCount < kHistoryLength)
    ++historyCount;
}

// ============================================================================
// Save / load
// ============================================================================

namespace
{
  constexpr uint32_t kSaveMagic = 0x53595443u; // "CTYS"
  constexpr uint32_t kSaveVersion = 1;
} // namespace

bool City::Save(const std::string& path) const
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  auto put = [&out](const void* data, size_t bytes) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  };
  const uint32_t header[4] = { kSaveMagic, kSaveVersion, static_cast<uint32_t>(N), 0u };
  put(header, sizeof(header));
  put(m_tiles.data(), m_tiles.size() * sizeof(Tile));
  put(m_height.data(), m_height.size() * sizeof(float));
  const double dayCopy = day;
  put(&dayCopy, sizeof(dayCopy));
  const float floats[4] = { money, happiness, demandR, gameSeconds };
  put(floats, sizeof(floats));
  const int ints[5] = { taxRate, milestone, roadTiles, speed, historyCount };
  put(ints, sizeof(ints));
  put(&policies, sizeof(policies));
  put(&historyHead, sizeof(historyHead));
  put(history, sizeof(history));
  return out.good();
}

bool City::Load(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  auto get = [&in](void* data, size_t bytes) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
  };
  uint32_t header[4] = {};
  get(header, sizeof(header));
  if (!in.good() || header[0] != kSaveMagic || header[1] != kSaveVersion
    || header[2] != static_cast<uint32_t>(N))
  {
    events.push_back("That save was written by a different version of CitySlice.");
    return false;
  }
  std::vector<Tile> tiles(static_cast<size_t>(N) * N);
  std::vector<float> heights(static_cast<size_t>(N + 1) * (N + 1));
  get(tiles.data(), tiles.size() * sizeof(Tile));
  get(heights.data(), heights.size() * sizeof(float));
  double dayCopy = 0.0;
  get(&dayCopy, sizeof(dayCopy));
  float floats[4] = {};
  get(floats, sizeof(floats));
  int ints[5] = {};
  get(ints, sizeof(ints));
  uint32_t pol = 0;
  int head = 0;
  get(&pol, sizeof(pol));
  get(&head, sizeof(head));
  HistorySample hist[kHistoryLength] = {};
  get(hist, sizeof(hist));
  if (in.fail())
  {
    events.push_back("That save file is truncated.");
    return false;
  }

  m_tiles = std::move(tiles);
  m_height = std::move(heights);
  day = dayCopy;
  money = floats[0];
  happiness = floats[1];
  demandR = floats[2];
  gameSeconds = floats[3];
  taxRate = ints[0];
  milestone = ints[1];
  roadTiles = ints[2];
  speed = std::clamp(ints[3], 0, 3);
  historyCount = std::clamp(ints[4], 0, kHistoryLength);
  policies = pol;
  historyHead = (head % kHistoryLength + kHistoryLength) % kHistoryLength;
  std::memcpy(history, hist, sizeof(history));

  vehicles.clear();
  boats.clear();
  m_undo.clear();
  animatingUntil = 0.0f;
  geometryDirty = buildingsDirty = terrainDirty = true;
  ++coverageVersion;
  RefreshServices();
  // The derived counters (population, jobs, desirability, budget) are not in
  // the file: one growth step recomputes them all. It also advances growth by
  // one tick, i.e. loading lands 0.35 game-seconds later than it saved. Store
  // the counters in the save if that ever needs to be exact.
  GrowthStep();
  events.push_back("City loaded.");
  return true;
}

bool City::NextRoadNeighbour(int x, int y, int fromX, int fromY, int& nx, int& ny)
{
  const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
  int candidates[4][2];
  int count = 0;
  // Prefer going straight on, then any turn, and only reverse at a dead end.
  const int sx = x + (x - fromX), sy = y + (y - fromY);
  if ((x != fromX || y != fromY) && IsRoad(sx, sy) && Rand01() < 0.7f)
  {
    nx = sx;
    ny = sy;
    return true;
  }
  for (auto& d : dirs)
  {
    const int cx = x + d[0], cy = y + d[1];
    if (IsRoad(cx, cy) && !(cx == fromX && cy == fromY))
    {
      candidates[count][0] = cx;
      candidates[count][1] = cy;
      ++count;
    }
  }
  if (count == 0)
  {
    if (!IsRoad(fromX, fromY) || (fromX == x && fromY == y))
      return false;
    nx = fromX;
    ny = fromY;
    return true;
  }
  const int pick = static_cast<int>(Rand() % static_cast<uint32_t>(count));
  nx = candidates[pick][0];
  ny = candidates[pick][1];
  return true;
}

void City::UpdateVehicles(float gameDt)
{
  // Traffic scales with the city, not just the asphalt.
  const size_t target = static_cast<size_t>(std::min(140, roadTiles / 4 + buildings / 3 + 1));
  m_spawnAccum += gameDt;
  if (vehicles.size() < target && m_spawnAccum > 0.35f && roadTiles > 1)
  {
    m_spawnAccum = 0.0f;
    for (int tries = 0; tries < 32; ++tries)
    {
      const int x = static_cast<int>(Rand() % N), y = static_cast<int>(Rand() % N);
      if (!IsRoad(x, y))
        continue;
      Vehicle v { x, y, x, y, 0.0f, kCarColors[Rand() % 7] };
      if (NextRoadNeighbour(x, y, x, y, v.nx, v.ny))
      {
        vehicles.push_back(v);
        break;
      }
    }
  }

  for (size_t i = 0; i < vehicles.size();)
  {
    Vehicle& v = vehicles[i];
    v.t += gameDt * (At(v.x, v.y).road ? 2.4f : 1.8f);
    if (v.t >= 1.0f)
    {
      v.t -= 1.0f;
      const int fx = v.x, fy = v.y;
      v.x = v.nx;
      v.y = v.ny;
      if (!IsRoad(v.x, v.y) || !NextRoadNeighbour(v.x, v.y, fx, fy, v.nx, v.ny))
      {
        vehicles[i] = vehicles.back();
        vehicles.pop_back();
        continue;
      }
    }
    ++i;
  }
}

void City::UpdateBoats(float gameDt)
{
  const float extent = N * kCell;
  auto deep = [&](float wx, float wz) {
    if (wx < kCell || wz < kCell || wx > extent - kCell || wz > extent - kCell)
      return false;
    const int tx = static_cast<int>(wx / kCell), tz = static_cast<int>(wz / kCell);
    if (!IsWater(tx, tz) || IsBridge(tx, tz))
      return false;
    return HeightAt(wx, wz) < kWaterLevel - 0.7f;
  };
  for (Boat& b : boats)
  {
    // Look ahead; steer away from shallows, otherwise wander gently.
    const float dt = std::min(gameDt, 0.1f);
    const float ax = b.x + std::cos(b.heading) * 14.0f, az = b.z + std::sin(b.heading) * 14.0f;
    if (!deep(ax, az))
    {
      const float lx = b.x + std::cos(b.heading + 0.9f) * 12.0f, lz = b.z + std::sin(b.heading + 0.9f) * 12.0f;
      b.heading += deep(lx, lz) ? 1.4f * dt : -1.4f * dt;
    }
    else
      b.heading += (Rand01() - 0.5f) * 0.5f * dt;
    const float nx = b.x + std::cos(b.heading) * b.speed * dt;
    const float nz = b.z + std::sin(b.heading) * b.speed * dt;
    if (deep(nx, nz))
    {
      b.x = nx;
      b.z = nz;
    }
    else
      b.heading += 3.14159f * 0.5f * dt + 0.5f;
  }
}
