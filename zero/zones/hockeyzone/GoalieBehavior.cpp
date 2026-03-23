/*
    ZERO BOT - GOALIE BEHAVIOR (V2.0 UNSCORABLE ARCHITECTURE)
    
    BASE BEHAVIOR FOCUS:
    - Pure Elliptical Arc mirroring
    - Dynamic Angle Challenge 
    - Velocity Mirroring 
    
    V2.0 UPGRADES (THE INEVITABILITY UPDATE):
    - Shot Imminent State (Pre-shot posture, reads settling players)
    - Preloaded Kick (Micro-bursts before release, commits on shot)
    - Intercept Lane Blending (Cuts off X/Y path, doesn't just track Y)
    - Hard "No Goal" Wall Logic (Absolute Y-alignment enforcement)
    - Momentum Dampening (PD Controller eliminates overshoot/jitter)
    - Cross-Net Panic & Aggressive Slot Threat weights
*/

#include "GoalieBehavior.h"

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
// CONFIGURATION (MULTI-RINK SUPPORT)
// =============================================================================
namespace GoalieConfig {
    constexpr float kLeftGoalLineX = 411.0f;
    constexpr float kRightGoalLineX = 613.0f; 
    
    constexpr float kCenterY[2] = { 511.5f, 736.0f }; 

    constexpr float kPostOffset = 13.5f;    
    
    constexpr float kMinDepth = 2.0f;       
    constexpr float kMaxArcDepth = 7.0f;    
    
    constexpr float kChallengeMinDepth = 5.0f;       
    constexpr float kChallengeMaxArcDepth = 15.0f;   
    
    constexpr float kChallengeStartDist = 35.0f;     
    constexpr float kChallengeRetreatDist = 18.0f;   

    constexpr float kDeepCornerThreshold = 28.0f;    
    constexpr float kAnchorDepth = 3.0f;             
    
    constexpr float kNorthAnchorYOffset = -12.0f;     
    constexpr float kSouthAnchorYOffset = 12.0f;     
    constexpr float kWrapAroundSealDepth = 1.0f;     
    
    constexpr float kDriveToCenterVelocityThreshold = 12.0f; 
}

// Vector math helper for blending
inline Vector2f LerpVector(const Vector2f& a, const Vector2f& b, float t) {
    return Vector2f(std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t));
}

// =============================================================================
// THE GOALIE
// =============================================================================
struct GoalieDefendNode : public behavior::BehaviorNode {
    GoalieDefendNode(const char* puck_key) : puck_key(puck_key) {}

    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
        if (!opt_puck) return behavior::ExecuteResult::Failure;
        
        Vector2f puck_pos = *opt_puck;
        float center_y = GoalieConfig::kCenterY[rink_index];

        if (std::abs(puck_pos.y - center_y) > 100.0f) {
            return behavior::ExecuteResult::Failure;
        }

        bool defend_left = (self->frequency % 2 == 0);
        float goal_line_x = defend_left ? GoalieConfig::kLeftGoalLineX : GoalieConfig::kRightGoalLineX;
        
        float top_post_y = center_y - GoalieConfig::kPostOffset;
        float bottom_post_y = center_y + GoalieConfig::kPostOffset;

        float track_top_y = center_y + GoalieConfig::kNorthAnchorYOffset;
        float track_bottom_y = center_y + GoalieConfig::kSouthAnchorYOffset;

        static int explosion_ticks = 0;
        if (explosion_ticks > 0) explosion_ticks--;

        // === TELEMETRY: GOAL DETECTOR ===
        static bool goal_scored_recently = false;
        bool puck_in_net = false;
        
        if (defend_left && puck_pos.x < goal_line_x && puck_pos.y > top_post_y && puck_pos.y < bottom_post_y) {
            puck_in_net = true;
        } else if (!defend_left && puck_pos.x > goal_line_x && puck_pos.y > top_post_y && puck_pos.y < bottom_post_y) {
            puck_in_net = true;
        }

