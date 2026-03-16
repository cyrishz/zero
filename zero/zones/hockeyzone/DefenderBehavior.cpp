/*
    ZERO BOT - DEFENDER BEHAVIOR
    
    BASE BEHAVIOR FOCUS:
    - Lemonaire Package (Shadowing, Zoning, Micro-thrusting/Coasting)
    - "The Bite" (Fixed Energy Bug: Now properly rushes stopped targets)
    - Juke Dampening (Aims for center-of-mass rather than over-predicting)
    - Snappy Trigger Cones (Fires while math is fresh, no over-aiming delay)
    - True Ballistic Leading (+0.025s latency compensation)
    - Inherited Velocity Correction (Compensates for shooter's slide)
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
    constexpr float kGapDistance = 26.0f;        
    constexpr float kTightGapDistance = 6.0f;    
    constexpr float kKillZoneRadius = 15.0f;     
    constexpr float kMaxYOffset = 25.0f;         
    
    constexpr float kCreaseForcefield = 12.0f;   
    constexpr float kSpiderBulletSpeed = 55.0f;  
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
        bool threat_is_player = false; 
        
        for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
            Player* p = ctx.bot->game->player_manager.players + i;
            if (p->ship >= 8 || p->frequency == self->frequency) continue;
            if (std::abs(p->position.y - center_y) > 100.0f) continue;

            if (p->position.Distance(puck_pos) < 3.0f) {
                threat_pos = p->position;
                threat_vel = p->velocity;
                threat_is_player = true; 
                break;
            }
        }

        // === SPIDER GAP CONTROL MATH & THE BITE ===
        float current_dist_to_threat = self->position.Distance(threat_pos);
        bool in_kill_zone = (current_dist_to_threat < DefenderConfig::kKillZoneRadius);
        
        // TWEAK: The Bite (Now strictly requires a player to trigger)
        bool is_biting = false;
        if (threat_is_player && threat_vel.Length() < 2.5f && current_dist_to_threat < 35.0f && self->energy > 200.0f) {
            is_biting = true;
        }

        float want_x = 0;
        float want_y = 0;

        if (is_biting) {
            want_x = threat_pos.x;
            want_y = threat_pos.y;
        } else {
            float gap = in_kill_zone ? DefenderConfig::kTightGapDistance : DefenderConfig::kGapDistance;
            
            want_x = defend_left 
                ? (puck_pos.x - gap) 
                : (puck_pos.x + gap);

            if (defend_left) {
                want_x = std::clamp(want_x, goal_line_x + DefenderConfig::kCreaseForcefield, 500.0f);
            } else {
                want_x = std::clamp(want_x, 524.0f, goal_line_x - DefenderConfig::kCreaseForcefield);
            }

            float predicted_y = threat_pos.y + (threat_vel.y * 0.30f);
            want_y = std::clamp(predicted_y, center_y - DefenderConfig::kMaxYOffset, center_y + DefenderConfig::kMaxYOffset);
        }

        Vector2f target(want_x, want_y);

        // === WEAPON SYSTEM & CALCULATED SNIPING ===
        static int bullet_cooldown = 0;
        static int bullet_hold_frames = 0;
        bool use_bullet = false;
        bool is_shooting_stance = false; 
        
        // TWEAK: Juke Dampener (0.85x on threat_vel to hit center-of-mass)
        Vector2f dampened_threat_vel = threat_vel * 0.85f;
        Vector2f relative_vel = dampened_threat_vel - self->velocity;
        
        // --- NEW CLOSING SPEED MATH ---
        Vector2f los = Normalize(threat_pos - self->position);
        
        // Calculate velocity vectors along the line-of-sight
        float our_closing = self->velocity.Dot(los);
        float threat_closing = dampened_threat_vel.Dot(los);
        
        // True bullet speed relative to the target's distance
        float closing_speed = DefenderConfig::kSpiderBulletSpeed + our_closing - threat_closing;
        
        // Safety floor to prevent division by zero or negative TTI if they outrun the bullet
        if (closing_speed < 15.0f) closing_speed = 15.0f; 
        
        // Calculate TTI using the true closing speed, then calculate aim point
        float time_to_impact = (current_dist_to_threat / closing_speed) + 0.025f;
        Vector2f aim_pos = threat_pos + (relative_vel * time_to_impact); 
        // ------------------------------

        if (bullet_cooldown > 0) bullet_cooldown--;

        if (bullet_hold_frames > 0) {
            use_bullet = true;
            is_shooting_stance = true;
            bullet_hold_frames--;
        } else {
            if ((current_dist_to_threat <= 13.0f || (is_biting && current_dist_to_threat <= 18.0f)) && bullet_cooldown <= 0 && self->energy > 135.0f) {
                is_shooting_stance = true; 
                
                Vector2f to_aim = Normalize(aim_pos - self->position);
                float aim_alignment = self->GetHeading().Dot(to_aim);
                
                // TWEAK: Wider trigger cone to prevent math from going stale while turning
                float required_alignment = (current_dist_to_threat < 8.0f || is_biting) ? 0.94f : 0.98f;
                
                if (aim_alignment > required_alignment) {
                    bullet_hold_frames = 3; 
                    bullet_cooldown = 150; 
                    Log(LogLevel::Info, "💥 [Defender] SNIPE! Aim:(%.1f, %.1f) | Dist: %.1f | Bite:%d | Align: %.3f", 
                        aim_pos.x, aim_pos.y, current_dist_to_threat, is_biting, aim_alignment);
                }
            }
        }

        // === FLIGHT DYNAMICS & VELOCITY CAPPING ===
        Vector2f to_target = target - self->position;
        float dist_to_target = to_target.Length();

        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
        bool use_ab = false;

        float braking_distance = 1.0f + (self->velocity.Length() * 0.15f);

        if (dist_to_target > braking_distance || (is_biting && dist_to_target > 2.0f)) {
            float max_speed = is_biting ? 55.0f : 30.0f;
            float desired_speed = std::min(max_speed, dist_to_target * 2.2f);
            Vector2f desired_velocity = Normalize(to_target) * desired_speed;
            
            float accel_multiplier = is_biting ? 3.0f : 1.2f;
            Vector2f force_needed = (desired_velocity - self->velocity) * accel_multiplier; 
            
            if (!in_kill_zone && !is_biting) {
                force_needed.y += (threat_vel.y * 1.2f); 
            }

            if (is_shooting_stance) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, aim_pos);
                
                if (self->velocity.Length() > 5.0f && !is_biting) {
                    ctx.bot->bot_controller->steering.force = -self->velocity * 2.0f;
                }
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + force_needed);
                
                float alignment = 1.0f;
                if (force_needed.LengthSq() > 0) {
                     alignment = self->GetHeading().Dot(Normalize(force_needed));
                }

                if (alignment < 0.2f && self->velocity.Length() > 5.0f) {
                    ctx.bot->bot_controller->steering.force = -self->velocity * 4.0f;
                } else if (std::abs(self->velocity.Length() - desired_speed) < 3.0f && alignment > 0.85f && !in_kill_zone && !is_biting) {
                    ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
                } else if (self->velocity.Length() > 16.0f && !in_kill_zone && !is_biting) {
                    ctx.bot->bot_controller->steering.force = -self->velocity * 2.0f;
                } else {
                    ctx.bot->bot_controller->steering.force = force_needed;
                    use_ab = ((in_kill_zone || is_biting) && self->energy > 800.0f && alignment > 0.8f); 
                }
            }
            
        } else {
            // ARRIVED AT CUSHION: Spider Anti-Slip Brakes
            Vector2f brake_force = -self->velocity * 6.0f;
            
            if (is_shooting_stance) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, aim_pos);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, threat_pos);
            }
            
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
            Log(LogLevel::Info, "[Defender] Target:(%.1f, %.1f) | Cushion:%.1f | Biting:%d | Speed:%.1f", 
                want_x, want_y, std::abs(puck_pos.x - self->position.x), is_biting, self->velocity.Length());
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
