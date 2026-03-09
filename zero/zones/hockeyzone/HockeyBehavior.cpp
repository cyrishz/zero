#include "HockeyBehavior.h"

#include <zero/behavior/BehaviorBuilder.h>
#include <zero/behavior/BehaviorTree.h>
#include <zero/behavior/nodes/AimNode.h>
#include <zero/behavior/nodes/BlackboardNode.h>
#include <zero/behavior/nodes/InputActionNode.h>
#include <zero/behavior/nodes/MapNode.h>
#include <zero/behavior/nodes/MathNode.h>
#include <zero/behavior/nodes/MoveNode.h>
#include <zero/behavior/nodes/PlayerNode.h>
#include <zero/behavior/nodes/PowerballNode.h>
#include <zero/behavior/nodes/RegionNode.h>
#include <zero/behavior/nodes/RenderNode.h>
#include <zero/behavior/nodes/ShipNode.h>
#include <zero/behavior/nodes/ThreatNode.h>
#include <zero/behavior/nodes/TimerNode.h>
#include <zero/behavior/nodes/WaypointNode.h>
#include <zero/game/Logger.h>
#include <cmath>
#include <cstring>

namespace zero {
namespace hockeyzone {

namespace HockeyConfig {
  constexpr float kLeftGoalX = 408.0f;
  constexpr float kRightGoalX = 615.0f;
  constexpr float kGoalTopPostY = 499.0f;
  constexpr float kGoalBottomPostY = 524.0f;
  
  constexpr float kLeftCreaseMinX = 405.0f;
  constexpr float kLeftCreaseMaxX = 419.0f;
  constexpr float kLeftCreaseMinY = 498.0f;
  constexpr float kLeftCreaseMaxY = 525.0f;
  
  constexpr float kRightCreaseMinX = 604.0f;
  constexpr float kRightCreaseMaxX = 618.0f;
  constexpr float kRightCreaseMinY = 498.0f;
  constexpr float kRightCreaseMaxY = 525.0f;
  
  constexpr float kLeftShootX = 450.0f;
  constexpr float kLeftShootY = 511.5f;
  constexpr float kRightShootX = 573.0f;
  constexpr float kRightShootY = 511.5f;
  
  constexpr float kCenterX = 512.0f;
  constexpr float kCenterY = 511.5f;
}

inline bool IsInLeftCrease(const Vector2f& pos) {
  return pos.x >= HockeyConfig::kLeftCreaseMinX && pos.x <= HockeyConfig::kLeftCreaseMaxX &&
         pos.y >= HockeyConfig::kLeftCreaseMinY && pos.y <= HockeyConfig::kLeftCreaseMaxY;
}

inline bool IsInRightCrease(const Vector2f& pos) {
  return pos.x >= HockeyConfig::kRightCreaseMinX && pos.x <= HockeyConfig::kRightCreaseMaxX &&
         pos.y >= HockeyConfig::kRightCreaseMinY && pos.y <= HockeyConfig::kRightCreaseMaxY;
}

inline bool IsInCrease(const Vector2f& pos) {
  return IsInLeftCrease(pos) || IsInRightCrease(pos);
}

inline bool IsLaneClear(Game& game, Vector2f start, Vector2f end, u16 my_freq) {
  Vector2f lane_vector = end - start;
  float lane_length = lane_vector.Length();
  if (lane_length == 0.0f) return true;
  
  Vector2f lane_dir = lane_vector / lane_length;

  for (size_t i = 0; i < game.player_manager.player_count; ++i) {
    Player* p = game.player_manager.players + i;
    if (p->ship >= 8 || p->frequency == my_freq) continue;

    Vector2f to_enemy = p->position - start;
    float projection = to_enemy.Dot(lane_dir);

    if (projection < 0.0f || projection > lane_length) continue;

    Vector2f closest_point = start + (lane_dir * projection);
    float distance_to_lane = closest_point.Distance(p->position);

    if (distance_to_lane < 3.5f) {
      return false;
    }
  }
  return true;
}

struct LaneIsClearNode : public behavior::BehaviorNode {
  LaneIsClearNode(const char* target_key) : target_key(target_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_target = ctx.blackboard.Value<Vector2f>(target_key);
    
    if (!self || !opt_target) return behavior::ExecuteResult::Failure;

    if (IsLaneClear(*ctx.bot->game, self->position, *opt_target, self->frequency)) {
      return behavior::ExecuteResult::Success;
    }
    
    return behavior::ExecuteResult::Failure;
  }
  const char* target_key;
};

struct FindOpenTeammateNode : public behavior::BehaviorNode {
  FindOpenTeammateNode(const char* pass_target_key, const char* enemy_goal_key) 
      : pass_target_key(pass_target_key), enemy_goal_key(enemy_goal_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_goal = ctx.blackboard.Value<Vector2f>(enemy_goal_key);
    
    if (!self || !opt_goal) return behavior::ExecuteResult::Failure;

    Player* best_teammate = nullptr;
    float my_dist_to_goal = self->position.Distance(*opt_goal);
    float best_teammate_dist_to_goal = my_dist_to_goal; 

    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
      Player* p = ctx.bot->game->player_manager.players + i;
      
      if (p->id == self->id || p->frequency != self->frequency || p->ship >= 8) continue;

      float their_dist_to_goal = p->position.Distance(*opt_goal);
      
      if (their_dist_to_goal < best_teammate_dist_to_goal) {
        if (IsLaneClear(*ctx.bot->game, self->position, p->position, self->frequency)) {
          best_teammate = p;
          best_teammate_dist_to_goal = their_dist_to_goal;
        }
      }
    }

