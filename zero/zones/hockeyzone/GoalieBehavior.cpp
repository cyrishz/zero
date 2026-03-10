#include "GoalieBehavior.h"

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
#include <algorithm>

namespace zero {
namespace hockeyzone {

namespace GoalieConfig {
  constexpr float kLeftGoalX = 408.0f;
  constexpr float kRightGoalX = 615.0f;
  constexpr float kCenterY = 511.5f;
  
  // THE POST BOUNDARIES
  constexpr float kTopPostY = 499.0f;
  constexpr float kBottomPostY = 524.0f;
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

    if (distance_to_lane < 3.5f) return false;
  }
  return true;
}

struct GoalieLeashNode : public behavior::BehaviorNode {
  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
      ctx.blackboard.Set("leash_distance", 1000.0f);
      return behavior::ExecuteResult::Success;
  }
};

struct GoalieIdleNode : public behavior::BehaviorNode {
  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    Vector2f our_net = (self->frequency == 0) 
        ? Vector2f(GoalieConfig::kLeftGoalX, GoalieConfig::kCenterY) 
        : Vector2f(GoalieConfig::kRightGoalX, GoalieConfig::kCenterY);

    float dist_home = self->position.Distance(our_net);
    
    if (dist_home > 3.0f) {
        float thrust = std::min(15.0f, dist_home);
        ctx.bot->bot_controller->steering.force = Normalize(our_net - self->position) * thrust;
        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, our_net);
    } else {
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, Vector2f(512.0f, 511.5f));
    }

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    return behavior::ExecuteResult::Success;
  }
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
      return behavior::ExecuteResult::Success;
    }

    return behavior::ExecuteResult::Failure;
  }
  const char* pass_target_key;
  const char* enemy_goal_key;
};

// PHASE 1: THE DYNAMIC ENERGY-AWARE MIRROR
struct GoalieDefendNode : public behavior::BehaviorNode {
  GoalieDefendNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    Vector2f puck_pos = *opt_puck;
    Vector2f optimal_spot;

    bool enemy_has_puck = false;
    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
        Player* p = ctx.bot->game->player_manager.players + i;
        if (p->ship >= 8 || p->id == self->id) continue;
        if (p->position.Distance(puck_pos) < 3.0f) {
            if (p->frequency != self->frequency) enemy_has_puck = true;
            break;
        }
    }

    // 1. DYNAMIC CREASE RETRACTION (Energy & Play Development)
    float max_step_out = 21.0f; // Default: Fully expanded into the red semi-circle

    // Energy Awareness: Retreat to the true crease if energy is below 50% (1100)
    if (self->energy < 1100) {
        max_step_out = 5.0f; 
    }

    // Corner Retreat: If the puck is deep in the corners, do NOT step out. Hug the post!
    if (std::abs(puck_pos.y - GoalieConfig::kCenterY) > 25.0f) {
        max_step_out = 2.0f; 
    }

    // 2. MIRROR PROTOCOL
    optimal_spot.y = std::clamp(puck_pos.y, GoalieConfig::kTopPostY, GoalieConfig::kBottomPostY);
    
    if (self->frequency == 0) { 
        float dist_x = puck_pos.x - GoalieConfig::kLeftGoalX;
        float step_out = std::clamp(dist_x * 0.30f, 0.0f, max_step_out);
        optimal_spot.x = GoalieConfig::kLeftGoalX + step_out;
    } else { 
        float dist_x = GoalieConfig::kRightGoalX - puck_pos.x;
        float step_out = std::clamp(dist_x * 0.30f, 0.0f, max_step_out);
        optimal_spot.x = GoalieConfig::kRightGoalX - step_out;
    }

    // 3. FLIGHT DYNAMICS & ENERGY CONSERVATION
    float dist_to_spot = self->position.Distance(optimal_spot);
    
    if (dist_to_spot < 1.5f) {
        // We are exactly where we need to be. Shut off engines to recharge energy!
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
    } else if (dist_to_spot < 3.0f) {
        // Soft brake to avoid jittering
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 5.0f; 
    } else {
        // Firm thrust to ensure he snaps into position and mirrors quickly
        float thrust = std::min(30.0f, dist_to_spot * 4.0f);
        ctx.bot->bot_controller->steering.force = Normalize(optimal_spot - self->position) * thrust;
    }
    
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, puck_pos);
    
    // 4. CREASE CLEARING (Pulsed Bullets if enemy gets too close)
    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
        
        if (self->position.Distance(puck_pos) < 18.0f && enemy_has_puck) {
            int cooldown = ctx.blackboard.ValueOr<int>("goalie_fire_cd", 0);
            if (cooldown == 0) {
                ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, true);
                ctx.blackboard.Set<int>("goalie_fire_cd", 1);
            } else {
                ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
                cooldown++;
                if (cooldown > 15) cooldown = 0; 
                ctx.blackboard.Set<int>("goalie_fire_cd", cooldown);
            }
        } else {
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        }
    }

    return behavior::ExecuteResult::Success;
  }
  const char* puck_key;
};

