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

namespace zero {
namespace hockeyzone {

namespace HockeyConfig {
  constexpr float kLeftGoalX = 405.0f;
  constexpr float kLeftGoalY = 511.5f;
  constexpr float kRightGoalX = 618.0f;
  constexpr float kRightGoalY = 511.5f;
  
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

struct HockeyGoalQueryNode : public behavior::BehaviorNode {
  HockeyGoalQueryNode(const char* output_key, bool get_enemy_goal = true) 
      : output_key(output_key), get_enemy_goal(get_enemy_goal) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    u16 freq = self->frequency;
    Vector2f goal_pos;
    
    if (get_enemy_goal) {
      if (freq == 0) {
        goal_pos = Vector2f(HockeyConfig::kRightGoalX, HockeyConfig::kRightGoalY);
      } else {
        goal_pos = Vector2f(HockeyConfig::kLeftGoalX, HockeyConfig::kLeftGoalY);
      }
    } else {
      if (freq == 0) {
        goal_pos = Vector2f(HockeyConfig::kLeftGoalX, HockeyConfig::kLeftGoalY);
      } else {
        goal_pos = Vector2f(HockeyConfig::kRightGoalX, HockeyConfig::kRightGoalY);
      }
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

// Aim using steering.Face() and fire when aligned
struct HockeyAimAndShootNode : public behavior::BehaviorNode {
  HockeyAimAndShootNode(const char* goal_key) : goal_key(goal_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    auto opt_goal = ctx.blackboard.Value<Vector2f>(goal_key);
    if (!opt_goal) return behavior::ExecuteResult::Failure;

    Vector2f goal = *opt_goal;
    Vector2f my_pos = self->position;
    Vector2f to_goal = goal - my_pos;
    
    // Use steering system to rotate toward goal
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, goal);
    
    // Check if we're aligned - compare ship heading to goal direction
    Vector2f heading = self->GetHeading();
    Vector2f goal_dir = Normalize(to_goal);
    
    // Dot product: 1.0 = perfectly aligned, 0 = perpendicular, -1 = opposite
    float alignment = heading.Dot(goal_dir);
    
    Log(LogLevel::Debug, "Aim: pos=(%.0f,%.0f) goal=(%.0f,%.0f) alignment=%.3f heading=(%.2f,%.2f) goaldir=(%.2f,%.2f)",
        my_pos.x, my_pos.y, goal.x, goal.y, alignment, heading.x, heading.y, goal_dir.x, goal_dir.y);
    
    // If alignment > 0.98 (about 11 degrees), we're close enough - fire!
    if (alignment > 0.98f) {
      Log(LogLevel::Info, "FIRING! alignment=%.3f", alignment);
      ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
      return behavior::ExecuteResult::Success;
    }
    
    // Still rotating
    return behavior::ExecuteResult::Running;
  }

  const char* goal_key;
};

struct HockeyInShootingRangeNode : public behavior::BehaviorNode {
  HockeyInShootingRangeNode(const char* shoot_pos_key, float range = 8.0f) 
      : shoot_pos_key(shoot_pos_key), range(range) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    // Don't shoot from inside the crease
    if (IsInCrease(self->position)) {
      return behavior::ExecuteResult::Failure;
    }

    auto opt_shoot_pos = ctx.blackboard.Value<Vector2f>(shoot_pos_key);
    if (!opt_shoot_pos) return behavior::ExecuteResult::Failure;

    float dist = self->position.Distance(*opt_shoot_pos);
    
    // Also check we're roughly stopped (low velocity) so we're stable
    float speed = self->velocity.Length();
    
    if (dist <= range && speed < 3.0f) {
      Log(LogLevel::Debug, "InShootingRange: dist=%.1f speed=%.1f - READY", dist, speed);
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
        // Sequence 1: Enter the game if in spec
        .Sequence()
            .Child<ExecuteNode>([](ExecuteContext& ctx) {
              auto self = ctx.bot->game->player_manager.GetSelf();
              if (!self) return ExecuteResult::Failure;
              if (self->ship >= 8) return ExecuteResult::Success;
              return ExecuteResult::Failure;
            })
            .Child<ShipRequestNode>(0)
            .End()

        // Sequence 2: If we don't have the puck, go get it
        .Sequence()
            .InvertChild<PowerballCarryQueryNode>()
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<GoToNode>("puck_position")
            .End()

        // Sequence 3: We have the puck - get in position, aim, and shoot
        .Sequence()
            .Child<PowerballCarryQueryNode>()
            .Child<HockeyGoalQueryNode>("enemy_goal", true)
            .Child<HockeyShootingPositionNode>("shooting_position")
            .Selector()
                // Option A: In shooting range - aim and fire
                .Sequence()
                    .Child<HockeyInShootingRangeNode>("shooting_position", 12.0f)
                    .Child<HockeyAimAndShootNode>("enemy_goal")
                    .End()
                
                // Option B: Move to shooting position
                .Child<GoToNode>("shooting_position")
                .End()
            .End()

        // Sequence 4: Default - go to center ice
        .Sequence()
            .Child<GoToNode>(center_ice)
            .End()
        .End();
  // clang-format on

  return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
