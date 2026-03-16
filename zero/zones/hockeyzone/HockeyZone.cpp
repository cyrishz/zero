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
#include "DefenderBehavior.h"
#include "OffenseBehavior.h"

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

  // ===========================================================================
  // THE CIRCUIT BREAKER: Cleanly swap behaviors and zero out all controls
  // ===========================================================================
  if (msg.rfind("!behavior ", 0) == 0) {
    std::string target_behavior = msg.substr(10);
    
    Log(LogLevel::Info, "Behavior change requested to: %s. Initiating Circuit Breaker...", target_behavior.c_str());

    // 1. Clear internal steering forces so the ship stops trying to fly
    bot->bot_controller->steering.force = Vector2f(0, 0);

    // 2. Lift the "fingers" off the physical input keys
    if (bot->bot_controller->input) {
        bot->bot_controller->input->SetAction(InputAction::Forward, false);
        bot->bot_controller->input->SetAction(InputAction::Backward, false);
        bot->bot_controller->input->SetAction(InputAction::Left, false);
        bot->bot_controller->input->SetAction(InputAction::Right, false);
        bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
        bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        bot->bot_controller->input->SetAction(InputAction::Bomb, false);
    }
    
    // 3. Swap the Brain
    SetBehavior(target_behavior.c_str());
    return;
  }

  // ===========================================================================
  // SHIP MANAGEMENT
  // ===========================================================================
  if (msg.rfind("!setship ", 0) == 0) {
    int user_ship = atoi(msg.substr(9).c_str());

    // Validate that it's a standard ship (1-8) or spectator (9)
    if (user_ship < 1 || user_ship > 9) {
      Log(LogLevel::Warning, "Invalid ship requested: %d. Please use 1-9.", user_ship);
      return;
    }

    if (user_ship == 9) {
      Log(LogLevel::Info, "Spectator mode requested. Suspending AI.");
      
      // 1. Update blackboard so any dynamic nodes know we want to stay in spec
      bot->execute_ctx.blackboard.Set("request_ship", 8);
      
      // 2. Suspend the active behavior so the tree stops fighting us
      SetBehavior(""); 
      
      // 3. Send the TRUE Subspace packet to drop ship into the stands
      bot->game->connection.SendShipRequest(8);
    } else {
      // Convert to the server's internal index (0-7)
      int internal_ship = user_ship - 1;
      
      Log(LogLevel::Info, "Ship change requested: User %d -> Internal %d", user_ship, internal_ship);
      
      // Update blackboard for respawns
      bot->execute_ctx.blackboard.Set("request_ship", internal_ship);
      
      // TRUE FIX: Only default to ML if the bot is currently brainless (like coming out of spec).
      // If it's already a goalie, leave it alone!
      if (bot->bot_controller->behavior_name.empty()) {
          SetBehavior("hockeyzoneml");
      }
      
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
  
  // Goalie behavior - use with: !behavior goalie
  repo.Add("goalie", std::make_unique<GoalieBehavior>()); 
  
  // Offense behavior - use with: !behavior offense
  repo.Add("offense", std::make_unique<OffenseBehavior>());
  
  // Defender behavior - use with: !behavior defender
  repo.Add("defender", std::make_unique<DefenderBehavior>()); 

  Log(LogLevel::Info, "Behaviors registered: 'hockeyzone', 'hockeyzoneml', 'goalie', 'offense', 'defender'"); 

  // Default to ML - but zero.cfg [General] Behavior setting will override this
  SetBehavior("hockeyzoneml");
}

}  // namespace hockeyzone
}  // namespace zero
