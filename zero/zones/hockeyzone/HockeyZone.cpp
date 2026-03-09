#include <string>
#include <cstdlib>
#include <string.h>
#include <zero/BotController.h>
#include <zero/ZeroBot.h>
#include <zero/game/GameEvent.h>
#include <zero/game/Logger.h>
#include <zero/zones/ZoneController.h>
#include <zero/zones/hockeyzone/HockeyBehavior.h>
#include <zero/zones/hockeyzone/MLBehavior.h>

namespace zero {
namespace hockeyzone {

// Inherit from EventHandler<ChatEvent> to automatically register for chat events
struct HockeyZoneController : ZoneController, EventHandler<ChatEvent> {
  bool IsZone(Zone zone) override { return zone == Zone::Subgame; }

  void CreateBehaviors(const char* arena_name) override;
  void HandleEvent(const ChatEvent& event) override; // Add override keyword
};

static HockeyZoneController controller;

void HockeyZoneController::HandleEvent(const ChatEvent& event) {
  // Safety check in case the char* is null
  if (!event.message) return; 
  
  std::string msg = event.message;

  // Check if the message starts with "!setship "
  if (msg.rfind("!setship ", 0) == 0) {
    int user_ship = atoi(msg.substr(9).c_str());

    // Validate that it's a standard player-facing ship number (1-8)
    if (user_ship < 1 || user_ship > 8) {
      Log(LogLevel::Warning, "Invalid ship requested: %d. Please use 1-8.", user_ship);
      return;
    }

    // Convert to the server's internal index (0-7)
    int internal_ship = user_ship - 1;

    Log(LogLevel::Info, "Ship change requested: User %d -> Internal %d", user_ship, internal_ship);

    // Update the blackboard so the behavior tree adopts the new default
    bot->execute_ctx.blackboard.Set("request_ship", internal_ship);

    // Send the immediate network request
    bot->game->connection.SendShipRequest(internal_ship);
  }
}

void HockeyZoneController::CreateBehaviors(const char* arena_name) {
  Log(LogLevel::Info, "Registering HockeyZone behaviors for arena: %s", arena_name);

  bot->bot_controller->energy_tracker.estimate_type = EnergyHeuristicType::Average;

  // Enable the actuator so the bot can control input
  bot->bot_controller->actuator.enabled = true;
  Log(LogLevel::Info, "Actuator enabled.");

  auto& repo = bot->bot_controller->behaviors;

  // Hardcoded behavior - use with: Behavior = hockeyzone
  repo.Add("hockeyzone", std::make_unique<HockeyBehavior>());
  
  // ML behavior - use with: Behavior = hockeyzoneml
  repo.Add("hockeyzoneml", std::make_unique<MLHockeyBehavior>());
  
  Log(LogLevel::Info, "Behaviors registered: 'hockeyzone' (hardcoded), 'hockeyzoneml' (ML)");
  
  // Default to ML - but zero.cfg [General] Behavior setting will override this
  SetBehavior("hockeyzoneml");
}

}  // namespace hockeyzone
}  // namespace zero
