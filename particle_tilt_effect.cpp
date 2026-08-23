#include "wled.h"
#include "FXparticleSystem.h"
#include "sensor_bus.h"

/*
 * Accel Tilt - a 2D matrix effect that pours WLED's particle system into a
 * box and tilts it with a real accelerometer reading, pulled from the
 * Sensor Hub (see ../sensor-hub/usermod_sensor_hub.cpp and
 * ../sensor-hub/sensor_bus.h) rather than an internal oscillator - tilt the
 * physical board and the particles roll the way you'd expect, like a
 * sand/ball toy.
 *
 * This is a *consumer* of the bus, not a provider: it never registers a
 * sensor of its own, it only reads existing ones (an accelerometer
 * provider's "<prefix>_accel_x/_accel_y/_accel_z", e.g. from the
 * sensor-hub-qmi8658-provider or sensor-hub-mpu6050-provider usermods) via
 * getValueByName(). getValue(SensorType::Acceleration) alone can't be used
 * here because a single accelerometer registers X/Y/Z as three separate
 * sensors that all share that one SensorType - getValueByName() is what
 * lets this usermod pick specific axes instead of an arbitrary one.
 *
 * Split of responsibilities:
 *   - This usermod's own Settings page (accelPrefix/matrixXAxis/matrixYAxis/
 *     invertMatrixX/invertMatrixY/pollInterval) covers the physical setup -
 *     which accelerometer to read and how it's mounted relative to the
 *     matrix (which sensor axis maps to the matrix's X/Y, and which way).
 *     That's a hardware/wiring property, set once, so it lives in persisted
 *     usermod config rather than as effect sliders.
 *   - The effect's own sliders (Particles/Sensitivity/Hardness/Size/...)
 *     cover the simulation "look", exactly like every other particle-system
 *     effect in WLED core (see mode_particlebox in wled00/FX.cpp, which
 *     this effect's structure closely follows - same particle system, same
 *     spawn/collision/friction pattern, just with the gravity vector coming
 *     from a real sensor instead of Perlin noise/a rotating circle).
 */

// Written by the usermod's loop() (polls the hub on its own schedule),
// read every effect frame by mode_sensorhub_tilt() - both live in this one
// translation unit. Range is -100..100 ("percent of 1g tilt"), already
// orientation-corrected; the effect's own "Sensitivity" slider applies any
// further user-facing scaling.
static int8_t gAccelTiltX = 0;
static int8_t gAccelTiltY = 0;
static bool   gAccelAvailable = false;

#define FX_FALLBACK_STATIC { SEGMENT.fill(SEGCOLOR(0)); return; }

///////////////////////
//  Effect Function  //
///////////////////////