// PHASE 2: THE SWEEPER CLEAR (If he gets bumped and scoops the puck)
struct GoalieClearPuckNode : public behavior::BehaviorNode {
  GoalieClearPuckNode(const char* pass_target_key) : pass_target_key(pass_target_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    auto opt_pass = ctx.blackboard.Value<Vector2f>(pass_target_key);
    Vector2f target;
    
    if (opt_pass) {
        target = *opt_pass; // Tape-to-Tape pass
    } else {
        // Panic clear into the far corner away from the net
        if (self->frequency == 0) { 
            target = Vector2f(524.0f, (self->position.y > GoalieConfig::kCenterY) ? 580.0f : 430.0f);
        } else { 
            target = Vector2f(500.0f, (self->position.y > GoalieConfig::kCenterY) ? 580.0f : 430.0f);
        }
    }

    Vector2f to_target = target - self->position;
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, target);
    
    // Plant feet to pass
    if (self->velocity.Length() > 2.0f) {
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity);
    } else {
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
    }

    Vector2f heading = self->GetHeading();
    Vector2f target_dir = Normalize(to_target);
    
    if (heading.Dot(target_dir) > 0.98f && self->velocity.Length() < 3.0f) {
        ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
        return behavior::ExecuteResult::Success;
    }
    
    return behavior::ExecuteResult::Running;
  }
  const char* pass_target_key;
};

struct GoalieEnemyGoalQueryNode : public behavior::BehaviorNode {
  GoalieEnemyGoalQueryNode(const char* output_key) : output_key(output_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    Vector2f enemy_goal = (self->frequency == 0) 
        ? Vector2f(GoalieConfig::kRightGoalX, GoalieConfig::kCenterY) 
        : Vector2f(GoalieConfig::kLeftGoalX, GoalieConfig::kCenterY);
    
    ctx.blackboard.Set<Vector2f>(output_key, enemy_goal);
    return behavior::ExecuteResult::Success;
  }
  const char* output_key;
};

std::unique_ptr<behavior::BehaviorNode> GoalieBehavior::CreateTree(behavior::ExecuteContext& ctx) {
  using namespace behavior;
  BehaviorBuilder builder;

  // clang-format off
  builder
    .Selector()
        .Sequence()
            .Child<ExecuteNode>([](ExecuteContext& ctx) {
              auto self = ctx.bot->game->player_manager.GetSelf();
              if (!self) return ExecuteResult::Failure;
              if (self->ship >= 8) return ExecuteResult::Success;
              return ExecuteResult::Failure;
            })
            .Child<ShipRequestNode>(7)
            .End()

        // OFFENSE: If we somehow get the puck, pass it or clear it immediately.
        .Sequence()
            .Child<PowerballCarryQueryNode>()
            .Child<GoalieEnemyGoalQueryNode>("enemy_goal")
            .Selector()
                .Sequence()
                    .Child<FindOpenTeammateNode>("pass_target", "enemy_goal")
                    .Child<GoalieClearPuckNode>("pass_target")
                    .End()
                
                .Child<GoalieClearPuckNode>("null_key")
                .End()
            .End()

        // DEFENSE: The strict, dynamic-crease mirror node.
        .Sequence()
            .InvertChild<PowerballCarryQueryNode>()
            .Child<GoalieLeashNode>() 
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<GoalieDefendNode>("puck_position") 
            .End()

        .Child<GoalieIdleNode>()
        .End();
  // clang-format on

  return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
