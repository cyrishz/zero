#include "MLGoalieBehavior.h"

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

// Call to the generated ONNX C function in your brains/ directory
extern "C" void entry_bot_brain_MasterGoalie(const float* input, float* output);

namespace zero {
namespace hockeyzone {

namespace MLGoalieConfig {
    constexpr float kMapWidth = 1024.0f;
    constexpr float kUniversalGoalX = 613.0f; // Updated to match your V2.0 goal line
    constexpr float kCenterY[2] = { 511.5f, 736.0f }; // Multi-rink support
    
    // Master Scaler Values
    const float kScalerMean[4]  = { -22.86142f, -1.82922f, 28.32131f, -20.89723f };
    const float kScalerScale[4] = { 30.04631f, 16.35017f, 142.14405f, 134.46270f };
}

// =============================================================================
// ML DEFEND NODE (The Brains + V2.0 Actuation)
// =============================================================================
struct MLGoalieDefendNode : public behavior::BehaviorNode {
    MLGoalieDefendNode(const char* puck_key) : puck_key(puck_key) {}

    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
        if (!opt_puck) return behavior::ExecuteResult::Failure;
        
        Vector2f puck_pos = *opt_puck;
        float center_y = MLGoalieConfig::kCenterY[rink_index];

        if (std::abs(puck_pos.y - center_y) > 100.0f) {
            return behavior::ExecuteResult::Failure;
        }

        bool is_left_team = (self->frequency % 2 == 0);
        float goal_line_x = is_left_team ? 411.0f : MLGoalieConfig::kUniversalGoalX;

        // === PUCK VELOCITY CALCULATION (V2.0 Style) ===
        static Vector2f last_puck_pos(0, 0);
        if (last_puck_pos.LengthSq() == 0.0f) last_puck_pos = puck_pos;
        Vector2f puck_vel_tick = puck_pos - last_puck_pos;
        last_puck_pos = puck_pos;
        
        if (puck_vel_tick.LengthSq() > 100.0f) puck_vel_tick = Vector2f(0, 0);

        // Convert tick velocity to units/sec for the neural net
        float p_vx = puck_vel_tick.x * 100.0f;
        float p_vy = puck_vel_tick.y * 100.0f;
        float p_x = puck_pos.x;
        float p_y = puck_pos.y;

        // === 1. MIRRORING (Spatial Normalization) ===
        if (is_left_team) {
            p_x  = MLGoalieConfig::kMapWidth - p_x;
            p_vx = -p_vx;
        }

        // === 2. CALCULATE RELATIVE INPUTS ===
        float raw_inputs[4];
        raw_inputs[0] = p_x - MLGoalieConfig::kUniversalGoalX; // puck_rel_x
        raw_inputs[1] = p_y - center_y;                        // puck_rel_y
        raw_inputs[2] = p_vx;                                  // puck_vel_x
        raw_inputs[3] = p_vy;                                  // puck_vel_y

        // === 3. SCALE INPUTS ===
        float scaled_inputs[4];
        for (int i = 0; i < 4; i++) {
            scaled_inputs[i] = (raw_inputs[i] - MLGoalieConfig::kScalerMean[i]) / MLGoalieConfig::kScalerScale[i];
        }

        // === 4. RUN NEURAL NET ===
        float outputs[2] = {0.0f, 0.0f};
        entry_bot_brain_MasterGoalie(scaled_inputs, outputs);

        // === 5. TRANSLATE TO RIGHT-SIDE COORDINATES ===
        Vector2f target;
        target.y = center_y + outputs[0]; 
        target.x = MLGoalieConfig::kUniversalGoalX - std::abs(outputs[1]); 

        // === 6. MIRROR BACK TO REALITY ===
        if (is_left_team) {
            target.x = MLGoalieConfig::kMapWidth - target.x;
        }

        // === V2.0 FLIGHT DYNAMICS & PD CONTROLLER ===
        Vector2f to_target = target - self->position;
        float dist_to_target = to_target.Length();
        float y_dist_to_target = std::abs(target.y - self->position.y);

        bool is_moving = dist_to_target > 0.8f || y_dist_to_target > 0.5f;
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0);

