#ifndef ZERO_BOT_ZONES_HOCKEYZONE_DEFENDER_BEHAVIOR_H_
#define ZERO_BOT_ZONES_HOCKEYZONE_DEFENDER_BEHAVIOR_H_

#include <zero/behavior/Behavior.h>

namespace zero {
namespace hockeyzone {

struct DefenderBehavior : public behavior::Behavior {
    void OnInitialize(behavior::ExecuteContext& ctx) override; // <-- We missed this line!
    std::unique_ptr<behavior::BehaviorNode> CreateTree(behavior::ExecuteContext& ctx) override;
};

}  // namespace hockeyzone
}  // namespace zero

#endif
