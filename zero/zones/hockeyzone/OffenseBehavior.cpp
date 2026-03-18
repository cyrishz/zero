#include "OffenseBehavior.h"

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
#include <zero/zones/hockey/HockeyZone.h>
#include <zero/zones/hockey/nodes/PowerballNode.h>
#include <zero/zones/hockey/nodes/RinkNode.h>
#include <zero/zones/svs/nodes/DynamicPlayerBoundingBoxQueryNode.h>
#include <zero/zones/trenchwars/nodes/MoveNode.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace zero {
namespace hockeyzone {
using namespace zero::hz; 

namespace HockeyConfig {
  constexpr float kLeftGoalX = 408.0f;
  constexpr float kRightGoalX = 615.0f;
  
  constexpr float kGoalTopPostY = 502.0f;
  constexpr float kGoalBottomPostY = 521.0f;
  
  constexpr float kLeftCreaseMinX = 405.0f;
  constexpr float kLeftCreaseMaxX = 419.0f;
  constexpr float kLeftCreaseMinY = 498.0f;
  constexpr float kLeftCreaseMaxY = 525.0f;
  
  constexpr float kRightCreaseMinX = 604.0f;
  constexpr float kRightCreaseMaxX = 618.0f;
  constexpr float kRightCreaseMinY = 498.0f;
  constexpr float kRightCreaseMaxY = 525.0f;
  
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

namespace {

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

struct FindGiveAndGoTeammateNode : public behavior::BehaviorNode {
  FindGiveAndGoTeammateNode(const char* pass_target_id_key, const char* enemy_goal_key) 
      : pass_target_id_key(pass_target_id_key), enemy_goal_key(enemy_goal_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_goal = ctx.blackboard.Value<Vector2f>(enemy_goal_key);
    if (!self || !opt_goal) return behavior::ExecuteResult::Failure;

    float my_speed = self->velocity.Length();
    if (my_speed > 8.0f) return behavior::ExecuteResult::Failure;

    Player* bomber = nullptr;
    float best_bomber_score = -1.0f;

    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
      Player* p = ctx.bot->game->player_manager.players + i;
      if (p->id == self->id || p->frequency != self->frequency || p->ship >= 8) continue;

      float their_speed = p->velocity.Length();
      
      if (their_speed < 14.0f) continue;

      Vector2f to_goal = Normalize(*opt_goal - p->position);
      Vector2f their_heading = Normalize(p->velocity);
      float goal_alignment = their_heading.Dot(to_goal);

      if (goal_alignment < 0.5f) continue; 

      if (!IsLaneClear(*ctx.bot->game, self->position, p->position, self->frequency)) continue;

      float score = their_speed * goal_alignment;
      if (score > best_bomber_score) {
          best_bomber_score = score;
          bomber = p;
      }
    }

    if (bomber) {
      ctx.blackboard.Set<u16>(pass_target_id_key, bomber->id);
      Log(LogLevel::Info, "GIVE AND GO INITIATED! Target: %s (Speed: %.1f)", bomber->name, bomber->velocity.Length());
      return behavior::ExecuteResult::Success;
    }

    return behavior::ExecuteResult::Failure;
  }
  const char* pass_target_id_key;
  const char* enemy_goal_key;
};

struct FindOpenTeammateNode : public behavior::BehaviorNode {
  FindOpenTeammateNode(const char* pass_target_id_key, const char* enemy_goal_key) 
      : pass_target_id_key(pass_target_id_key), enemy_goal_key(enemy_goal_key) {}

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
      ctx.blackboard.Set<u16>(pass_target_id_key, best_teammate->id);
      return behavior::ExecuteResult::Success;
    }

    return behavior::ExecuteResult::Failure;
  }
  const char* pass_target_id_key;
  const char* enemy_goal_key;
};

struct HockeyOffBallStateNode : public behavior::BehaviorNode {
  HockeyOffBallStateNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    Player* carrier = nullptr;
    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
        Player* p = ctx.bot->game->player_manager.players + i;
        if (p->ship >= 8 || p->id == self->id) continue;
        if (p->position.Distance(*opt_puck) < 3.0f) {
            carrier = p;
            break;
        }
    }

    int state = 0; 
    if (carrier) {
        if (carrier->frequency == self->frequency) state = 1;
        else state = 2;
    }

    ctx.blackboard.Set<int>("tactical_state", state);
    return behavior::ExecuteResult::Success;
  }
  const char* puck_key;
};

