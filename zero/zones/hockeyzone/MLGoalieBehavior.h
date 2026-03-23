#pragma once
#include <zero/Math.h>
#include <zero/behavior/Behavior.h>
#include <zero/behavior/BehaviorBuilder.h>
#include <zero/behavior/nodes/BehaviorNode.h>

namespace zero {
namespace hockeyzone {

// ML-driven Goalie steering node
// Takes ball position/velocity, runs the MasterGoalie neural network, 
// and outputs the exact (x, y) target for the bot to seek.
struct MLGoalieSteerNode : public behavior::BehaviorNode {
  MLGoalieSteerNode(const char* ball_pos_key) 
      : ball_pos_key(ball_pos_key) {}
      
  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override;
  
  const char* ball_pos_key;
};

// Neural network goalie behavior for Marvin
struct MLGoalieBehavior : public behavior::Behavior {
  void OnInitialize(behavior::ExecuteContext& ctx) override {
    // You can set the optimal goalie ship here if Marvin shouldn't use the default
    // ctx.blackboard.Set("request_ship", 2); // e.g., Spider
    ctx.blackboard.Set("leash_distance", 35.0f);
  }
  
  std::unique_ptr<behavior::BehaviorNode> CreateTree(behavior::ExecuteContext& ctx) override;
};

}  // namespace hockeyzone
}  // namespace zero
