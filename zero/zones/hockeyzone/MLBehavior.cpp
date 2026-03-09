#include "MLBehavior.h"
#include "ShipBrains.h"

#include <zero/behavior/BehaviorBuilder.h>
#include <zero/behavior/BehaviorTree.h>
#include <zero/behavior/nodes/BlackboardNode.h>
#include <zero/behavior/nodes/MoveNode.h>
#include <zero/behavior/nodes/PlayerNode.h>
#include <zero/behavior/nodes/PowerballNode.h>
#include <zero/behavior/nodes/ShipNode.h>
#include <zero/game/Clock.h>        // For GetCurrentTick()
#include <zero/game/InputState.h>   // For InputAction
#include <zero/game/Logger.h>
#include <cmath>

namespace zero {
namespace hockeyzone {

// ============================================================================
// Hockey constants (same as HockeyBehavior)
// ============================================================================
namespace HockeyConfig {
  constexpr float kLeftGoalX = 405.0f;
  constexpr float kLeftGoalY = 511.5f;
  constexpr float kRightGoalX = 618.0f;
  constexpr float kRightGoalY = 511.5f;
  constexpr float kCenterX = 512.0f;
  constexpr float kCenterY = 511.5f;
}

// ============================================================================
// Anti-oscillation constants
// ============================================================================
namespace AntiOscillation {
  // FIX 1: Damping radius - within this range, blend ML direction with current heading
  constexpr float kDampingRadius = 4.0f;
  
  // FIX 2: If approach quality is above this AND within settle range, maintain course
  constexpr float kApproachQualityThreshold = 0.75f;
  constexpr float kSettleRange = 6.0f;
  
  // FIX 3: Ticks to pause steering after firing (100 ticks ≈ 1 second)
  constexpr u32 kPostFirePauseTicks = 30;  // ~0.3 seconds
}

// ============================================================================
// MLSteerNode - Neural network driven steering + opportunistic shooting
// ============================================================================
behavior::ExecuteResult MLSteerNode::Execute(behavior::ExecuteContext& ctx) {
  auto self = ctx.bot->game->player_manager.GetSelf();
  if (!self) return behavior::ExecuteResult::Failure;

  // Get ball position from blackboard
  auto opt_ball = ctx.blackboard.Value<Vector2f>(ball_pos_key);
  if (!opt_ball) return behavior::ExecuteResult::Failure;

  Vector2f ball_pos = *opt_ball;
  Vector2f my_pos = self->position;
  Vector2f my_vel = self->velocity;
  Vector2f heading = self->GetHeading();
  
  u32 current_time = GetCurrentTick();

  // ==========================================================================
  // FIX 3: Post-fire steering pause
  // After firing, briefly coast to let the shot leave before re-engaging
  // ==========================================================================
  if (current_time < post_fire_pause_until) {
    // During pause, just maintain current velocity (no steering changes)
    Log(LogLevel::Debug, "ML: Post-fire pause active, coasting...");
    return behavior::ExecuteResult::Running;
  }

  // Calculate relative ball position
  float rel_bx = ball_pos.x - my_pos.x;
  float rel_by = ball_pos.y - my_pos.y;
  float dist = std::sqrt(rel_bx * rel_bx + rel_by * rel_by);
  if (dist < 0.01f) dist = 0.01f;

  // Build raw input feature vector (7 features)
  float raw_features[7] = {
    rel_bx,
    rel_by,
    my_vel.x,
    my_vel.y,
    heading.x,
    heading.y,
    dist
  };

  // DEBUG: Log raw values to compare with training data
  Log(LogLevel::Info, "RAW: rel_bx=%.1f rel_by=%.1f dist=%.1f vel=(%.1f,%.1f) head=(%.2f,%.2f)",
      rel_bx, rel_by, dist, my_vel.x, my_vel.y, heading.x, heading.y);

  // Get current ship type
  ShipType ship_type = static_cast<ShipType>(self->ship);
  
  // Run brain with automatic normalization
  float outputs[2];
  RunShipBrainRaw(ship_type, raw_features, outputs);

  // ML output is a normalized direction vector (via Tanh, so in [-1, 1])
  float ml_dir_x = outputs[0];
  float ml_dir_y = outputs[1];
  
  // Normalize ML direction
  float ml_len = std::sqrt(ml_dir_x * ml_dir_x + ml_dir_y * ml_dir_y);
  if (ml_len > 0.01f) {
    ml_dir_x /= ml_len;
    ml_dir_y /= ml_len;
  }
  
  // ==========================================================================
  // FIX 2: Velocity-based approach quality check
  // If we're already approaching the ball well at close range, maintain course
  // ==========================================================================
  float speed = my_vel.Length();
  Vector2f to_ball_dir = Normalize(ball_pos - my_pos);
  
  float final_dir_x = ml_dir_x;
  float final_dir_y = ml_dir_y;
  
  if (speed > 1.0f && dist < AntiOscillation::kSettleRange) {
    Vector2f vel_dir = Normalize(my_vel);
    float approach_quality = vel_dir.Dot(to_ball_dir);
    
    if (approach_quality > AntiOscillation::kApproachQualityThreshold) {
      // Already approaching well - maintain current velocity direction
      final_dir_x = vel_dir.x;
      final_dir_y = vel_dir.y;
      Log(LogLevel::Debug, "ML: Good approach (quality=%.2f), maintaining course", approach_quality);
    }
  }
  
  // ==========================================================================
  // FIX 1: Close-range damping factor
  // When very close, blend ML direction with current heading to reduce jitter
  // ==========================================================================
  if (dist < AntiOscillation::kDampingRadius) {
    float damping = dist / AntiOscillation::kDampingRadius;  // 0.0 to 1.0
    float inv_damping = 1.0f - damping;
    
    // Blend: at dist=0, use 100% heading; at dist=kDampingRadius, use 100% ML
    final_dir_x = final_dir_x * damping + heading.x * inv_damping;
    final_dir_y = final_dir_y * damping + heading.y * inv_damping;
    
    // Re-normalize
    float blend_len = std::sqrt(final_dir_x * final_dir_x + final_dir_y * final_dir_y);
    if (blend_len > 0.01f) {
      final_dir_x /= blend_len;
      final_dir_y /= blend_len;
    }
    
    Log(LogLevel::Debug, "ML: Close-range damping (dist=%.1f, factor=%.2f)", dist, damping);
  }
  
  // Compute target position based on final direction
  float move_dist = 15.0f;
  Vector2f target = my_pos + Vector2f(final_dir_x * move_dist, final_dir_y * move_dist);
  
  Log(LogLevel::Debug, "ML[ship%d]: pos=(%.0f,%.0f) ball=(%.0f,%.0f) ML_dir=(%.2f,%.2f) final_dir=(%.2f,%.2f)",
      self->ship, my_pos.x, my_pos.y, ball_pos.x, ball_pos.y, ml_dir_x, ml_dir_y, final_dir_x, final_dir_y);
  
  // Check if ball is in/behind a goal (unreachable)
  bool ball_in_left_goal = ball_pos.x < HockeyConfig::kLeftGoalX + 2;
  bool ball_in_right_goal = ball_pos.x > HockeyConfig::kRightGoalX - 2;
  
  if (ball_in_left_goal || ball_in_right_goal) {
    target = Vector2f(HockeyConfig::kCenterX, HockeyConfig::kCenterY);
    Log(LogLevel::Debug, "ML[ship%d]: Ball in goal, going to center", self->ship);
  }

  // Use steering to move toward target
  ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, target, 0.0f);

