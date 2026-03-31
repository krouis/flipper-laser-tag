# Flipper Laser Tag

[![Build Flipper Laser Tag](https://github.com/krouis/flipper-laser-tag/actions/workflows/flipper_laser_tag.yml/badge.svg)](https://github.com/krouis/flipper-laser-tag/actions/workflows/flipper_laser_tag.yml)

A multiplayer infrared laser tag game for the Flipper Zero. Players shoot each other using the device's built-in IR LED and receiver. Each hit costs a life, last one standing wins.

---

## Rules of the game

- Each player starts with **5 HP**.
- Press **OK** to fire a shot. Your device name is embedded in every shot so the receiver knows who fired.
- A valid hit from any other player reduces your HP by 1 and triggers an **error sound + vibration**.
- Firing triggers a **success sound + vibration** as confirmation.
- After taking a hit you enter a **3-second cooldown** — you cannot shoot or be hit again until it expires. The screen shows `COOLDOWN...` during this period.
- At **0 HP** the screen shows `GAME OVER` and shooting is disabled.
- Press **BACK** at any time to exit.

### Display

```
Laser Tag
Player: Plasma        #your Flipper's name (Settings > Desktop > Name)
HP: 4/5
COOLDOWN...           #or: OK: Shoot   BACK: Exit / SHOT! / HIT! / GAME OVER
```

### Player identity

Each Flipper is identified by a unique 8-bit ID derived from its hardware UID (XOR of all UID bytes). This ID is transmitted with every shot so your own shots are never counted as hits against you. Your **display name** comes from the name you set in Flipper Settings > Desktop > Name.

---

## Requirements

- Flipper Zero running official firmware (release channel recommended)
- Two or more players, each with their own Flipper Zero

---

## Building

### Option A — inside the full firmware tree (fbt)

Clone the firmware and place (or symlink) this app under `applications_user/flipper-laser-tag/`:

```bash
git clone --recursive https://github.com/flipperdevices/flipperzero-firmware.git
cd flipperzero-firmware

# Build the FAP
./fbt fap_flipper_laser_tag
```

The compiled app is written to:
```
build/f7-firmware-D/.extapps/flipper_laser_tag.fap
```

### Option B — standalone with ufbt (recommended for development)

[ufbt](https://github.com/flipperdevices/flipperzero-ufbt) is a lightweight build tool that downloads only the SDK, without cloning the full firmware:

```bash
# Install ufbt
pip install ufbt

# Build from the app directory
cd applications_user/flipper_laser_tag
ufbt
```

The `.fap` is written to `dist/flipper_laser_tag.fap`.

---

## Deploying

### Option 1 — USB (Flipper connected via cable)

Launch directly on the device with a single command:

```bash
# From the firmware root
./fbt launch_app APPSRC=applications_user/flipper_laser_tag
```

Or with ufbt from the app directory:

```bash
ufbt launch
```

### Option 2 — Copy to SD card (no USB / over qFlipper)

1. Build the FAP using either method above.
2. Copy `flipper_laser_tag.fap` to your SD card:
   - **Physical SD card:** place the file at `SD:/apps/Games/flipper_laser_tag.fap`
   - **qFlipper (GUI):** open qFlipper > File Manager > `SD Card/apps/Games/` > drag and drop the `.fap`
   - **Flipper CLI over USB:**
     ```bash
     # From the firmware root
     python3 scripts/storage.py send build/f7-firmware-D/.extapps/flipper_laser_tag.fap /ext/apps/Games/flipper_laser_tag.fap
     ```
3. On the Flipper, navigate to **Apps > Games > Laser Tag**.

---

## CI / GitHub Actions

The pipeline at `.github/workflows/flipper_laser_tag.yml` triggers on every push or pull request that touches the app source or the workflow file itself. It builds the FAP using ufbt against the latest release SDK and uploads the resulting `flipper_laser_tag.fap` as a build artifact.

To use it on your own fork:
1. Push your fork to GitHub.
2. Actions run automatically:  no secrets or self-hosted runners required.
3. Download the built `.fap` from the **Actions > Build Flipper Laser Tag > Artifacts** section of any successful run.