    if (best_teammate) {
      ctx.blackboard.Set<Vector2f>(pass_target_key, best_teammate->position);
      Log(LogLevel::Info, "Found open pass to %s!", best_teammate->name);
      return behavior::ExecuteResult::Success;
    }

    return behavior::ExecuteResult::Failure;
  }
  
  const char* pass_target_key;
  const char* enemy_goal_key;
};

struct HockeyAwarenessChaseNode : public behavior::BehaviorNode {
  HockeyAwarenessChaseNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    Vector2f puck_pos = *opt_puck;
    Player* carrier = nullptr;

    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
      Player* p = ctx.bot->game->player_manager.players + i;
      if (p->ship >= 8 || p->id == self->id) continue;

      if (p->position.Distance(puck_pos) < 3.0f) {
        carrier = p;
        break;
      }
    }

    if (carrier) {
      if (carrier->frequency == self->frequency) {
        Vector2f open_pos;
        open_pos.x = (self->frequency == 0) ? HockeyConfig::kRightShootX : HockeyConfig::kLeftShootX;
        
        open_pos.y = (carrier->position.y > HockeyConfig::kCenterY) 
                     ? HockeyConfig::kCenterY - 15.0f 
                     : HockeyConfig::kCenterY + 15.0f;
            
        if (self->position.Distance(open_pos) < 8.0f && IsLaneClear(*ctx.bot->game, carrier->position, self->position, self->frequency)) {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
            ctx.bot->bot_controller->steering.Face(*ctx.bot->game, puck_pos); 
        } else {
            ctx.bot->bot_controller->steering.force = open_pos - self->position; 
        }
        return behavior::ExecuteResult::Success;
        
      } else {
        Vector2f to_enemy = carrier->position - self->position;
        float dist_to_enemy = self->position.Distance(carrier->position);
        
        ctx.bot->bot_controller->steering.force = to_enemy; 
        
        if (dist_to_enemy <= 2.0f) {
            ctx.bot->bot_controller->steering.Face(*ctx.bot->game, carrier->position);
            
            int cooldown = ctx.blackboard.ValueOr<int>("enforcer_cooldown", 0);
            if (cooldown == 0) {
                if (ctx.bot->bot_controller->input) {
                    ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, true);
                }
                ctx.blackboard.Set<int>("enforcer_cooldown", 1);
            } else {
                cooldown++;
                if (cooldown > 25) cooldown = 0; 
                ctx.blackboard.Set<int>("enforcer_cooldown", cooldown);
            }
        } else {
            ctx.blackboard.Set<int>("enforcer_cooldown", 0);
        }
        
        return behavior::ExecuteResult::Success;
      }
    }

    // UPGRADED: Loose puck chase (Anti-Orbiting Math)
    float dist = self->position.Distance(puck_pos);
    Vector2f to_puck = puck_pos - self->position;
    float current_speed = self->velocity.Length();
    
    if (dist < 14.5f) {
      // We are in the lock-on zone. If going too fast, slam the brakes!
      if (current_speed > 15.0f) {
          ctx.bot->bot_controller->steering.force = -Normalize(self->velocity); 
      } else {
          // Normal speed? Aim straight for the puck. No more scaling force to zero!
          ctx.bot->bot_controller->steering.force = to_puck;
      }
    } else {
      ctx.bot->bot_controller->steering.force = to_puck;
    }
    
    return behavior::ExecuteResult::Success;
  }
  const char* puck_key;
};