  // ==========================================================================
  // DEFENSIVE SHOOTING - Check/kill enemies near ball
  // ==========================================================================
  u16 my_freq = self->frequency;
  
  // Only try to shoot if we have energy and cooldown elapsed
  if (self->energy >= 400 && (current_time - last_fire_time) >= 250) {
    const Player* best_target = nullptr;
    float best_score = 0.0f;

    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
      const Player* player = ctx.bot->game->player_manager.players + i;
      
      // Skip self, teammates, spectators
      if (player->id == self->id) continue;
      if (player->frequency == my_freq) continue;
      if (player->ship >= 8) continue;

      // How close is this enemy to the ball?
      float enemy_ball_dist = (player->position - ball_pos).Length();
      
      // Only consider enemies near the ball (within 6 tiles)
      if (enemy_ball_dist > 6.0f) continue;

      // How far are they from us?
      float enemy_my_dist = (player->position - my_pos).Length();
      
      // Only consider enemies within shooting range (25 tiles)
      if (enemy_my_dist > 25.0f) continue;

      // Check if we're roughly facing them
      Vector2f to_enemy = Normalize(player->position - my_pos);
      float alignment = heading.Dot(to_enemy);
      
      // Need to be facing within ~25 degrees (dot > 0.90)
      if (alignment < 0.85f) continue;

      // Score: prefer better alignment, closer to us
      float score = alignment * (25.0f / (enemy_my_dist + 1.0f));
      
      if (score > best_score) {
        best_score = score;
        best_target = player;
      }
    }

    // If we found a good target, FIRE!
    if (best_target && best_score > 0.8f) {
      float fire_dist = (best_target->position - my_pos).Length();
      Log(LogLevel::Info, "CHECK! Firing at %s (dist=%.1f, score=%.2f)", 
          best_target->name, fire_dist, best_score);
      
      ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, true);
      last_fire_time = current_time;
      
      // FIX 3: Set post-fire pause
      post_fire_pause_until = current_time + AntiOscillation::kPostFirePauseTicks;
    }
  }

  return behavior::ExecuteResult::Running;
}

// ============================================================================
// MLAimAndShootNode - Hardcoded carry/shoot logic
// ============================================================================
struct MLAimAndShootNode : public behavior::BehaviorNode {
  MLAimAndShootNode(const char* ball_key, const char* goal_key) 
      : ball_key(ball_key), goal_key(goal_key), last_ball_fire_time(0) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    auto opt_goal = ctx.blackboard.Value<Vector2f>(goal_key);
    if (!opt_goal) return behavior::ExecuteResult::Failure;

