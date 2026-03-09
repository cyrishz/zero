#!/usr/bin/env python3
"""
train2.py - Per-Ship Neural Network Trainer
============================================
Input:   ml_ready_{ShipName}.csv (from prepare2.py)
Output:  bot_brain_{ShipName}.onnx (for onnx2c)

Architecture (onnx2c-safe):
    Linear(7 -> 128) -> ReLU
    Linear(128 -> 64) -> ReLU  
    Linear(64 -> 2)  -> Tanh

Input features (7):
    rel_bx, rel_by   - ball relative to player (tiles)
    pvx, pvy         - player velocity (tiles/tick)
    heading_x, heading_y - unit heading vector
    dist             - distance to ball (tiles)

Output (2):
    dir_x, dir_y     - normalized direction vector toward ball
"""

import argparse
import os
import numpy as np
import pandas as pd
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split

# =============================================================================
# Config
# =============================================================================

SHIPS = ["Warbird", "Javelin", "Spider", "Leviathan",
         "Terrier", "Weasel", "Lancaster", "Shark"]

BATCH_SIZE   = 512
EPOCHS       = 1000
EARLY_STOP   = 100
LR           = 0.001
WEIGHT_DECAY = 1e-5
BLEND        = 0.7  # 70% direct-to-ball, 30% human velocity direction

FEATURE_COLS = ["rel_bx", "rel_by", "pvx", "pvy", "heading_x", "heading_y", "dist"]


# =============================================================================
# Model
# =============================================================================

class BotBrain(nn.Module):
    """Simple MLP - no BatchNorm for onnx2c compatibility."""
    
    def __init__(self):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(7, 128), nn.ReLU(),
            nn.Linear(128, 64), nn.ReLU(),
            nn.Linear(64, 2),
            nn.Tanh(),
        )

    def forward(self, x):
        return self.net(x)


# =============================================================================
# Label Builder
# =============================================================================

def build_labels(df):
    """
    Build training labels from dataframe.
    
    Returns X (features) and Y (labels) as numpy arrays.
    Labels are blended direction vectors - mostly toward ball,
    with some influence from actual player velocity.
    """
    rel_bx = df["rel_bx"].values
    rel_by = df["rel_by"].values
    dist = df["dist"].values.clip(0.01)

    # Direction toward ball
    dir_x = rel_bx / dist
    dir_y = rel_by / dist

    # Human movement direction from velocity
    speed = np.sqrt(df["pvx"].values**2 + df["pvy"].values**2).clip(0.01)
    hum_x = df["pvx"].values / speed
    hum_y = df["pvy"].values / speed

    # Blend: mostly direct-to-ball
    blend = BLEND
    
    # More direct for SHOOT behavior
    if 'behavior' in df.columns:
        shoot_mask = (df["behavior"] == "SHOOT").values
        blend_arr = np.where(shoot_mask, 0.9, blend)
    else:
        blend_arr = np.full(len(df), blend)

    label_x = blend_arr * dir_x + (1 - blend_arr) * hum_x
    label_y = blend_arr * dir_y + (1 - blend_arr) * hum_y

    # Re-normalize
    label_len = np.sqrt(label_x**2 + label_y**2).clip(1e-6)
    label_x /= label_len
    label_y /= label_len

    X = df[FEATURE_COLS].values.astype(np.float32)
    Y = np.stack([label_x, label_y], axis=1).astype(np.float32)
    
    return X, Y


# =============================================================================
# Training
# =============================================================================