void mode_sensorhub_tilt(void) {
  ParticleSystem2D *PartSys = nullptr;

  if (SEGMENT.call == 0) { // initialization
    if (!initParticleSystem2D(PartSys, 1, 0, true)) // advanced properties needed for collisions/size
      FX_FALLBACK_STATIC; // allocation failed or not a 2D matrix
    bool wrap = SEGMENT.check3;
    PartSys->setBounceX(!wrap);
    PartSys->setBounceY(!wrap);
    PartSys->setWrapX(wrap);
    PartSys->setWrapY(wrap);
  } else {
    PartSys = reinterpret_cast<ParticleSystem2D *>(SEGENV.data); // not first call - reuse existing system
  }
  if (PartSys == nullptr) FX_FALLBACK_STATIC;

  PartSys->updateSystem(); // update dimensions/data pointers - required every frame
  PartSys->setWallHardness(min(SEGMENT.custom2, (uint8_t)200));
  PartSys->enableParticleCollisions(SEGMENT.check1, max(2, (int)SEGMENT.custom2));
  PartSys->setColorByAge(SEGMENT.check2);
  PartSys->setParticleSize(map(SEGMENT.custom3, 0, 31, 0, 255));
  PartSys->setUsedParticles(SEGMENT.intensity); // intensity is already a 0-255 "percent of allocated particles" value

  // (re)spawn one dead particle per frame until 'usedParticles' is filled -
  // same pacing mode_particlebox uses, so growing the Particles slider
  // fills in gradually instead of all at once.
  for (uint32_t i = 0; i < PartSys->usedParticles; i++) {
    if (PartSys->particles[i].ttl < 260) {
      PartSys->particles[i].ttl = 260;
      PartSys->particles[i].x = hw_random16(PartSys->maxX);
      PartSys->particles[i].y = hw_random16(PartSys->maxY);
      PartSys->particles[i].hue = hw_random8();
      PartSys->particleFlags[i].perpetual = true;
      PartSys->particleFlags[i].collide = true;
      break;
    }
  }

  // The actual tilt: read from the shared state the usermod's loop() keeps
  // updated from the Sensor Hub. Falls back to a light constant downward
  // pull (rather than a frozen box) when no accelerometer reading is
  // available yet, e.g. Sensor Hub/provider not present in this build.
  int32_t xforce, yforce;
  if (gAccelAvailable) {
    xforce = ((int32_t)gAccelTiltX * SEGMENT.custom1) / 100;
    yforce = ((int32_t)gAccelTiltY * SEGMENT.custom1) / 100;
  } else {
    xforce = 0;
    yforce = (int32_t)SEGMENT.custom1 / 4;
  }
  PartSys->applyForce((int8_t)constrain(xforce, -127, 127), (int8_t)constrain(yforce, -127, 127));

  if ((SEGMENT.call & 0x0F) == 0) PartSys->applyFriction(1); // every 16th frame, matches mode_particlebox

  PartSys->update(); // physics + render
}
static const char _data_FX_MODE_SENSORHUB_TILT[] PROGMEM = "Accel Tilt@,Particles,Sensitivity,Hardness,Size,Collisions,Color by age,Wrap;;!;2;ix=80,c1=100,c2=128,c3=1,o1=1";

/////////////////////
//  UserMod Class  //
/////////////////////

// Which physical accelerometer axis feeds a given matrix axis - one of
// these is picked per matrix axis via a dropdown, instead of a single
// "swap X/Y" toggle, so a sensor mounted with e.g. its Z axis in the
// matrix plane can still be mapped correctly.
enum class AccelAxis : uint8_t { SensorX = 0, SensorY = 1, SensorZ = 2 };

class ParticleTiltEffectUsermod : public Usermod {
  private:
    SensorHub* hub = nullptr;

    // config
    bool enabled = true;
    String accelPrefix = "qmi8658"; // must match an accelerometer provider's namePrefix; sensor names become "<prefix>_accel_x/_accel_y/_accel_z"
    uint8_t matrixXAxis = (uint8_t)AccelAxis::SensorX; // which sensor axis drives the matrix's horizontal (X) direction
    uint8_t matrixYAxis = (uint8_t)AccelAxis::SensorY; // which sensor axis drives the matrix's vertical (Y) direction
    bool invertMatrixX = false;
    bool invertMatrixY = false;
    uint16_t pollIntervalMs = 50;   // how often to re-read the accelerometer from the hub

    unsigned long lastPoll = 0;

    static const char _name[];
    static const char _enabled[];
    static const char _accelPrefix[];
    static const char _matrixXAxis[];
    static const char _matrixYAxis[];
    static const char _invertMatrixX[];
    static const char _invertMatrixY[];
    static const char _pollInterval[];

    // Reads the sensor axis currently mapped to 'axis' (e.g. "qmi8658_accel_z").
    bool readAxis(AccelAxis axis, float& out) {
      const char* suffix = axis == AccelAxis::SensorX ? "_accel_x"
                          : axis == AccelAxis::SensorY ? "_accel_y"
                                                        : "_accel_z";
      return hub->getValueByName((accelPrefix + suffix).c_str(), out);
    }

  public:
    void setup() override {
      strip.addEffect(255, &mode_sensorhub_tilt, _data_FX_MODE_SENSORHUB_TILT);
    }

