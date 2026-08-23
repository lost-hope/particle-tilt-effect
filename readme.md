# Accel Tilt - Particle System Effect

A [Sensor Hub](https://github.com/lost-hope/sensor-hub) *consumer* usermod - unlike the `sensor-hub-*-provider`
usermods, it doesn't register a sensor of its own. Instead it adds a new 2D
matrix effect, **"Accel Tilt"**, to WLED's effects list: a box of WLED's
built-in particle system (`wled00/FXparticleSystem.h`) that tilts along with
a real accelerometer reading pulled from the Sensor Hub, so physically
tilting the board rolls the particles the way you'd expect - a sand/ball
toy, not a canned animation.

## Requirements

This usermod is a **consumer** of the [Sensor
Hub](https://github.com/lost-hope/sensor-hub) usermod's `sensor_bus.h` bus
API - it does not include a copy of that header and cannot compile on its
own. You need, next to this usermod in your `custom_usermods`:

- The [Sensor Hub](https://github.com/lost-hope/sensor-hub) usermod itself
  (provides `sensor_bus.h` and the `SensorHub` interface this effect reads
  from).
- An accelerometer **provider** usermod that registers `_accel_x`/`_accel_y`/
  `_accel_z` sensors (e.g. a QMI8658 or MPU6050 provider).

PlatformIO's Library Dependency Finder resolves `#include "sensor_bus.h"`
automatically as long as the Sensor Hub usermod is present in the same
build - no extra include-path flags needed for that part.

## Why a separate usermod, and why `getValueByName()`?

An accelerometer provider registers its X/Y/Z axes as three separate
sensors that all share the same Acceleration measurement type. The hub's
type-based pull API can only hand back *one* sensor for a type - useful for
"give me a temperature", meaningless for "give me X *and* Y from the *same*
chip". This usermod instead uses `getValueByName()` to read whichever
specific axis sensors it needs by name.

## Usage

Add it to `custom_usermods` alongside the Sensor Hub usermod and an
accelerometer provider (QMI8658 or MPU6050), the same way as any other
PlatformIO usermod.

Once built, select **Accel Tilt** from the Effects list on a 2D matrix
segment like any other effect.

## Usermod Settings

These live on the usermod's own Settings page (not the Effects UI) because
they describe a one-time physical setup - which accelerometer to read and
how it's mounted - rather than something you'd want to change every time you
pick a look:

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch for reading the accelerometer (effect still runs with a light constant "gravity" if off/unavailable) |
| Accel prefix | `qmi8658` | Name prefix of the accelerometer provider to read - must match that provider's own **Name prefix** setting (e.g. `mpu6050` if you're using the MPU-6050 provider instead) |
| Matrix X axis | Sensor X | Which accelerometer axis (X/Y/Z) drives the matrix's horizontal direction |
| Matrix Y axis | Sensor Y | Which accelerometer axis (X/Y/Z) drives the matrix's vertical direction |
| Invert Matrix X | off | Flip the matrix X direction - use if tilting right makes particles roll left |
| Invert Matrix Y | off | Flip the matrix Y direction - use if tilting forward makes particles roll away from you instead of towards |
| Poll interval | 50 ms | How often to re-read the accelerometer from the Sensor Hub |

`Matrix X axis` / `Matrix Y axis` pick which physical sensor axis feeds
each matrix direction (not limited to a plain X/Y swap - either can be
mapped to the sensor's Z axis too, for accelerometers mounted with an
unusual orientation). Combined with the two invert toggles, all eight
possible mounting orientations (four rotations, each optionally mirrored)
of an accelerometer relative to the matrix can be matched: tilt the board
and watch which way the particles roll, then adjust whichever setting makes
it feel wrong.

## Effect Settings (Effects tab)

The simulation "look" itself uses ordinary WLED effect sliders, exactly like
every other particle-system effect (e.g. the built-in "PS Box"):

| Slider | Default | Description |
|---|---|---|
| Particles | 80% | How many of the allocated particles are active (`ParticleSystem2D::setUsedParticles()`) |
| Sensitivity | 100 | Scales the accelerometer tilt into a simulation force - higher reacts more strongly to a small tilt |
| Hardness | 128 | Wall/particle collision hardness |
| Size | 1 | Particle size |
| Collisions (checkbox) | on | Whether particles collide with each other or pass through |
| Color by age (checkbox) | off | Color particles by how long they've been alive instead of a fixed random hue per particle |
| Wrap (checkbox) | off | Particles wrap around the edges instead of bouncing off them |
| Palette | - | Standard WLED palette selector for particle color |

## Notes

- If the Sensor Hub usermod isn't present in the build, or the configured
  `Accel prefix` doesn't match any registered sensor, the effect still runs
  - it just falls back to a light constant downward pull instead of real
  tilt, so a misconfiguration looks like "gravity, but no tilt" rather than
  a broken/frozen effect.
- Only reads the two sensor axes currently mapped to the matrix's X/Y via
  `Matrix X axis` / `Matrix Y axis` - the matrix is a 2D plane, so at most
  two of the sensor's three axes are ever meaningful at once.
