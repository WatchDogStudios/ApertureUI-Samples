/*
 * SelfTest.cpp - the one runnable check for the simulation (cityslice --selftest).
 * Depends only on City.h, so it also builds standalone without the engine.
 */
#include "SelfTest.h"

#include "City.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// --------------------------------------------------------------------------
// Self-test: the one runnable check for the non-trivial simulation logic.
// --------------------------------------------------------------------------
int RunCitySelfTest()
{
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
  };

  City city;
  city.Reset(7u);
  const float startMoney = city.money;

  // A road across the flat centre must place tiles and charge for them.
  const int placed = city.BuildRoad(30, 48, 60, 48);
  check(placed > 0 && placed <= 31, "road placement count");
  float expected = 0.0f;
  for (int x = 30; x <= 60; ++x)
    if (city.IsRoad(x, 48))
      expected += city.IsWater(x, 48) ? City::kBridgeCost : City::kRoadCost; // water tiles became bridges
  check(city.money == startMoney - expected, "road charges money (bridges at the bridge price)");
  check(city.roadTiles == placed, "road tile counter");
  check(city.BuildRoad(30, 48, 60, 48) == 0, "re-laying a road is free and a no-op");

  // Zone both sides; tiles without road access must never grow.
  city.ZoneRect(30, 47, 60, 47, City::Res);
  city.ZoneRect(30, 49, 60, 49, City::Com);
  city.ZoneRect(30, 55, 60, 55, City::Ind); // far away: no access
  for (int i = 0; i < 600; ++i)
    city.Tick(0.1f); // 60 game-seconds at speed 1
  check(city.population > 0, "residential grows next to a road");
  check(city.jobs > 0, "commercial grows next to a road");
  bool farGrew = false;
  for (int x = 30; x <= 60; ++x)
    farGrew |= city.At(x, 55).level > 0;
  check(!farGrew, "zones without road access stay vacant");
  check(!city.vehicles.empty(), "traffic spawns on the road network");
  for (const City::Vehicle& v : city.vehicles)
    if (!city.IsRoad(v.x, v.y) || !city.IsRoad(v.nx, v.ny))
    {
      check(false, "vehicle left the road");
      break;
    }

  // Bulldozing a road removes the cars on it and the counter follows.
  const int before = city.roadTiles;
  city.BulldozeRect(40, 48, 40, 48);
  check(city.roadTiles == before - 1, "bulldoze decrements road tiles");
  for (const City::Vehicle& v : city.vehicles)
    check(!(v.x == 40 && v.y == 48) && !(v.nx == 40 && v.ny == 48), "cars evicted from bulldozed tile");

  // Paused city does not advance.
  city.speed = 0;
  const double day = city.day;
  city.Tick(5.0f);
  check(city.day == day, "speed 0 pauses time");

  // L-path shape: horizontal leg first, then vertical.
  std::vector<std::pair<int, int>> path;
  City::RoadPath(2, 2, 5, 4, path);
  check(path.size() == 6 && path.front() == std::make_pair(2, 2) && path.back() == std::make_pair(5, 4),
    "L-shaped road path");

  // Services: a power plant and a water tower next to the road cover the lots.
  city.speed = 1;
  city.money = 50000.0f;
  city.BuildRoad(30, 48, 60, 48); // relay the bulldozed tile
  check(city.PlaceService(31, 46, City::PowerPlant), "power plant placed");
  check(city.PlaceService(33, 46, City::WaterTower), "water tower placed");
  city.BuildRoad(31, 47, 33, 47);
  city.RefreshServices();
  check((city.At(45, 47).coverage & City::CovPower) != 0, "power reaches lots along the road");
  check((city.At(45, 49).coverage & City::CovWater) != 0, "water reaches lots along the road");
  check(city.ServiceStatsFor(City::PowerPlant).capacity == City::kServices[City::PowerPlant].capacity, "power capacity counted");
  check(!(city.At(45, 55).coverage & City::CovPower), "coverage stops at the road network");
  check(!city.PlaceService(31, 46, City::Clinic), "cannot build on an occupied tile");

  // Bridges: a road across water becomes a bridge and costs more.
  int wx = -1, wy = -1;
  for (int y = 1; y < City::N - 1 && wx < 0; ++y)
    for (int x = 1; x < City::N - 1; ++x)
      if (city.IsWater(x, y) && !city.IsWater(x - 1, y) && city.At(x - 1, y).type == City::Empty)
      {
        wx = x;
        wy = y;
        break;
      }
  if (wx > 0)
  {
    const float m0 = city.money;
    check(city.BuildRoad(wx - 1, wy, wx, wy) == 2, "road continues onto water as a bridge");
    check(city.IsBridge(wx, wy) && !city.IsBridge(wx - 1, wy), "bridge flag on the water tile only");
    check(m0 - city.money == City::kRoadCost + City::kBridgeCost, "bridge tile charged the bridge price");
    check(city.RoadHeightAt((wx + 0.5f) * City::kCell, (wy + 0.5f) * City::kCell) > City::kWaterLevel + 2.0f,
      "bridge deck sits above the water");
    check(!city.splashes.empty(), "building over water makes a splash");
  }

  // Terrain tools and undo.
  city.BeginAction();
  const float hBefore = city.CornerHeight(70, 70);
  const float mBefore = city.money;
  check(city.ModifyTerrain(70, 70, 2, 3.0f) > 0, "raise terrain moves corners");
  check(city.CornerHeight(70, 70) > hBefore + 1.0f, "terrain got higher");
  check(city.money < mBefore, "terrain work costs money");
  check(city.Undo(), "undo pops the action");
  check(city.CornerHeight(70, 70) == hBefore && city.money == mBefore, "undo restores heights and money");
  city.BeginAction();
  city.FlattenRect(70, 70, 73, 73);
  check(city.CornerHeight(70, 70) == city.CornerHeight(73, 73), "flatten levels the corners");
  city.Undo();
  check(city.PlantTrees(80, 80, 81, 81) == 4 && city.At(80, 80).planted, "trees planted on empty land");

  // Avenue upgrade keeps the road tile and charges the difference.
  const int rt = city.roadTiles;
  city.BuildRoad(35, 48, 40, 48, 1);
  check(city.roadTiles == rt && city.At(36, 48).road == 1, "avenue upgrade in place");

  // Boats stay on water.
  for (int i = 0; i < 200; ++i)
    city.Tick(0.1f);
  bool boatsOk = true;
  for (const City::Boat& b : city.boats)
    boatsOk &= city.IsWater(static_cast<int>(b.x / City::kCell), static_cast<int>(b.z / City::kCell));
  check(boatsOk, "boats stay on water");

  // Policies flip, cost money and show up in the daily budget.
  check(!city.HasPolicy(City::PolicyRecycling), "policies start off");
  check(city.TogglePolicy(City::PolicyRecycling) && city.HasPolicy(City::PolicyRecycling), "policy toggles on");
  check(city.PolicyCost() > 0.0f, "an enabled policy costs money");
  check(!city.TogglePolicy(City::PolicyRecycling) && city.PolicyCost() == 0.0f, "policy toggles back off");
  city.TogglePolicy(City::PolicyNightLighting);

  // Save/load round-trips the whole city.
  const std::string savePath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/cityslice-selftest.city";
  const float savedMoney = city.money;
  const int savedPop = city.population, savedRoads = city.roadTiles;
  const double savedDay = city.day;
  const float savedHeight = city.CornerHeight(70, 70);
  check(city.Save(savePath), "save writes a file");
  City reloaded;
  reloaded.Reset(99u); // a different map, so nothing can pass by luck
  check(reloaded.money != savedMoney || reloaded.roadTiles != savedRoads, "fresh city differs from the save");
  check(reloaded.Load(savePath), "load reads the file back");
  check(reloaded.money == savedMoney && reloaded.roadTiles == savedRoads && reloaded.day == savedDay,
    "load restores treasury, roads and the date");
  check(reloaded.population == savedPop, "load restores population");
  check(reloaded.CornerHeight(70, 70) == savedHeight, "load restores the terrain");
  check(reloaded.HasPolicy(City::PolicyNightLighting), "load restores policies");
  check(!reloaded.Load(savePath + ".missing"), "loading a missing file fails cleanly");
  std::remove(savePath.c_str());

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