        if (puck_in_net && !goal_scored_recently) {
            Log(LogLevel::Info, "[GoalieMetrics] 🚨 GOAL ALLOWED | Puck:(%.1f, %.1f) | Goalie:(%.1f, %.1f) | DistFromCenter: %.1f", 
                puck_pos.x, puck_pos.y, self->position.x, self->position.y, std::abs(self->position.y - center_y));
            goal_scored_recently = true; 
        } else if (!puck_in_net && puck_pos.x > 450.0f && puck_pos.x < 574.0f) {
            goal_scored_recently = false; 
        }

        // === FIND THE THREAT & SLOT RECEIVERS ===
        Vector2f threat_pos = puck_pos;
        Vector2f threat_vel(0, 0);
        float dist_to_puck_carrier = 999.0f;
        
        Vector2f secondary_threat_pos(0,0);
        float highest_slot_danger = 0.0f;
        
        for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
            Player* p = ctx.bot->game->player_manager.players + i;
            if (p->ship >= 8 || p->frequency == self->frequency) continue;
            if (std::abs(p->position.y - center_y) > 100.0f) continue;

            float dist = p->position.Distance(puck_pos);
            if (dist < 3.0f) {
                dist_to_puck_carrier = dist;
                threat_pos = p->position;
                threat_vel = p->velocity;
            } else {
                float dist_to_net = std::abs(p->position.x - goal_line_x);
                float y_offset_from_center = std::abs(p->position.y - center_y);
                
                if (dist_to_net < 25.0f && y_offset_from_center < 20.0f) {
                    float danger_score = 100.0f / (dist_to_net + y_offset_from_center + 1.0f);
                    if (danger_score > highest_slot_danger) {
                        highest_slot_danger = danger_score;
                        secondary_threat_pos = p->position;
                    }
                }
            }
        }

        // === DYNAMIC VELOCITY ANTICIPATION ===
        static Vector2f last_puck_pos(0, 0);
        if (last_puck_pos.LengthSq() == 0.0f) last_puck_pos = puck_pos;
        Vector2f puck_vel_tick = puck_pos - last_puck_pos;
        last_puck_pos = puck_pos;
        
        if (puck_vel_tick.LengthSq() > 100.0f) puck_vel_tick = Vector2f(0, 0);

        float predicted_y = threat_pos.y;
        float puck_speed = puck_vel_tick.Length() * 100.0f; 
        bool heading_to_net = defend_left ? (puck_vel_tick.x < -0.001f) : (puck_vel_tick.x > 0.001f);
        
        // V2: LIVE SHOT VS IMMINENT SHOT
        bool is_live_shot = (dist_to_puck_carrier > 1.5f && heading_to_net && puck_speed > 15.0f);
        
        float threat_dist_to_center_x = std::abs(threat_pos.x - goal_line_x);
        bool shot_imminent = (dist_to_puck_carrier < 3.0f && 
                              threat_vel.Length() < 2.0f && 
                              std::abs(threat_pos.y - center_y) < 25.0f && 
                              threat_dist_to_center_x < 30.0f);

        // V2: PRELOAD & KICK TIMERS + CROSS-NET PANIC
        static float last_dist_to_carrier = 0.0f;
        static bool shot_in_progress = false;

        if (is_live_shot && !shot_in_progress) {
            float goalie_depth = std::abs(self->position.x - goal_line_x);
            Log(LogLevel::Info, "[GoalieMetrics] 🏒 SHOT DETECTED | Puck:(%.1f, %.1f) | Vel:(%.2f, %.2f) | Goalie:(%.1f, %.1f) | Depth: %.1f", 
                puck_pos.x, puck_pos.y, puck_vel_tick.x * 100.0f, puck_vel_tick.y * 100.0f, self->position.x, self->position.y, goalie_depth);
            
            shot_in_progress = true;
            explosion_ticks = 12; // V2: Full commit explosion
            
        } else if (shot_imminent && explosion_ticks == 0 && !is_live_shot) {
            explosion_ticks = 6; // V2: Preload micro-burst
        } else if (!is_live_shot) {
            shot_in_progress = false;
        }
        
        // V2: CROSS-NET PANIC OVERRIDE
        bool cross_net = std::abs(puck_vel_tick.y) > 2.5f && is_live_shot;
        if (cross_net) {
            explosion_ticks = std::max(explosion_ticks, 10);
        }
        
        last_dist_to_carrier = dist_to_puck_carrier;

        // V2: TRUE INTERCEPT PREDICTION + AGGRESSIVE SLOT WEIGHT
        if (is_live_shot || shot_imminent) {
            float velocity_to_use_x = is_live_shot ? puck_vel_tick.x : threat_vel.x;
            float velocity_to_use_y = is_live_shot ? puck_vel_tick.y : threat_vel.y;
            
            if (std::abs(velocity_to_use_x) > 0.01f) {
                float t = (goal_line_x - puck_pos.x) / velocity_to_use_x;
                if (t > 0 && t < 150.0f) {
                    predicted_y = puck_pos.y + velocity_to_use_y * t;
                }
            }
        } else {
            float threat_speed = threat_vel.Length();
            float prediction_time = std::clamp(0.10f + (threat_speed * 0.01f), 0.10f, 0.35f);
            predicted_y = threat_pos.y + threat_vel.y * prediction_time;
            
            // V2: AGGRESSIVE SLOT OVERRIDE
            if (highest_slot_danger > 5.0f && dist_to_puck_carrier < 3.0f) {
                predicted_y = std::lerp(predicted_y, secondary_threat_pos.y, 0.45f); // Increased to 0.45f
            }
        }

        // === THE HYBRID CLAMP (THE LUNGE) ===
        float want_y;
        if (is_live_shot || shot_imminent || dist_to_puck_carrier > 3.0f) {
            want_y = std::clamp(predicted_y, top_post_y, bottom_post_y);
        } else {
            want_y = std::clamp(predicted_y, track_top_y, track_bottom_y);
        }

        // === DYNAMIC ANGLE CHALLENGE (RED TO BLUE LINE) ===
        float threat_dist_x = 0.0f;
        bool is_behind_net = false;
        
        if (defend_left) {
            threat_dist_x = threat_pos.x - goal_line_x;
            if (threat_pos.x < goal_line_x) is_behind_net = true;
        } else {
            threat_dist_x = goal_line_x - threat_pos.x;
            if (threat_pos.x > goal_line_x) is_behind_net = true;
        }
        
        float challenge_factor = std::clamp(
            (threat_dist_x - GoalieConfig::kChallengeRetreatDist) / 
            (GoalieConfig::kChallengeStartDist - GoalieConfig::kChallengeRetreatDist), 
            0.0f, 1.0f
        );

        float current_min_depth = GoalieConfig::kMinDepth + 
            (GoalieConfig::kChallengeMinDepth - GoalieConfig::kMinDepth) * challenge_factor;
        float current_max_depth = GoalieConfig::kMaxArcDepth + 
            (GoalieConfig::kChallengeMaxArcDepth - GoalieConfig::kMaxArcDepth) * challenge_factor;

        float dy = want_y - center_y;
        float arc_ratio = std::sqrt(std::max(0.0f, 1.0f - (dy * dy) / (GoalieConfig::kPostOffset * GoalieConfig::kPostOffset)));
        float depth = current_min_depth + (current_max_depth - current_min_depth) * arc_ratio;
        
        float want_x = defend_left ? (goal_line_x + depth) : (goal_line_x - depth);
        
        if (defend_left) {
            want_x = std::max(want_x, goal_line_x + 1.5f);
        } else {
            want_x = std::min(want_x, goal_line_x - 1.5f);
        }

        // === DEEP CORNER & WRAP-AROUND ANCHORS ===
        bool driving_out_of_north = (threat_pos.y < center_y && threat_vel.y > GoalieConfig::kDriveToCenterVelocityThreshold);
        bool driving_out_of_south = (threat_pos.y > center_y && threat_vel.y < -GoalieConfig::kDriveToCenterVelocityThreshold);
        bool driving_to_center = driving_out_of_north || driving_out_of_south;

        if (!is_live_shot && !driving_to_center) {
            if (is_behind_net && std::abs(threat_pos.y - center_y) < GoalieConfig::kDeepCornerThreshold + 10.0f) {
                want_x = defend_left ? (goal_line_x + GoalieConfig::kWrapAroundSealDepth) : (goal_line_x - GoalieConfig::kWrapAroundSealDepth);
                want_y = threat_pos.y < center_y ? top_post_y + 1.0f : bottom_post_y - 1.0f; 
            } 
            else if (threat_pos.y < center_y - GoalieConfig::kDeepCornerThreshold) {
                want_x = defend_left ? (goal_line_x + GoalieConfig::kAnchorDepth) : (goal_line_x - GoalieConfig::kAnchorDepth);
                want_y = top_post_y + 1.0f;
            } else if (threat_pos.y > center_y + GoalieConfig::kDeepCornerThreshold) {
                want_x = defend_left ? (goal_line_x + GoalieConfig::kAnchorDepth) : (goal_line_x - GoalieConfig::kAnchorDepth);
                want_y = bottom_post_y - 1.0f;
            }
        }

        // V2: INTERCEPT LANE BLENDING (THE BIG UPGRADE)
        Vector2f base_target(want_x, want_y);
        Vector2f lane_target(goal_line_x, predicted_y); // Pure geometric blockage point
        
        float lane_blend = is_live_shot ? 0.65f : (shot_imminent ? 0.25f : 0.0f);
        Vector2f target = LerpVector(base_target, lane_target, lane_blend);

        // V2: HARD "NO GOAL" WALL
        if (is_live_shot) {
            float max_y_error = 2.5f;
            if (std::abs(self->position.y - predicted_y) > max_y_error) {
                target.y = predicted_y; // Absolute Y-alignment override
            }
        }

        // === FLIGHT DYNAMICS ===
        Vector2f to_target = target - self->position;
        float dist_to_target = to_target.Length();
        float y_dist_to_target = std::abs(target.y - self->position.y);

        if (explosion_ticks > 0 && dist_to_target < 0.5f) {
            explosion_ticks = 0; 
        }

        bool is_moving = ctx.blackboard.ValueOr<bool>("defend_is_moving", false);

        if (is_live_shot || shot_imminent || y_dist_to_target > 0.5f || dist_to_target > 0.8f) {
            is_moving = true;  
        } else if (!is_live_shot && !shot_imminent && dist_to_target < 0.2f) {
            is_moving = false; 
        }

        ctx.blackboard.Set<bool>("defend_is_moving", is_moving);
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
        bool use_ab = false;

        if (is_moving) {
            // V2: URGENCY-BASED AB TRIGGER
            if (is_live_shot || shot_imminent) {
                use_ab = (y_dist_to_target > 2.0f && self->energy > 250.0f) || (explosion_ticks > 0 && self->energy > 200.0f);
            } else {
                use_ab = (y_dist_to_target > 5.0f && self->energy > 600.0f);
            }

            Vector2f final_force;
            if (use_ab) {
                // V2: MOMENTUM DAMPENING (PD CONTROLLER) ON BURST
                final_force = Normalize(to_target) * 100.0f - (self->velocity * 0.5f);
            } else {
                float desired_speed = std::min(50.0f, dist_to_target * 5.0f);
                Vector2f desired_velocity = Normalize(to_target) * desired_speed;
                
                // V2: HEAVY MOMENTUM DAMPENING ON DRIFT
                final_force = desired_velocity - (self->velocity * 1.35f);
                
                if (!is_live_shot && !shot_imminent) {
                    final_force.y += (threat_vel.y * 0.40f); 
                }
            }

            if (final_force.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + final_force);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - final_force);
            }
            ctx.bot->bot_controller->steering.force = final_force;

        } else {
            // PLANTED 
            Vector2f brake_force = -self->velocity * 4.0f; 
            
            if (brake_force.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + brake_force);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - brake_force);
            }
            ctx.bot->bot_controller->steering.force = brake_force; 
        }

        // === GLOBAL OVERRIDES ===
        Vector2f final_force = ctx.bot->bot_controller->steering.force;
        if (defend_left && self->position.x < goal_line_x + 1.0f) {
            final_force.x += 35.0f; 
        } else if (!defend_left && self->position.x > goal_line_x - 1.0f) {
            final_force.x -= 35.0f;
        }
        ctx.bot->bot_controller->steering.force = final_force;

        static int bullet_cooldown = 0;
        bool use_bullet = false;
        
        if (bullet_cooldown > 0) {
            bullet_cooldown--;
        }
        
        float current_dist_to_threat = self->position.Distance(threat_pos);
        if (current_dist_to_threat <= 2.5f && bullet_cooldown <= 0 && self->energy > 500.0f) {
            use_bullet = true;
            bullet_cooldown = 150; 
        }

        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, use_ab);
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, use_bullet);
        }

        return behavior::ExecuteResult::Success;
    }

    const char* puck_key;
};

