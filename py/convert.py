#!/usr/bin/env python3
"""
convert.py - Parse Continuum .rec replay files for ML training

Outputs:
  - batch_positions.csv: Player + ball positions per frame
  - batch_events.csv: Ball pickup/fire/goal events

Columns in batch_positions.csv:
  ReplayName, Time, PlayerID, Name, Freq, Ship, 
  X, Y, VelX, VelY, Rot, Energy,
  BX, BY, BVelX, BVelY, BallCarrier

All coordinates are in RAW PIXELS (prepare2.py will convert to tiles).
"""

import struct
import gzip
import io
import os
import glob
import pandas as pd

def batch_process_replays(directory_path="."):
    all_positions_data = []
    all_events_data = []
    
    # Grab all .rec files in the target directory
    rec_files = glob.glob(os.path.join(directory_path, "*.rec"))
    print(f"Found {len(rec_files)} replay files. Starting batch processing...")

    for file_path in rec_files:
        replay_name = os.path.basename(file_path)
        print(f"Parsing {replay_name}...")
        
        players = {}  # Reset the player dictionary for every new file
        
        # Track ball state (there's typically 1 ball in hockey, but support multiple)
        balls = {}  # ball_id -> {x, y, vx, vy, carrier_pid}
        
        try:
            with open(file_path, 'rb') as f:
                # 1. Read ReplayFileHeader
                header_raw = f.read(40)
                if len(header_raw) < 40:
                    continue  # Skip empty files
                    
                magic, version, offset, events, end_time, max_pid, spec_freq, time_rec, map_chk = struct.unpack('<8s 8i', header_raw)
                
                recorder = f.read(24).decode('ascii', 'ignore').strip('\x00')
                arena = f.read(24).decode('ascii', 'ignore').strip('\x00')
                
                # Fast-forward past variable-length comments
                comments_len = offset - f.tell()
                if comments_len > 0:
                    f.read(comments_len)
                
                # 2. Decompress body
                compressed_body = f.read()
                uncompressed_body = gzip.decompress(compressed_body)
                stream = io.BytesIO(uncompressed_body)
                stream_len = len(uncompressed_body)
                
                # 3. Iterate Byte Stream
                while stream.tell() < stream_len:
                    server_time, evt_type = struct.unpack('<ih', stream.read(6))
                    
                    if evt_type == 1:  # ENTER
                        pid, = struct.unpack('<h', stream.read(2))
                        name = stream.read(24).decode('ascii', 'ignore').strip('\x00')
                        squad = stream.read(24).decode('ascii', 'ignore').strip('\x00')
                        ship, freq = struct.unpack('<hh', stream.read(4))
                        players[pid] = {'Name': name, 'Freq': freq, 'Ship': ship}
                        
                    elif evt_type == 2:  # LEAVE
                        pid, = struct.unpack('<h', stream.read(2))
                        # If this player was carrying a ball, drop it at their last position
                        for ball_id, ball in balls.items():
                            if ball.get('carrier') == pid:
                                ball['carrier'] = -1
                        
                    elif evt_type == 3:  # SHIP_CHANGE
                        pid, new_ship, new_freq = struct.unpack('<hhh', stream.read(6))
                        if pid in players:
                            players[pid]['Ship'] = new_ship
                            players[pid]['Freq'] = new_freq
                            
                    elif evt_type == 4:  # FREQ_CHANGE
                        pid, new_freq = struct.unpack('<hh', stream.read(4))
                        if pid in players:
                            players[pid]['Freq'] = new_freq
                        
                    elif evt_type == 5:  # KILL
                        stream.read(8)
                        
                    elif evt_type == 6:  # CHAT
                        _, _, _, msg_len = struct.unpack('<hBBh', stream.read(6))
                        stream.read(msg_len)
                        
                    elif evt_type == 7:  # POSITION
                        if version >= 103:
                            struct.unpack('<i', stream.read(4))
                            
                        pos_raw = stream.read(22)
                        length, rot, pid, x_spd, y, chksum, status, x, y_spd, bounty, energy, wep = struct.unpack('<BBihhBBhhHhH', pos_raw)
                        
                        if length > 22:
                            stream.read(length - 22)
                        
                        # Update player's last known position (for ball tracking)
                        if pid in players:
                            players[pid]['last_x'] = x
                            players[pid]['last_y'] = y
                        
                        # Get ball position - use ball 0 if exists, otherwise estimate
                        ball_x, ball_y = 0, 0
                        ball_vx, ball_vy = 0, 0
                        ball_carrier = -1
                        
                        if 0 in balls:
                            ball = balls[0]
                            ball_carrier = ball.get('carrier', -1)
                            
                            if ball_carrier >= 0 and ball_carrier in players:
                                # Ball is being carried - use carrier's position
                                ball_x = players[ball_carrier].get('last_x', ball.get('x', 0))
                                ball_y = players[ball_carrier].get('last_y', ball.get('y', 0))
                                ball_vx, ball_vy = 0, 0
                            else:
                                # Ball is free - use last known ball position
                                ball_x = ball.get('x', 0)
                                ball_y = ball.get('y', 0)
                                ball_vx = ball.get('vx', 0)
                                ball_vy = ball.get('vy', 0)
                        
                        # Dynamically grab the player's current state
                        name = players.get(pid, {}).get('Name', 'Unknown')
                        freq = players.get(pid, {}).get('Freq', -1)
                        ship = players.get(pid, {}).get('Ship', 99)
                        
                        all_positions_data.append([
                            replay_name, server_time, pid, name, freq, ship,
                            x, y, x_spd, y_spd, rot, energy,
                            ball_x, ball_y, ball_vx, ball_vy, ball_carrier
                        ])
                        
                    elif evt_type == 9:  # BALL_PICKUP
                        pid, ball_id = struct.unpack('<hB', stream.read(3))
                        
                        # Update ball state - now being carried by this player
                        if ball_id not in balls:
                            balls[ball_id] = {}
                        balls[ball_id]['carrier'] = pid
                        
                        all_events_data.append([replay_name, server_time, 'BALL_PICKUP', pid, ball_id])
                        
                    elif evt_type == 10:  # BALL_FIRE
                        pid, ball_id, bx, by, bvx, bvy = struct.unpack('<hBiiii', stream.read(19))
                        if version >= 103:
                            stream.read(4)
                        
                        # Update ball state - now free at this position
                        if ball_id not in balls:
                            balls[ball_id] = {}
                        balls[ball_id]['x'] = bx
                        balls[ball_id]['y'] = by
                        balls[ball_id]['vx'] = bvx
                        balls[ball_id]['vy'] = bvy
                        balls[ball_id]['carrier'] = -1
                        
                        all_events_data.append([replay_name, server_time, 'BALL_FIRE', pid, ball_id, bx, by])
                        
                    elif evt_type == 11:  # GOAL
                        pid, ball_id = struct.unpack('<hB', stream.read(3))
                        
                        # Ball scored - reset to center (or wherever it spawns)
                        if ball_id in balls:
                            balls[ball_id]['carrier'] = -1
                            # Ball will respawn at center ice typically
                            balls[ball_id]['x'] = 512 * 16  # Center in raw pixels
                            balls[ball_id]['y'] = 512 * 16
                            balls[ball_id]['vx'] = 0
                            balls[ball_id]['vy'] = 0
                        
                        all_events_data.append([replay_name, server_time, 'GOAL', pid, ball_id])
                        
                    elif evt_type == 12:  # PASS_DELAY
                        stream.read(2)
                        
                    elif evt_type == 8:  # BALL_POSITION (if your recorder has this)
                        # Some recorders track ball position separately
                        try:
                            ball_id, bx, by, bvx, bvy = struct.unpack('<Biiii', stream.read(17))
                            if ball_id not in balls:
                                balls[ball_id] = {}
                            balls[ball_id]['x'] = bx
                            balls[ball_id]['y'] = by
                            balls[ball_id]['vx'] = bvx
                            balls[ball_id]['vy'] = bvy
                        except:
                            pass
                        
                    else:
                        # Unknown event type - try to skip or break
                        break
                        
        except Exception as e:
            print(f"Error parsing {replay_name}: {e}")

    # 4. Export consolidated DataFrames
    print("\nExporting master datasets to CSV...")
    
    pos_df = pd.DataFrame(all_positions_data, columns=[
        'ReplayName', 'Time', 'PlayerID', 'Name', 'Freq', 'Ship',
        'X', 'Y', 'VelX', 'VelY', 'Rot', 'Energy',
        'BX', 'BY', 'BVelX', 'BVelY', 'BallCarrier'
    ])
    pos_df.to_csv("batch_positions.csv", index=False)
    print(f"  -> batch_positions.csv ({len(pos_df)} rows)")
    
    evt_df = pd.DataFrame(all_events_data)
    # Handle variable columns in events
    if len(evt_df.columns) >= 5:
        evt_df.columns = ['ReplayName', 'Time', 'EventType', 'PlayerID', 'BallID'] + \
                         [f'Extra{i}' for i in range(len(evt_df.columns) - 5)]
    evt_df.to_csv("batch_events.csv", index=False)
    print(f"  -> batch_events.csv ({len(evt_df)} rows)")
    
    # Also create the combined_data.csv that prepare2.py expects
    # Rename columns to match prepare2.py expectations
    print("\nCreating combined_data.csv for ML training...")
    
    ml_df = pos_df.copy()
    ml_df = ml_df.rename(columns={
        'VelX': 'VX',
        'VelY': 'VY',
        'Rot': 'Rotation'
    })
    
    # Filter out rows with no valid ball position
    ml_df = ml_df[(ml_df['BX'] != 0) | (ml_df['BY'] != 0)]
    
    # Filter out spectators (ship >= 8)
    ml_df = ml_df[ml_df['Ship'] < 8]
    
    ml_df.to_csv("combined_data.csv", index=False)
    print(f"  -> combined_data.csv ({len(ml_df)} rows, ready for prepare2.py)")
    
    print("\nDone! Next step: python prepare2.py")


# Execute the script on the current directory
if __name__ == "__main__":
    batch_process_replays()