struct HockeySlotPositionNode : public behavior::BehaviorNode {
  HockeySlotPositionNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
    }

    bool attacking_right = (self->frequency == 0);
    float target_y = std::clamp(opt_puck->y, 503.0f, 532.0f);
    
    Vector2f slot_pos;
    Vector2f enemy_goal;

    if (attacking_right) {
        slot_pos = Vector2f(585.0f, target_y);
        enemy_goal = Vector2f(HockeyConfig::kRightGoalX, HockeyConfig::kCenterY);
    } else {
        slot_pos = Vector2f(435.0f, target_y);
        enemy_goal = Vector2f(HockeyConfig::kLeftGoalX, HockeyConfig::kCenterY);
    }

    float dist_to_slot = self->position.Distance(slot_pos);
    float speed = self->velocity.Length();

    // TAZ FIX: Utilizing Seek to prevent overshooting/orbiting the slot
    if (dist_to_slot < 6.0f) {
        if (speed > 1.0f) {
            ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 15.0f;
        } else {
            ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
        }
        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, enemy_goal); 
    } else {
        ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, slot_pos);
        ctx.bot->bot_controller->steering.Face(*ctx.bot->game, slot_pos);
    }

    return behavior::ExecuteResult::Success;
  }
  const char* puck_key;
};

struct HockeySeekAndDestroyNode : public behavior::BehaviorNode {
  HockeySeekAndDestroyNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    Player* carrier = nullptr;
    for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
        Player* p = ctx.bot->game->player_manager.players + i;
        if (p->ship >= 8 || p->id == self->id) continue;
        if (p->position.Distance(*opt_puck) < 3.0f && p->frequency != self->frequency) {
            carrier = p;
            break;
        }
    }

    if (!carrier) return behavior::ExecuteResult::Failure;

    float dist = self->position.Distance(carrier->position);
    
    // TAZ FIX: Pursue logic! Extrapolate enemy position to intercept them
    float intercept_time = dist / 22.0f; // Assumed bot intercept speed
    Vector2f intercept_pos = carrier->position + (carrier->velocity * intercept_time);
    
    // Seek the predicted interception point to prevent trailing them
    ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, intercept_pos);
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, carrier->position);

    Vector2f heading = self->GetHeading();
    Vector2f to_enemy = carrier->position - self->position;
    
    if (dist <= 20.0f && heading.Dot(Normalize(to_enemy)) > 0.85f) {
        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, true);
        }
    } else {
        if (ctx.bot->bot_controller->input) {
            ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        }
    }

    return behavior::ExecuteResult::Success;
  }
  const char* puck_key;
};

struct HockeyLoosePuckChaseNode : public behavior::BehaviorNode {
  HockeyLoosePuckChaseNode(const char* puck_key) : puck_key(puck_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    auto opt_puck = ctx.blackboard.Value<Vector2f>(puck_key);
    if (!self || !opt_puck) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
    }

    Vector2f puck_pos = *opt_puck;
    float dist = self->position.Distance(puck_pos);
    float speed = self->velocity.Length();

    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, puck_pos);

    if (dist < 4.0f && speed > 5.0f) {
        // Hard brake to ensure clean pickup without overshooting
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 15.0f;
    } else {
        // TAZ FIX: Utilizing Seek to completely eliminate the "orbiting" effect!
        ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, puck_pos);
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
        ctx.bot->bot_controller->input->SetAction(InputAction::Afterburner, false);
    }

    bool attacking_right = (self->frequency == 0);
    Vector2f target = *opt_target;
    
    if (attacking_right) {
        if (self->position.x > HockeyConfig::kRightGoalX - 2.0f) {
            target.x = HockeyConfig::kRightGoalX - 15.0f; 
            target.y = (self->position.y < HockeyConfig::kCenterY) ? 485.0f : 545.0f; 
        }
    } else {
        if (self->position.x < HockeyConfig::kLeftGoalX + 2.0f) {
            target.x = HockeyConfig::kLeftGoalX + 15.0f; 
            target.y = (self->position.y < HockeyConfig::kCenterY) ? 485.0f : 545.0f; 
        }
    }
    
    float dist = self->position.Distance(target);
    float speed = self->velocity.Length();

    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, target); 
    
    // TAZ FIX: Use Seek for transit, apply brakes when close to the final zone
    if (dist < 10.0f && speed > 5.0f) {
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 15.0f;
    } else {
        ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, target);
    }

    return behavior::ExecuteResult::Success;
  }
  const char* target_key;
};

struct OffenseGoalQueryNode : public behavior::BehaviorNode {
  OffenseGoalQueryNode(const char* output_key, bool get_enemy_goal = true) 
      : output_key(output_key), get_enemy_goal(get_enemy_goal) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    u16 freq = self->frequency;
    Vector2f goal_pos;
    
    bool attacking_right = (get_enemy_goal && freq == 0) || (!get_enemy_goal && freq != 0);
    
