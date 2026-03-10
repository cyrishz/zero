#pragma once

#include <zero/Math.h>
#include <zero/behavior/Behavior.h>
#include <zero/behavior/BehaviorBuilder.h>

namespace zero {
namespace hockeyzone {

struct GoalieBehavior : public behavior::Behavior {
  void OnInitialize(behavior::ExecuteContext& ctx) override {
    // 7 is Shark. Change to 6 if you prefer Lancaster!
    ctx.blackboard.Set("request_ship", 7); 
    // Matches his max dive radius for loose pucks
    ctx.blackboard.Set("leash_distance", 20.0f); 
  }

  std::unique_ptr<behavior::BehaviorNode> CreateTree(behavior::ExecuteContext& ctx) override;
};

}  // namespace hockeyzone
}  // namespace zero
