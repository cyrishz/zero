/*
    ZERO BOT - DEFENDER BEHAVIOR
    
    BASE BEHAVIOR FOCUS:
    - ML-Spider Inspired Gap Control (Maintains 26-tile cushion)
    - The "Tight Gap" Kill Zone (Closes to 6 tiles and forms a wall)
    - Face-Threat Braking (Snaps nose to attacker)
    - Reliable Burst Fire (3-frame hold, energy-aware for Spider)
    - Aim-Override (Snaps nose to threat when firing)
    - Natural Crease Repulsor (Clamps target X to avoid net tangling)
*/

#include "DefenderBehavior.h"

#include <zero/behavior/BehaviorBuilder.h>
#include <zero/behavior/BehaviorTree.h>
#include <zero/behavior/nodes/BlackboardNode.h>
#include <zero/behavior/nodes/MoveNode.h>
#include <zero/behavior/nodes/PlayerNode.h>
#include <zero/behavior/nodes/PowerballNode.h>
#include <zero/behavior/nodes/ShipNode.h>
#include <zero/game/Logger.h>

#include <cmath>
#include <algorithm>

namespace zero {
namespace hockeyzone {

// =============================================================================
// CONFIGURATION: SPIDER TACTICS
// =============================================================================
namespace DefenderConfig {
    constexpr float kLeftGoalLineX = 411.0f;
    constexpr float kRightGoalLineX = 613.0f; 
    constexpr float kCenterY[2] = { 511.5f, 736.0f }; 

    // SPIDER ML METRICS
    constexpr float kGapDistance = 26.0f;        // Normal cushion
    constexpr float kTightGapDistance = 6.0f;    // Aggressive cushion (Stops overshoot!)
    constexpr float kKillZoneRadius = 15.0f;     // Trigger aggressive tight gap
    constexpr float kMaxYOffset = 25.0f;         // Slot lock
    
    constexpr float kCreaseForcefield = 12.0f;   // Stay out of the goalie's paint
}

// =============================================================================
// THE DEFENDER
// =============================================================================
struct DefenderDefendNode : public behavior::BehaviorNode {
    DefenderDefendNode(const char* puck_key) : puck_key(puck_key) {}

    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
        if (!opt_puck) return behavior::ExecuteResult::Failure;
        
        Vector2f puck_pos = *opt_puck;
        float center_y = DefenderConfig::kCenterY[rink_index];

        if (std::abs(puck_pos.y - center_y) > 100.0f) return behavior::ExecuteResult::Failure;

        bool defend_left = (self->frequency % 2 == 0);
        float goal_line_x = defend_left ? DefenderConfig::kLeftGoalLineX : DefenderConfig::kRightGoalLineX;

        // === FIND THE THREAT ===
        Vector2f threat_pos = puck_pos;
        Vector2f threat_vel(0, 0);
        
        for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
            Player* p = ctx.bot->game->player_manager.players + i;
            if (p->ship >= 8 || p->frequency == self->frequency) continue;
            if (std::abs(p->position.y - center_y) > 100.0f) continue;

            if (p->position.Distance(puck_pos) < 3.0f) {
                threat_pos = p->position;
                threat_vel = p->velocity;
                break;
            }
        }

        // === SPIDER GAP CONTROL MATH ===
        float current_dist_to_puck = self->position.Distance(puck_pos);
        bool in_kill_zone = (current_dist_to_puck < DefenderConfig::kKillZoneRadius);

        // Dynamically select our cushion distance
        float gap = in_kill_zone ? DefenderConfig::kTightGapDistance : DefenderConfig::kGapDistance;
        
        float want_x = defend_left 
            ? (puck_pos.x - gap) 
            : (puck_pos.x + gap);

        // Natural Repulsor: Mathematically forbid targeting inside the crease
        if (defend_left) {
            want_x = std::clamp(want_x, goal_line_x + DefenderConfig::kCreaseForcefield, 500.0f);
        } else {
            want_x = std::clamp(want_x, 524.0f, goal_line_x - DefenderConfig::kCreaseForcefield);
        }

        // Predict Y slightly, but lock it to the slot
        float predicted_y = threat_pos.y + (threat_vel.y * 0.20f);
        float want_y = std::clamp(predicted_y, center_y - DefenderConfig::kMaxYOffset, center_y + DefenderConfig::kMaxYOffset);

        Vector2f target(want_x, want_y);

        // === WEAPON SYSTEM & AIM CALCULATION ===
        static int bullet_cooldown = 0;
        static int bullet_hold_frames = 0;
        bool use_bullet = false;
        bool is_shooting_stance = false; // Flag to tell steering to aim at player
        
        if (bullet_cooldown > 0) bullet_cooldown--;

        if (bullet_hold_frames > 0) {
            use_bullet = true;
            is_shooting_stance = true;
            bullet_hold_frames--;
        } else {
            float current_dist_to_threat = self->position.Distance(threat_pos);
            
            // Spider max energy is 400 (~133 per bullet). 
            // We require > 135.0f to ensure we have enough juice to actually spawn the bullet.
            if (current_dist_to_threat <= 9.0f && bullet_cooldown <= 0 && self->energy > 135.0f) {
                use_bullet = true;
                is_shooting_stance = true;
                bullet_hold_frames = 3; // 3-frame hold to bypass server drops
                bullet_cooldown = 150; 
                
                // INSTANT LOG: Prints exactly when the bot pulls the trigger
                Log(LogLevel::Info, "💥 [Defender] FIRED at Threat:(%.1f, %.1f) | Dist: %.1f | Energy: %.1f", 
                    threat_pos.x, threat_pos.y, current_dist_to_threat, self->energy);
            }
        }