// =============================================================================
// Smart Clear puck when we have it
// =============================================================================
struct GoalieClearNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        bool defend_left = (self->frequency % 2 == 0);
        float center_y = GoalieConfig::kCenterY[rink_index];
        
        float safe_y_offset = self->position.y > center_y ? 25.0f : -25.0f;
        
        for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
            Player* p = ctx.bot->game->player_manager.players + i;
            if (p->ship >= 8 || p->frequency == self->frequency) continue;
            
            float dist = p->position.Distance(self->position);
            if (dist < 15.0f) {
                if (p->position.y > center_y) {
                    safe_y_offset = -25.0f;
                } else {
                    safe_y_offset = 25.0f;
                }
            }
        }
        
        Vector2f clear_target;
        clear_target.x = defend_left ? 480.0f : 543.0f;
        clear_target.y = center_y + safe_y_offset;

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

        if (alignment > 0.95f && self->velocity.Length() < 4.0f) {
            ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
            return behavior::ExecuteResult::Success;
        }

        return behavior::ExecuteResult::Running;
    }
};

// =============================================================================
// Idle at home
// =============================================================================
struct GoalieIdleNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
            return behavior::ExecuteResult::Success; 
        }

        bool defend_left = (self->frequency % 2 == 0);
        float goal_line_x = defend_left ? GoalieConfig::kLeftGoalLineX : GoalieConfig::kRightGoalLineX;
        float center_y = GoalieConfig::kCenterY[rink_index];
        
        float home_x = defend_left 
            ? goal_line_x + GoalieConfig::kMaxArcDepth
            : goal_line_x - GoalieConfig::kMaxArcDepth;
            
        Vector2f home(home_x, center_y);
        
        Vector2f to_home = home - self->position;
        float dist_to_home = to_home.Length();

        bool is_moving = ctx.blackboard.ValueOr<bool>("idle_is_moving", false);

        if (dist_to_home > 2.5f) {
            is_moving = true;
        } else if (dist_to_home < 0.5f) {
            is_moving = false;
        }

        ctx.blackboard.Set<bool>("idle_is_moving", is_moving);
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);

        if (is_moving) {
            float desired_speed = std::min(40.0f, dist_to_home * 4.0f);
            Vector2f desired_velocity = Normalize(to_home) * desired_speed;
            Vector2f force_needed = desired_velocity - self->velocity;
            
            if (force_needed.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + force_needed);
                ctx.bot->bot_controller->steering.force = force_needed;
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - force_needed);
                ctx.bot->bot_controller->steering.force = force_needed;
            }
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
std::unique_ptr<behavior::BehaviorNode> GoalieBehavior::CreateTree(behavior::ExecuteContext& ctx) {
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
                    .Child<GoalieClearNode>()
                    .End()

                .Sequence()
                    .InvertChild<PowerballCarryQueryNode>()
                    .Child<PowerballClosestQueryNode>("puck_position", true)
                    .Child<GoalieDefendNode>("puck_position")
                    .End()

                .Child<GoalieIdleNode>()
                .End()
        .End();

    return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