struct HockeyCarryPuckNode : public behavior::BehaviorNode {
  HockeyCarryPuckNode(const char* target_key) : target_key(target_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_target = ctx.blackboard.Value<Vector2f>(target_key);
    
    if (!self || !opt_target) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    Vector2f target = *opt_target;
    float current_speed = self->velocity.Length();
    
    if (current_speed > 18.0f) {
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, target); 
    } else {
        ctx.bot->bot_controller->steering.force = target - self->position; 
    }

    return behavior::ExecuteResult::Success;
  }
  const char* target_key;
};

struct HockeyGoalQueryNode : public behavior::BehaviorNode {
  HockeyGoalQueryNode(const char* output_key, bool get_enemy_goal = true) 
      : output_key(output_key), get_enemy_goal(get_enemy_goal) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    u16 freq = self->frequency;
    Vector2f goal_pos;
    
    bool attacking_right = (get_enemy_goal && freq == 0) || (!get_enemy_goal && freq != 0);
    
    float target_y = (self->position.y < HockeyConfig::kCenterY) 
                     ? HockeyConfig::kGoalBottomPostY 
                     : HockeyConfig::kGoalTopPostY;

    if (attacking_right) {
      goal_pos = Vector2f(HockeyConfig::kRightGoalX, target_y);
    } else {
      goal_pos = Vector2f(HockeyConfig::kLeftGoalX, target_y);
    }
    
    ctx.blackboard.Set<Vector2f>(output_key, goal_pos);
    return behavior::ExecuteResult::Success;
  }

  const char* output_key;
  bool get_enemy_goal;
};

struct HockeyShootingPositionNode : public behavior::BehaviorNode {
  HockeyShootingPositionNode(const char* output_key) : output_key(output_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    u16 freq = self->frequency;
    Vector2f shoot_pos;
    
    if (freq == 0) {
      shoot_pos = Vector2f(HockeyConfig::kRightShootX, HockeyConfig::kRightShootY);
    } else {
      shoot_pos = Vector2f(HockeyConfig::kLeftShootX, HockeyConfig::kLeftShootY);
    }
    
    ctx.blackboard.Set<Vector2f>(output_key, shoot_pos);
    return behavior::ExecuteResult::Success;
  }

  const char* output_key;
};

struct HockeyAimAndShootNode : public behavior::BehaviorNode {
  HockeyAimAndShootNode(const char* goal_key) : goal_key(goal_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    auto opt_goal = ctx.blackboard.Value<Vector2f>(goal_key);
    if (!opt_goal) return behavior::ExecuteResult::Failure;

    Vector2f goal = *opt_goal;
    Vector2f my_pos = self->position;
    Vector2f to_goal = goal - my_pos;
    
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, goal);
    
