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
#include "GoalieBehavior.h"

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

    // Validate that it's a standard ship (1-8) or spectator (9)
    if (user_ship < 1 || user_ship > 9) {
      Log(LogLevel::Warning, "Invalid ship requested: %d. Please use 1-9.", user_ship);
      return;
    }

    if (user_ship == 9) {
      Log(LogLevel::Info, "Spectator mode requested. Suspending ML AI.");
      
      // Update blackboard just to keep it in sync
      bot->execute_ctx.blackboard.Set("request_ship", 8);
      
      // Suspend the active behavior so the tree stops fighting us!
      SetBehavior(""); 
      
      // Send the standard ship request packet to drop to spec
      bot->game->connection.SendShipRequest(8);
    } else {
      // Convert to the server's internal index (0-7)
      int internal_ship = user_ship - 1;
      
      Log(LogLevel::Info, "Ship change requested: User %d -> Internal %d", user_ship, internal_ship);
      
      // Update blackboard for respawns
      bot->execute_ctx.blackboard.Set("request_ship", internal_ship);
      
      // Re-enable the bot's brain so it starts playing again!
      SetBehavior("hockeyzoneml");
      
      // Send the standard ship request packet
      bot->game->connection.SendShipRequest(internal_ship);
    }
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
  
  // Goalie behavior - use with: !behavior goalie
  repo.Add("goalie", std::make_unique<GoalieBehavior>()); // <-- ADD THIS LINE

  // Optional: Update your log message so you know it loaded!
  Log(LogLevel::Info, "Behaviors registered: 'hockeyzone' (hardcoded), 'hockeyzoneml' (ML), 'goalie'");
  // Default to ML - but zero.cfg [General] Behavior setting will override this
  SetBehavior("hockeyzoneml");
}

}  // namespace hockeyzone
}  // namespace zero
