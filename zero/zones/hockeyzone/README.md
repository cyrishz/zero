/** HockeyBehavior.cpp
 * MAJOR REFACTOR: PUCK PHYSICS & POSITIONAL IQ
 * -------------------------------------------
 * 1. FLIGHT & KINETICS
 * - HockeyChasePuckNode: Implemented 14.5-tile "lock-on" logic. Matches puck velocity 
 * on approach to eliminate momentum-based "orbiting."
 * - HockeyCarryPuckNode: Hard-capped carry speed at 18.0 tiles/sec to prevent 
 * desyncing from the puck's physics hitbox.
 *
 * 2. OFFENSIVE UTILITY & SHOOTING
 * - Cross-Crease Aiming: Targets dynamic inner posts (Y: 499/524) based on position
 * rather than net center.
 * - Drive-By Mechanics: Removed < 3.0f speed requirement for shooting; implemented
 * "Aiming Brakes" to stabilize rotation during high-speed passes of the crease.
 * - Range Correction: Increased trigger range to 35.0f; fixed distance-to-center bug.
 *
 * 3. TACTICAL AI (HOCKEY IQ)
 * - IsLaneClear: Custom vector-based collision detection (3.5-tile clearance check).
 * - FindOpenTeammateNode: Prioritizes passing to teammates closer to the net if 
 * the passing lane is clear.
 * - Weak-Side Support: Off-puck attackers now break chase to find open one-timer lanes.
 *
 * 4. DEFENSE & TRIGGER DISCIPLINE
 * - The Enforcer: Relentless enemy tracking (< 2.0 tiles) with pulsed Bullet input.
 * - Safety Overrides: Forces InputAction::Bullet to false upon puck possession to 
 * prevent accidental "immediate-release" shots.
 *
 * Data-mined via extract_heuristics.py / find_shots.py.
 */ - 3/9/26 12:40p EDT