    if (self->velocity.Length() > 8.0f) {
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity);
    } else {
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
    }
    
    Vector2f heading = self->GetHeading();
    Vector2f goal_dir = Normalize(to_goal);
    float alignment = heading.Dot(goal_dir);
    
    Log(LogLevel::Debug, "Aim: pos=(%.0f,%.0f) goal=(%.0f,%.0f) alignment=%.3f",
        my_pos.x, my_pos.y, goal.x, goal.y, alignment);
    
    // UPGRADED: Tighter Sniper Alignment (from 0.97 to 0.99)
    if (alignment > 0.99f) {
      if (std::strcmp(goal_key, "pass_target") == 0) {
          Log(LogLevel::Info, "SNIPING PASS TO TEAMMATE! alignment=%.3f", alignment);
      } else {
          Log(LogLevel::Info, "FIRING SNAP SHOT AT POST! alignment=%.3f", alignment);
      }
      ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
      return behavior::ExecuteResult::Success;
    }
    
    return behavior::ExecuteResult::Running;
  }

  const char* goal_key;
};

struct HockeyInShootingRangeNode : public behavior::BehaviorNode {
  HockeyInShootingRangeNode(const char* shoot_pos_key, float range = 18.0f) 
      : shoot_pos_key(shoot_pos_key), range(range) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    if (IsInCrease(self->position)) {
      return behavior::ExecuteResult::Failure;
    }

    auto opt_shoot_pos = ctx.blackboard.Value<Vector2f>(shoot_pos_key);
    if (!opt_shoot_pos) return behavior::ExecuteResult::Failure;

    // We calculate distance to the target key dynamically
    float dist = self->position.Distance(*opt_shoot_pos);
    
    if (dist <= range) {
      return behavior::ExecuteResult::Success;
    }
    
    return behavior::ExecuteResult::Failure;
  }

  const char* shoot_pos_key;
  float range;
};

std::unique_ptr<behavior::BehaviorNode> HockeyBehavior::CreateTree(behavior::ExecuteContext& ctx) {
  using namespace behavior;

  BehaviorBuilder builder;
  Vector2f center_ice(HockeyConfig::kCenterX, HockeyConfig::kCenterY);

  // clang-format off
  builder
    .Selector()

        // Only request a ship if the bot is spectating.
        // This respects zero.cfg RequestShip and !setship commands.
        .Sequence()
            .Child<ExecuteNode>([](ExecuteContext& ctx) {
                auto self = ctx.bot->game->player_manager.GetSelf();
                if (!self) return ExecuteResult::Failure;
                if (self->ship == 8) return ExecuteResult::Success; // spectator only
                return ExecuteResult::Failure;
            })
            .Child<ShipRequestNode>("request_ship")
            .End()

        // Chase puck when nobody has it
        .Sequence()
            .InvertChild<PowerballCarryQueryNode>()
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<HockeyAwarenessChaseNode>("puck_position")
            .End()

        // Handle puck possession
        .Sequence()
            .Child<PowerballCarryQueryNode>()
            .Child<HockeyGoalQueryNode>("enemy_goal", true)
            .Child<HockeyShootingPositionNode>("shooting_position")

            .Selector()

                // Take shot if lane is clear
                .Sequence()
                    .Child<HockeyInShootingRangeNode>("enemy_goal", 35.0f)
                    .Child<LaneIsClearNode>("enemy_goal")
                    .Child<HockeyAimAndShootNode>("enemy_goal")
                    .End()

                // Pass if teammate is better positioned
                .Sequence()
                    .Child<FindOpenTeammateNode>("pass_target", "enemy_goal")
                    .Child<HockeyAimAndShootNode>("pass_target")
                    .End()

                // Otherwise carry puck to shooting position
                .Child<HockeyCarryPuckNode>("shooting_position")

            .End()
        .End()

        // Idle behavior (center ice)
        .Sequence()
            .Child<GoToNode>(center_ice)
            .End()

    .End();
  // clang-format on

  return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
