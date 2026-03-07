Adapted Zero to function within HZ's Server.  Still very much a work in progress.  

zero/zones/svs/Svs.cpp`
**Problem:** SVS controller claims `Zone::Subgame`, conflicting with our HockeyZone controller.

**Solution:** Rename/disable the file.

```bash
mv ~/zero/zero/zones/svs/Svs.cpp ~/zero/zero/zones/svs/Svs.cpp.bak
```

## Files Added

### 1. `zero/zones/hockeyzone/HockeyZone.cpp`
New zone controller that handles `Zone::Subgame` and registers hockey behaviors.

### 2. `zero/zones/hockeyzone/HockeyBehavior.h`
Header for hockey behavior, based on PowerballBehavior structure.

### 3. `zero/zones/hockeyzone/HockeyBehavior.cpp`
Hockey behavior implementation using powerball nodes for puck handling.

## Configuration (zero.cfg)

```ini
[Login]
Server = Subgame
Encryption = Subspace

[Servers]
Subgame = YOUR_SERVER_IP:PORT

[General]
Behavior = hockey
RequestShip = 1

[Subgame]
Behavior = hockey
```

## Build Instructions

```bash
cd ~/zero/build
cmake ..
make -j4
./zero
```

## How It Works

1. Bot connects using VIE/Subspace encryption
2. `JoinRequestEvent` fires -> `HockeyZoneController::IsZone(Zone::Subgame)` returns true -> `in_zone = true`
3. `JoinGameEvent` fires -> triggers `CreateBehaviors("(public)")` -> registers "hockey" behavior
4. `SetBehavior("hockey")` is called based on config
5. Bot enters ship and executes hockey behavior tree (chase puck, find goal, score)

## Known Limitations

- Arena name is hardcoded to "(public)" since VIE clients don't receive arena list
- Goal detection relies on map having goal tiles configured correctly
- Some ASSS-specific features may not work (arena-specific behaviors based on name)

## Next Steps (Phase 2)

- Configure goal/crease coordinates for specific hockey map
- Integrate ONNX/ML models for advanced decision making
- Add goalie behavior
- Add team coordination

All hail the great plushmonkey and his work.  Extremely grateful for everything he has put together and I continue to work with on various different projects.

-Cyris
Message me in Discord/In-game with questions