    Vector2f goal = *opt_goal;
    Vector2f my_pos = self->position;
    Vector2f my_vel = self->velocity;
    Vector2f heading = self->GetHeading();
    u16 freq = self->frequency;
    
    u32 current_time = GetCurrentTick();

    Vector2f shoot_pos;
    bool in_shooting_zone;
    bool behind_goal;
    
    if (freq == 0) {
      shoot_pos = Vector2f(573.0f, HockeyConfig::kCenterY);
      in_shooting_zone = (my_pos.x > 550.0f && my_pos.x < 600.0f);
      behind_goal = (my_pos.x > HockeyConfig::kRightGoalX - 5.0f);
    } else {
      shoot_pos = Vector2f(450.0f, HockeyConfig::kCenterY);
      in_shooting_zone = (my_pos.x > 420.0f && my_pos.x < 470.0f);
      behind_goal = (my_pos.x < HockeyConfig::kLeftGoalX + 5.0f);
    }

    Vector2f to_goal = goal - my_pos;
    float goal_dist = to_goal.Length();
    Vector2f goal_dir = Normalize(to_goal);
    float alignment = heading.Dot(goal_dir);
    float speed = my_vel.Length();

    if (behind_goal) {
      Log(LogLevel::Info, "CARRY: Behind goal! Repositioning to shoot_pos=(%.0f,%.0f)", 
          shoot_pos.x, shoot_pos.y);
      ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, shoot_pos, 0.0f);
      return behavior::ExecuteResult::Running;
    }

    if (in_shooting_zone && alignment > 0.90f && speed < 6.0f && goal_dist < 80.0f) {
      // Prevent double-fire: check cooldown (at least 50 ticks between fires)
      if ((current_time - last_ball_fire_time) >= 50) {
        Log(LogLevel::Info, "FIRING! align=%.3f speed=%.1f goal_dist=%.1f", 
            alignment, speed, goal_dist);
        ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
        last_ball_fire_time = current_time;
        return behavior::ExecuteResult::Success;
      } else {
        Log(LogLevel::Debug, "CARRY: Fire cooldown active, waiting...");
        return behavior::ExecuteResult::Running;
      }
    }

    if (in_shooting_zone) {
      Log(LogLevel::Debug, "CARRY: In zone, aligning... align=%.2f", alignment);
      ctx.bot->bot_controller->steering.Face(*ctx.bot->game, goal);
      Vector2f target = my_pos + goal_dir * 5.0f;
      ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, target, 0.0f);
      return behavior::ExecuteResult::Running;
    }

    Log(LogLevel::Debug, "CARRY: Going to shoot_pos=(%.0f,%.0f) from (%.0f,%.0f)",
        shoot_pos.x, shoot_pos.y, my_pos.x, my_pos.y);
    ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, shoot_pos, 0.0f);
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, goal);

    return behavior::ExecuteResult::Running;
  }

  const char* ball_key;
  const char* goal_key;
  mutable u32 last_ball_fire_time;  // Prevent double-fire
};

// ============================================================================
// MLGoalQueryNode - Get enemy goal position
// ============================================================================
struct MLGoalQueryNode : public behavior::BehaviorNode {
  MLGoalQueryNode(const char* output_key) : output_key(output_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    u16 freq = self->frequency;
    Vector2f goal_pos;
    
    if (freq == 0) {
      goal_pos = Vector2f(HockeyConfig::kRightGoalX, HockeyConfig::kRightGoalY);
    } else {
      goal_pos = Vector2f(HockeyConfig::kLeftGoalX, HockeyConfig::kLeftGoalY);
    }
    
    ctx.blackboard.Set<Vector2f>(output_key, goal_pos);
    return behavior::ExecuteResult::Success;
  }

  const char* output_key;
};

// ============================================================================
// MLHockeyBehavior::CreateTree
// ============================================================================
std::unique_ptr<behavior::BehaviorNode> MLHockeyBehavior::CreateTree(behavior::ExecuteContext& ctx) {
  using namespace behavior;

  BehaviorBuilder builder;
  Vector2f center_ice(HockeyConfig::kCenterX, HockeyConfig::kCenterY);

  // clang-format off
  builder
    .Selector()
        // If in spectator mode, request Warbird (ship 0) - the only trained brain
        .Sequence()
            .Child<ExecuteNode>([](ExecuteContext& ctx) {
              auto self = ctx.bot->game->player_manager.GetSelf();
              if (!self) return ExecuteResult::Failure;
              if (self->ship >= 8) return ExecuteResult::Success;
              return ExecuteResult::Failure;
            })
            .Child<ShipRequestNode>("request_ship")
            .End()

        // If carrying the ball, aim and shoot
        .Sequence()
            .Child<PowerballCarryQueryNode>()
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<MLGoalQueryNode>("enemy_goal")
            .Child<MLAimAndShootNode>("puck_position", "enemy_goal")
            .End()

        // Otherwise, use ML steering to chase the ball
        .Sequence()
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<MLSteerNode>("puck_position")
            .End()

        // Fallback: go to center ice
        .Sequence()
            .Child<GoToNode>(center_ice)
            .End()
        .End();
  // clang-format on

  return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