def train_ship(ship_name):
    csv_file = f"ml_ready_{ship_name}.csv"
    
    if not os.path.exists(csv_file):
        print(f"  [SKIP] {csv_file} not found")
        return

    print(f"\n{'='*60}")
    print(f"  Training: {ship_name}")
    print(f"{'='*60}")

    df = pd.read_csv(csv_file)
    print(f"  Samples: {len(df):,}")
    
    if 'behavior' in df.columns:
        print(f"  Behaviors: {df['behavior'].value_counts().to_dict()}")

    if len(df) < 500:
        print(f"  [SKIP] Too few samples ({len(df)})")
        return

    X, Y = build_labels(df)

    # Train/val split
    X_tr, X_val, Y_tr, Y_val = train_test_split(X, Y, test_size=0.2, random_state=42)

    # Scale features
    scaler = StandardScaler()
    X_tr = scaler.fit_transform(X_tr).astype(np.float32)
    X_val = scaler.transform(X_val).astype(np.float32)

    # Save scaler for C++ inference
    scaler_file = f"scaler_{ship_name}.txt"
    with open(scaler_file, "w") as f:
        f.write("# StandardScaler for ShipBrains.h\n")
        f.write("# Feature order: rel_bx, rel_by, pvx, pvy, heading_x, heading_y, dist\n")
        f.write("#\n")
        f.write("mean = " + ", ".join(f"{v:.5f}" for v in scaler.mean_) + "\n")
        f.write("scale = " + ", ".join(f"{v:.5f}" for v in scaler.scale_) + "\n")
    print(f"  Scaler: {scaler_file}")

    # DataLoaders
    train_ds = TensorDataset(torch.tensor(X_tr), torch.tensor(Y_tr))
    val_ds = TensorDataset(torch.tensor(X_val), torch.tensor(Y_val))
    train_dl = DataLoader(train_ds, batch_size=BATCH_SIZE, shuffle=True)
    val_dl = DataLoader(val_ds, batch_size=BATCH_SIZE, shuffle=False)

    # Model
    model = BotBrain()
    optimizer = optim.Adam(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
    scheduler = optim.lr_scheduler.ReduceLROnPlateau(optimizer, patience=30, factor=0.5)
    loss_fn = nn.MSELoss()

    best_val = float("inf")
    patience = 0
    best_path = f"best_{ship_name}.pt"

    # Training loop
    for epoch in range(EPOCHS):
        # Train
        model.train()
        train_loss = 0.0
        for Xb, Yb in train_dl:
            optimizer.zero_grad()
            loss = loss_fn(model(Xb), Yb)
            loss.backward()
            optimizer.step()
            train_loss += loss.item() * Xb.size(0)
        train_loss /= len(train_dl.dataset)

        # Validate
        model.eval()
        val_loss = 0.0
        with torch.no_grad():
            for Xb, Yb in val_dl:
                val_loss += loss_fn(model(Xb), Yb).item() * Xb.size(0)
        val_loss /= len(val_dl.dataset)

        scheduler.step(val_loss)

        # Checkpoint
        if val_loss < best_val:
            best_val = val_loss
            patience = 0
            torch.save(model.state_dict(), best_path)
        else:
            patience += 1
            if patience >= EARLY_STOP:
                print(f"  Early stop at epoch {epoch+1}. Best val: {best_val:.6f}")
                break

        if (epoch + 1) % 50 == 0 or epoch == 0:
            lr_now = optimizer.param_groups[0]["lr"]
            print(f"  Epoch {epoch+1:4d}/{EPOCHS} | Train: {train_loss:.6f} | Val: {val_loss:.6f} | LR: {lr_now:.6f}")

    # Export ONNX
    model.load_state_dict(torch.load(best_path))
    model.eval()

    dummy = torch.randn(1, 7, dtype=torch.float32)
    onnx_file = f"bot_brain_{ship_name}.onnx"

    torch.onnx.export(
        model, dummy, onnx_file,
        export_params=True,
        opset_version=11,
        do_constant_folding=True,
        input_names=["inputs"],
        output_names=["outputs"],
        dynamic_axes=None,
    )
    
    print(f"  Exported: {onnx_file} (best val: {best_val:.6f})")
    print(f"  Next: onnx2c {onnx_file} > warbird_brain.c")
    
    # Cleanup temp file
    if os.path.exists(best_path):
        os.remove(best_path)


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ship", default=None, help="Train single ship (e.g. --ship Warbird)")
    args = parser.parse_args()

    targets = [args.ship] if args.ship else SHIPS
    
    for ship in targets:
        train_ship(ship)

    print("\nDone!")