    void loop() override {
      if (!enabled) { gAccelAvailable = false; return; }

      if (!hub) hub = getSensorHub(); // Sensor Hub usermod may finish init after us
      if (!hub) { gAccelAvailable = false; return; }

      unsigned long now = millis();
      if (now - lastPoll < pollIntervalMs) return;
      lastPoll = now;

      float rawMatrixX, rawMatrixY;
      if (!readAxis((AccelAxis)matrixXAxis, rawMatrixX) ||
          !readAxis((AccelAxis)matrixYAxis, rawMatrixY)) {
        gAccelAvailable = false; // wrong prefix, provider not present yet, or no reading yet
        return;
      }

      if (invertMatrixX) rawMatrixX = -rawMatrixX;
      if (invertMatrixY) rawMatrixY = -rawMatrixY;

      // Normalize m/s^2 (the hub's canonical Acceleration unit - see
      // sensor_bus.h) to a -100..100 "percent of 1g tilt" range, so this
      // works the same regardless of which accelerometer chip is behind it.
      const float g = 9.80665f;
      long x = lroundf((rawMatrixX / g) * 100.0f);
      long y = lroundf((rawMatrixY / g) * 100.0f);
      gAccelTiltX = (int8_t)constrain(x, -127L, 127L);
      gAccelTiltY = (int8_t)constrain(y, -127L, 127L);
      gAccelAvailable = true;
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_accelPrefix)] = accelPrefix;
      top[FPSTR(_matrixXAxis)] = matrixXAxis;
      top[FPSTR(_matrixYAxis)] = matrixYAxis;
      top[FPSTR(_invertMatrixX)] = invertMatrixX;
      top[FPSTR(_invertMatrixY)] = invertMatrixY;
      top[FPSTR(_pollInterval)] = pollIntervalMs;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_accelPrefix)], accelPrefix);
      configComplete &= getJsonValue(top[FPSTR(_matrixXAxis)], matrixXAxis);
      configComplete &= getJsonValue(top[FPSTR(_matrixYAxis)], matrixYAxis);
      configComplete &= getJsonValue(top[FPSTR(_invertMatrixX)], invertMatrixX);
      configComplete &= getJsonValue(top[FPSTR(_invertMatrixY)], invertMatrixY);
      configComplete &= getJsonValue(top[FPSTR(_pollInterval)], pollIntervalMs);
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('ParticleTiltEffect:accelPrefix',1,'name prefix of the accelerometer provider to read, e.g. &quot;qmi8658&quot; or &quot;mpu6050&quot; - sensor names become &lt;prefix&gt;_accel_x/_accel_y/_accel_z');"));
      settingsScript.print(F("dd=addDropdown('ParticleTiltEffect','matrixXAxis');addOption(dd,'Sensor X',0);addOption(dd,'Sensor Y',1);addOption(dd,'Sensor Z',2);"));
      settingsScript.print(F("addInfo('ParticleTiltEffect:matrixXAxis',1,'accelerometer axis that drives the matrix&#39;s horizontal (X) direction');"));
      settingsScript.print(F("dd=addDropdown('ParticleTiltEffect','matrixYAxis');addOption(dd,'Sensor X',0);addOption(dd,'Sensor Y',1);addOption(dd,'Sensor Z',2);"));
      settingsScript.print(F("addInfo('ParticleTiltEffect:matrixYAxis',1,'accelerometer axis that drives the matrix&#39;s vertical (Y) direction');"));
      settingsScript.print(F("addInfo('ParticleTiltEffect:invertMatrixX',1,'flip the matrix X direction');"));
      settingsScript.print(F("addInfo('ParticleTiltEffect:invertMatrixY',1,'flip the matrix Y direction');"));
      settingsScript.print(F("addInfo('ParticleTiltEffect:pollInterval',1,'milliseconds between accelerometer reads from the Sensor Hub');"));
    }
};

const char ParticleTiltEffectUsermod::_name[]           PROGMEM = "ParticleTiltEffect";
const char ParticleTiltEffectUsermod::_enabled[]        PROGMEM = "enabled";
const char ParticleTiltEffectUsermod::_accelPrefix[]    PROGMEM = "accelPrefix";
const char ParticleTiltEffectUsermod::_matrixXAxis[]    PROGMEM = "matrixXAxis";
const char ParticleTiltEffectUsermod::_matrixYAxis[]    PROGMEM = "matrixYAxis";
const char ParticleTiltEffectUsermod::_invertMatrixX[]  PROGMEM = "invertMatrixX";
const char ParticleTiltEffectUsermod::_invertMatrixY[]  PROGMEM = "invertMatrixY";
const char ParticleTiltEffectUsermod::_pollInterval[]   PROGMEM = "pollInterval";

static ParticleTiltEffectUsermod particle_tilt_effect;
REGISTER_USERMOD(particle_tilt_effect);
