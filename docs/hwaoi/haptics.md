# DualSense haptics

`TOTK_DS_HAPTICS=auto` (the default; `off` disables it) plays the game's HD rumble on a DualSense's voice-coil actuators through
its 4-channel USB audio device (`runtime/dualsense_haptics.c`) instead of collapsing the two bands of each side into motor
strengths. Each side's low and high band (amplitude 0..1 at a frequency in Hz) becomes two sine waves on that side's actuator
(channels 3 and 4 of the device), with a square-root amplitude curve, 4 ms smoothing and a 2 s fade-out when the game stops
sending. The technique is the one RenHDrum (Apache-2.0) and Eden's `dualsense_haptics` use; this is an independent
implementation.

It is used only for a player whose controller is the only wired DualSense (SDL cannot say which audio device belongs to which
pad) and only while exactly one DualSense audio device with 4 channels exists. Bluetooth has no audio device, so a Bluetooth
DualSense keeps the normal rumble; so does any other controller. If the device is unplugged the stream is closed and the normal
rumble resumes.

On Linux with PipeWire the card must be on the Direct profile (`pactl set-card-profile <card> Direct`), and a new output starts
at about 6% volume, so set it once: `wpctl set-volume <sink> 1.0`. The runtime logs a hint when a DualSense audio device has
fewer than 4 channels.

`./hyrule-warriors-aoi-recomp-optimized --probe-haptics` plays a test pattern and measures it with the controller's own
accelerometer (keep it still on a table): the standard deviation of each axis in milli-g per step against a silent baseline.
The IMU samples at about 290 Hz, so it cannot measure bands above roughly 100 Hz reliably. Measured on a real DualSense: 40 to 313
times the baseline per step, strongest around 80 Hz; a Steam Controller 2 in PS5 mode through the same path: 2 to 28 times, same shape.

The settings menu row "DualSense haptics" sits next to the other rumble settings. The strength setting scales the amplitudes
(its default of 130 % is full scale).