    float inner_top_y = 504.0f;
    float inner_bottom_y = 529.0f;
    
    float target_y;
    
    if (self->velocity.y > 1.5f) {
        target_y = inner_bottom_y; 
    } else if (self->velocity.y < -1.5f) {
        target_y = inner_top_y;
    } else {
        target_y = (self->position.y < HockeyConfig::kCenterY) ? inner_bottom_y : inner_top_y;
    }

    if (attacking_right) {
      goal_pos = Vector2f(615.0f, target_y); 
    } else {
      goal_pos = Vector2f(408.0f, target_y); 
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
      if (self->position.y < HockeyConfig::kCenterY) {
          shoot_pos = Vector2f(610.0f, 545.0f);
      } else {
          shoot_pos = Vector2f(610.0f, 480.0f);
      }
    } else { 
      if (self->position.y < HockeyConfig::kCenterY) {
          shoot_pos = Vector2f(412.0f, 545.0f);
      } else {
          shoot_pos = Vector2f(412.0f, 480.0f);
      }
    }
    
    ctx.blackboard.Set<Vector2f>(output_key, shoot_pos);
    return behavior::ExecuteResult::Success;
  }

  const char* output_key;
};

struct DynamicAimAndShootNode : public behavior::BehaviorNode {
  DynamicAimAndShootNode(const char* target_key, bool is_player = false) 
    : target_key(target_key), is_player(is_player) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    if (ctx.bot->bot_controller->input) {
        ctx.bot->bot_controller->input->SetAction(InputAction::Bullet, false);
    }

    Vector2f target_pos;
    
    if (is_player) {
        auto opt_id = ctx.blackboard.Value<u16>(target_key);
        if (!opt_id) return behavior::ExecuteResult::Failure;
        
        Player* p = nullptr;
        for (size_t i = 0; i < ctx.bot->game->player_manager.player_count; ++i) {
            if (ctx.bot->game->player_manager.players[i].id == *opt_id) {
                p = ctx.bot->game->player_manager.players + i;
                break;
            }
        }
        
        if (!p || p->ship >= 8) return behavior::ExecuteResult::Failure; 
        
        float dist_to_teammate = self->position.Distance(p->position);
        float estimated_puck_speed = 65.0f; 
        float lead_time = dist_to_teammate / estimated_puck_speed;
        
        Vector2f lead_vector = p->velocity * lead_time;
        if (lead_vector.LengthSq() > 6.25f) { 
            lead_vector = Normalize(lead_vector) * 2.5f;
        }
        target_pos = p->position + lead_vector;
    } else {
        auto opt_goal = ctx.blackboard.Value<Vector2f>(target_key);
        if (!opt_goal) return behavior::ExecuteResult::Failure;
        target_pos = *opt_goal;
    }

    Vector2f my_pos = self->position;
    Vector2f to_target = target_pos - my_pos;
    float dist = to_target.Length();
    float speed = self->velocity.Length();
    
    float assumed_puck_speed = 65.0f; 
    float time_to_target = dist / assumed_puck_speed;
    Vector2f compensated_aim = target_pos - (self->velocity * time_to_target);
    
    ctx.bot->bot_controller->steering.Face(*ctx.bot->game, compensated_aim);
    
    Vector2f heading = self->GetHeading();
    Vector2f aim_dir = Normalize(compensated_aim - my_pos);
    float alignment = heading.Dot(aim_dir);
    
    Log(LogLevel::Debug, "Aim: pos=(%.1f,%.1f) tgt=(%.1f,%.1f) comp=(%.1f,%.1f) align=%.3f spd=%.1f",
        my_pos.x, my_pos.y, target_pos.x, target_pos.y, compensated_aim.x, compensated_aim.y, alignment, speed);
    
    float required_alignment = 0.96f; 
    
    if (!is_player && dist > 53.0f) {
        required_alignment = 0.985f; 
    }

    if (is_player) {
        required_alignment = 0.98f; 
    }

    float max_speed = is_player ? 15.0f : 16.0f; 

    if (speed > max_speed) {
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 20.0f;
    } else if (alignment < 0.9f && speed > 5.0f) {
        ctx.bot->bot_controller->steering.force = -Normalize(self->velocity) * 15.0f;
    } else {
        ctx.bot->bot_controller->steering.force = Vector2f(0, 0); 
    }
    
