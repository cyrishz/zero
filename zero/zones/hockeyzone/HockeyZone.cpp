#include <string.h>
#include <zero/BotController.h>
#include <zero/ZeroBot.h>
#include <zero/game/GameEvent.h>
#include <zero/game/Logger.h>
#include <zero/zones/ZoneController.h>
#include <zero/zones/hockeyzone/HockeyBehavior.h>

namespace zero {
namespace hockeyzone {

struct HockeyZoneController : ZoneController {
  bool IsZone(Zone zone) override { return zone == Zone::Subgame; }

  void CreateBehaviors(const char* arena_name) override;
};

static HockeyZoneController controller;

void HockeyZoneController::CreateBehaviors(const char* arena_name) {
  Log(LogLevel::Info, "Registering HockeyZone behaviors for arena: %s", arena_name);

  bot->bot_controller->energy_tracker.estimate_type = EnergyHeuristicType::Average;

  // Enable the actuator so the bot can control input
  bot->bot_controller->actuator.enabled = true;
  Log(LogLevel::Info, "Actuator enabled.");

  auto& repo = bot->bot_controller->behaviors;

  repo.Add("hockey", std::make_unique<HockeyBehavior>());
  
  SetBehavior("hockey");
}

}  // namespace hockeyzone
}  // namespace zero