        // === FLIGHT DYNAMICS ===
        Vector2f to_target = target - self->position;
        float dist_to_target = to_target.Length();

        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
        bool use_ab = false;

        if (dist_to_target > 2.0f) {
            float desired_speed = std::min(in_kill_zone ? 45.0f : 35.0f, dist_to_target * 5.0f);
            Vector2f desired_velocity = Normalize(to_target) * desired_speed;
            Vector2f force_needed = desired_velocity - self->velocity;
            
            if (!in_kill_zone) force_needed.y += (threat_vel.y * 0.35f); 

            // AIM PRIORITY: If pulling the trigger, look at the threat. Otherwise, look where we are flying.
            if (is_shooting_stance) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, threat_pos);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + force_needed);
            }
            
            ctx.bot->bot_controller->steering.force = force_needed;
            use_ab = (dist_to_target > 6.0f && self->energy > 800.0f);
            
        } else {
            // ARRIVED AT CUSHION: Slam brakes and track threat
            Vector2f brake_force = -self->velocity * 4.0f;
            ctx.bot->bot_controller->steering.Face(*ctx.bot->game, threat_pos);
            ctx.bot->bot_controller->steering.force = brake_force; 
        }

        // Apply hardware inputs
        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, use_ab);
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, use_bullet);
        }

        // --- THROTTLED LOGGING ---
        static int defend_ticks = 0;
        if (++defend_ticks % 100 == 0) {
            Log(LogLevel::Info, "[Defender] Target:(%.1f, %.1f) | Cushion:%.1f | KillZone:%d | FireStance:%d", 
                want_x, want_y, std::abs(puck_pos.x - self->position.x), in_kill_zone, is_shooting_stance);
        }

        return behavior::ExecuteResult::Success;
    }

    const char* puck_key;
};

// =============================================================================
// Clear puck when we have it
// =============================================================================
struct DefenderClearNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        bool defend_left = (self->frequency % 2 == 0);
        float center_y = DefenderConfig::kCenterY[rink_index];
        
        Vector2f clear_target;
        clear_target.x = defend_left ? 512.0f : 512.0f;
        clear_target.y = center_y;

        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, clear_target);
        
        if (self->velocity.Length() > 2.0f) {
            ctx.bot->bot_controller->steering.force = Normalize(self->velocity) * -10.0f;
        } else {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
        }

        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        }

        Vector2f to_target = Normalize(clear_target - self->position);
        float alignment = self->GetHeading().Dot(to_target);

        if (alignment > 0.90f) {
            ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
            return behavior::ExecuteResult::Success;
        }

        return behavior::ExecuteResult::Running;
    }
};

// =============================================================================
// Idle at home
// =============================================================================
struct DefenderIdleNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
            return behavior::ExecuteResult::Success; 
        }

        bool defend_left = (self->frequency % 2 == 0);
        float goal_line_x = defend_left ? DefenderConfig::kLeftGoalLineX : DefenderConfig::kRightGoalLineX;
        float center_y = DefenderConfig::kCenterY[rink_index];
        
        float home_x = defend_left 
            ? goal_line_x + DefenderConfig::kCreaseForcefield + 10.0f
            : goal_line_x - DefenderConfig::kCreaseForcefield - 10.0f;
            
        Vector2f home(home_x, center_y);
        Vector2f to_home = home - self->position;
        float dist_to_home = to_home.Length();

        bool is_moving = ctx.blackboard.ValueOr<bool>("def_idle_is_moving", false);

        if (dist_to_home > 3.0f) {
            is_moving = true;
        } else if (dist_to_home < 0.5f) {
            is_moving = false;
        }

        ctx.blackboard.Set<bool>("def_idle_is_moving", is_moving);
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);

        if (is_moving) {
            float desired_speed = std::min(40.0f, dist_to_home * 4.0f);
            Vector2f desired_velocity = Normalize(to_home) * desired_speed;
            Vector2f force_needed = desired_velocity - self->velocity;
            
            ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + force_needed);
            ctx.bot->bot_controller->steering.force = force_needed;
        } else {
            ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + Vector2f(0, -1));
            ctx.bot->bot_controller->steering.force = -self->velocity * 3.0f;
        }

        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        }

        return behavior::ExecuteResult::Success;
    }
};

// =============================================================================
// BEHAVIOR TREE
// =============================================================================

void DefenderBehavior::OnInitialize(behavior::ExecuteContext& ctx) {}

std::unique_ptr<behavior::BehaviorNode> DefenderBehavior::CreateTree(behavior::ExecuteContext& ctx) {
    using namespace behavior;
    BehaviorBuilder builder;

    builder
        .Selector()
            .Sequence()
                .Child<ExecuteNode>([](ExecuteContext& ctx) {
                    auto self = ctx.bot->game->player_manager.GetSelf();
                    if (!self) return ExecuteResult::Failure;
                    return self->ship >= 8 ? ExecuteResult::Success : ExecuteResult::Failure;
                })
                .Child<ShipRequestNode>(7)
                .End()

            .Selector()
                .Sequence()
                    .Child<PowerballCarryQueryNode>()
                    .Child<DefenderClearNode>()
                    .End()

                .Sequence()
                    .InvertChild<PowerballCarryQueryNode>()
                    .Child<PowerballClosestQueryNode>("puck_position", true)
                    .Child<DefenderDefendNode>("puck_position")
                    .End()

                .Child<DefenderIdleNode>()
                .End()
            .End();

    return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