        if (is_moving) {
            // Urgency check for Afterburner
            bool use_ab = (y_dist_to_target > 5.0f && self->energy > 600.0f);
            
            Vector2f final_force;
            if (use_ab) {
                // V2: MOMENTUM DAMPENING ON BURST
                final_force = Normalize(to_target) * 100.0f - (self->velocity * 0.5f);
                ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, true);
            } else {
                float desired_speed = std::min(50.0f, dist_to_target * 5.0f);
                Vector2f desired_velocity = Normalize(to_target) * desired_speed;
                
                // V2: HEAVY MOMENTUM DAMPENING ON DRIFT
                final_force = desired_velocity - (self->velocity * 1.35f);
                ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
            }

            if (final_force.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + final_force);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - final_force);
            }
            ctx.bot->bot_controller->steering.force = final_force;

        } else {
            // PLANTED - Brake hard
            Vector2f brake_force = -self->velocity * 4.0f; 
            
            if (brake_force.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + brake_force);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - brake_force);
            }
            ctx.bot->bot_controller->steering.force = brake_force; 
            ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
        }

        // === GLOBAL OVERRIDES ===
        Vector2f final_force = ctx.bot->bot_controller->steering.force;
        if (is_left_team && self->position.x < goal_line_x + 1.0f) {
            final_force.x += 35.0f; 
        } else if (!is_left_team && self->position.x > goal_line_x - 1.0f) {
            final_force.x -= 35.0f;
        }
        ctx.bot->bot_controller->steering.force = final_force;

        return behavior::ExecuteResult::Success;
    }

    const char* puck_key;
};

// =============================================================================
// ML CLEAR NODE (Re-used from V2.0)
// =============================================================================
struct MLGoalieClearNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) return behavior::ExecuteResult::Failure;

        bool defend_left = (self->frequency % 2 == 0);
        float center_y = MLGoalieConfig::kCenterY[rink_index];
        
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
// ML IDLE NODE (Re-used from V2.0)
// =============================================================================
struct MLGoalieIdleNode : public behavior::BehaviorNode {
    behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
        auto self = ctx.bot->game->player_manager.GetSelf();
        if (!self) return behavior::ExecuteResult::Failure;

        int rink_index = self->frequency / 2; 
        if (rink_index > 1) {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0);
            return behavior::ExecuteResult::Success; 
        }

        bool defend_left = (self->frequency % 2 == 0);
        float goal_line_x = defend_left ? 411.0f : MLGoalieConfig::kUniversalGoalX;
        float center_y = MLGoalieConfig::kCenterY[rink_index];
        
        // Idle roughly 7 tiles off the line
        float home_x = defend_left ? goal_line_x + 7.0f : goal_line_x - 7.0f;
            
        Vector2f home(home_x, center_y);
        Vector2f to_home = home - self->position;
        float dist_to_home = to_home.Length();

        if (dist_to_home > 2.5f) {
            float desired_speed = std::min(40.0f, dist_to_home * 4.0f);
            Vector2f desired_velocity = Normalize(to_home) * desired_speed;
            Vector2f force_needed = desired_velocity - self->velocity;
            
            if (force_needed.y <= 0) {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position + force_needed);
            } else {
                ctx.bot->bot_controller->steering.Face(*ctx.bot->game, self->position - force_needed);
            }
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
// BEHAVIOR TREE WIRING
// =============================================================================
std::unique_ptr<behavior::BehaviorNode> MLGoalieBehavior::CreateTree(behavior::ExecuteContext& ctx) {
    using namespace behavior;
    BehaviorBuilder builder;

    builder
        .Selector()
            // 1. SHIP REQUEST (Lancaster)
            .Sequence()
                .Child<ExecuteNode>([](ExecuteContext& ctx) {
                    auto self = ctx.bot->game->player_manager.GetSelf();
                    if (!self) return ExecuteResult::Failure;
                    return self->ship >= 8 ? ExecuteResult::Success : ExecuteResult::Failure;
                })
                .Child<ShipRequestNode>(7)
                .End()

            .Selector()
                // 2. CLEAR THE PUCK (If holding it)
                .Sequence()
                    .Child<PowerballCarryQueryNode>()
                    .Child<MLGoalieClearNode>()
                    .End()

                // 3. NEURAL NET DEFEND (If puck is nearby)
                .Sequence()
                    .InvertChild<PowerballCarryQueryNode>()
                    .Child<PowerballClosestQueryNode>("puck_position", true)
                    .Child<MLGoalieDefendNode>("puck_position")
                    .End()

                // 4. IDLE AT HOME
                .Child<MLGoalieIdleNode>()
                .End()
        .End();

    return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