    if (alignment >= required_alignment) {
      if (is_player) {
          Log(LogLevel::Info, "SNIPING PASS! Tgt=(%.1f,%.1f) Comp_Aim=(%.1f,%.1f) align=%.3f spd=%.1f", 
              target_pos.x, target_pos.y, compensated_aim.x, compensated_aim.y, alignment, speed);
      } else {
          Log(LogLevel::Info, "HAWK-DIVE SHOT! Tgt=(%.1f,%.1f) Comp_Aim=(%.1f,%.1f) align=%.3f spd=%.1f", 
              target_pos.x, target_pos.y, compensated_aim.x, compensated_aim.y, alignment, speed);
      }
      ctx.bot->game->soccer.FireBall(BallFireMethod::Gun);
      return behavior::ExecuteResult::Success;
    }
    
    return behavior::ExecuteResult::Running;
  }

  const char* target_key;
  bool is_player;
};

struct HockeyInShootingRangeNode : public behavior::BehaviorNode {
  HockeyInShootingRangeNode(const char* target_key) : target_key(target_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self) return behavior::ExecuteResult::Failure;

    if (IsInCrease(self->position)) return behavior::ExecuteResult::Failure;
    
    auto opt_target = ctx.blackboard.Value<Vector2f>(target_key);
    if (!opt_target) return behavior::ExecuteResult::Failure;
    
    Vector2f target = *opt_target;
    bool attacking_right = (self->frequency == 0);
    bool in_kill_box = false;

    if (attacking_right) {
        if (self->position.x >= 560.0f && self->position.x <= 620.0f &&
            self->position.y >= 485.0f && self->position.y <= 545.0f) {
            in_kill_box = true;
        }
    } else {
        if (self->position.x >= 405.0f && self->position.x <= 465.0f &&
            self->position.y >= 485.0f && self->position.y <= 545.0f) {
            in_kill_box = true;
        }
    }
    
    if (!in_kill_box) return behavior::ExecuteResult::Failure;

    float current_speed = self->velocity.Length();
    if (current_speed < 12.0f) return behavior::ExecuteResult::Failure; 

    Vector2f vel_dir = Normalize(self->velocity);
    Vector2f goal_dir = Normalize(target - self->position);
    float momentum_alignment = vel_dir.Dot(goal_dir);
    
    if (momentum_alignment < 0.3f) return behavior::ExecuteResult::Failure; 

    return behavior::ExecuteResult::Success;
  }

  const char* target_key;
};

} // End of Anonymous Namespace

std::unique_ptr<behavior::BehaviorNode> OffenseBehavior::CreateTree(behavior::ExecuteContext& ctx) {
  using namespace behavior;

  BehaviorBuilder builder;
  Vector2f center_ice(HockeyConfig::kCenterX, HockeyConfig::kCenterY);

  // clang-format off
  builder
    .Selector()
        .Sequence()
            .InvertChild<ShipQueryNode>("request_ship")
            .Child<ShipRequestNode>("request_ship")
            .End()

        .Sequence()
            .Child<PowerballCarryQueryNode>()
            .Child<AvoidEnemyNode>(8.0f) 
            
            .Child<OffenseGoalQueryNode>("enemy_goal", true)
            .Child<HockeyShootingPositionNode>("shooting_position")
            
            .Selector()
                .Sequence() 
                    .Child<FindGiveAndGoTeammateNode>("pass_target_id", "enemy_goal")
                    .Child<DynamicAimAndShootNode>("pass_target_id", true) 
                    .End()
                .Sequence() 
                    .Child<HockeyInShootingRangeNode>("enemy_goal")
                    .Child<DynamicAimAndShootNode>("enemy_goal", false) 
                    .End()
                .Sequence() 
                    .Child<FindOpenTeammateNode>("pass_target_id", "enemy_goal")
                    .Child<DynamicAimAndShootNode>("pass_target_id", true) 
                    .End()
                .Sequence() 
                    .Child<ShipTraverseQueryNode>("shooting_position")
                    .Child<HockeyCarryPuckNode>("shooting_position")
                    .End()
            .End()
        .End()

        .Sequence()
            .InvertChild<PowerballCarryQueryNode>()
            .Child<PowerballClosestQueryNode>("puck_position", true)
            .Child<HockeyOffBallStateNode>("puck_position")
            
            .Selector()
                .Sequence()
                    .Child<EqualityNode<int>>("tactical_state", 1)
                    .Child<HockeySlotPositionNode>("puck_position")
                    .End()
                .Sequence()
                    .Child<EqualityNode<int>>("tactical_state", 2)
                    .Child<HockeySeekAndDestroyNode>("puck_position")
                    .End()
                .Sequence()
                    .Child<EqualityNode<int>>("tactical_state", 0)
                    .Child<HockeyLoosePuckChaseNode>("puck_position")
                    .End()
            .End()
        .End()

        .Sequence()
            .Child<GoToNode>(center_ice)
            .End()
    .End();
  // clang-format on

  return builder.Build();
}

}  // namespace hockeyzone
}  // namespace zero
