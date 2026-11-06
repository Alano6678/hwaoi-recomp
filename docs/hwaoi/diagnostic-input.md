# Expiring diagnostic input

The optional input feed provides controlled buttons and sticks for local
bring-up when desktop focus is unavailable. It is disabled by default and uses
the normal HID input merge, so collision, animation and gameplay code still run.
It does not teleport the player, modify saves or patch game memory.

Create a feed under ignored `work/reports/`, then explicitly opt into it:

```powershell
python tools/hwaoi_input.py --feed work/reports/input-feed.txt --hold-ms 0
python tools/hwaoi.py launch --input-feed work/reports/input-feed.txt
```

A supervised probe also accepts `--input-feed <path>`. Probe input still uses an
isolated local profile, and a normal deadline remains in force. Use
`--extended-play --timeout 600` only when a longer bounded story check is needed.

Send one command at a time while observing the resulting game image/log:

```powershell
# Move forward for three seconds, then automatically release
python tools/hwaoi_input.py --feed work/reports/input-feed.txt --left 0 32767 --hold-ms 3000

# Rotate the camera; use short steps and inspect the result
python tools/hwaoi_input.py --feed work/reports/input-feed.txt --right 12000 0 --hold-ms 300

# A short normal-attack input and an explicit release
python tools/hwaoi_input.py --feed work/reports/input-feed.txt --buttons Y --hold-ms 150
python tools/hwaoi_input.py --feed work/reports/input-feed.txt --hold-ms 0
```

Commands contain a monotonically increasing sequence, a button mask, four signed
axes and a hold duration. The runtime polls at most 30 times per second, rejects
invalid/stale updates, and expires every accepted command within ten seconds on
the host clock. Replacing the file does not refresh the same command's deadline.
The command log records accepted sequence, controls and duration. Keep the feed
private under the ignored work tree. Synthetic input does not by itself prove
the game reacted: verify position/camera/animation changes or progression in the
captured output.
