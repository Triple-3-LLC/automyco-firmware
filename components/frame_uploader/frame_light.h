#pragma once
// Lit timelapse frames: when to light the camera's LED for an uploaded frame, and the short
// light -> settle -> discard -> capture -> restore sequence. Plain C++ with no ESPHome or ESP-IDF
// types, so the host test (tests/test_frame_light.cpp) runs the same code the camera does.
//
// "Dark" comes from the OV2640's own auto-exposure state, read over SCCB just before the upload:
// the sensor runs AE on every frame it outputs (whether or not the ESP32 keeps the frame), so its
// exposure time (AEC, in sensor line periods) and analog gain describe the scene as it is right now.
// Exposure index = AEC x gain, in "lines at 1x gain". A lit chamber needs a fraction of a frame's
// exposure at 1x; a dark one has run the shutter to the full frame (about 1,200 lines at UXGA) and
// started adding gain. Reading three registers costs nothing, unlike decoding a JPEG to average
// its luma, and it doesn't need the chamber's light schedule (a room light or a window counts too).

#include <cstdint>

namespace frame_light {

// OV2640 sensor-bank GAIN register (0x00): gain = (b7+1)(b6+1)(b5+1)(b4+1) x (1 + b[3:0]/16).
// Returned x16 so it stays an integer: 16 = 1x, 32 = 2x, ... up to 16 x 31 = 496 (31x).
inline uint32_t gain_x16(uint8_t reg) {
  const uint32_t coarse = ((reg >> 7) & 1u) + 1u;
  const uint32_t c6 = ((reg >> 6) & 1u) + 1u;
  const uint32_t c5 = ((reg >> 5) & 1u) + 1u;
  const uint32_t c4 = ((reg >> 4) & 1u) + 1u;
  return coarse * c6 * c5 * c4 * (16u + (reg & 0x0Fu));
}

// OV2640 exposure (AEC[15:0]) is spread over three sensor-bank registers:
// REG45[5:0] = AEC[15:10], AEC (0x10) = AEC[9:2], REG04[1:0] = AEC[1:0].
inline uint32_t aec_lines(uint8_t reg04, uint8_t reg10, uint8_t reg45) {
  return (static_cast<uint32_t>(reg45 & 0x3Fu) << 10) | (static_cast<uint32_t>(reg10) << 2) | (reg04 & 0x03u);
}

// Exposure the scene needs, in sensor lines at 1x gain. Bigger = darker.
inline uint32_t exposure_index(uint32_t aec, uint8_t gain_reg) { return aec * gain_x16(gain_reg) / 16u; }

enum class Decision : uint8_t {
  PLAIN,  // take the frame as it is (lighting off, LED already on, or the scene is bright enough)
  LIGHT,  // light the LED for this frame
};

// read_ok = false: the sensor registers couldn't be read (or it isn't an OV2640). Light anyway: a
// 1-2 s LED pulse costs less than a black frame in the timelapse.
inline Decision decide(bool enabled, bool led_already_on, bool read_ok, uint32_t index, uint32_t dark_threshold) {
  if (!enabled || led_already_on)
    return Decision::PLAIN;
  if (!read_ok)
    return Decision::LIGHT;
  return index >= dark_threshold ? Decision::LIGHT : Decision::PLAIN;
}

struct Timing {
  uint32_t ramp_ms = 300;    // LED fades up over this long (no hard flash)
  uint32_t settle_ms = 700;  // from switch-on until frames are asked for (includes the ramp): AE converges
  uint8_t discard = 2;       // frames thrown away after settling: the camera's queued frame predates the
                             // light, and one more gives AE margin (same as camera_http's lit /snapshot)
  uint32_t max_on_ms = 2500; // hard cap: the LED is released at this point whatever happened
};

// One lit capture. The caller turns the LED on (with timing.ramp_ms) right before begin(), then calls
// tick() from its loop and on_frame() for every frame it was waiting for, and does what they return.
// Every path ends in release: the frame to upload arrived, or max_on_ms passed without one. The caller
// keeps the LED in step with holding() after every call, so no return value can leave it on.
class LitCapture {
 public:
  enum class Tick : uint8_t {
    NONE,
    REQUEST,  // ask the camera for a frame (settle time is over)
    TIMEOUT,  // no usable frame within max_on_ms: release the LED, give up on this upload
  };
  enum class Frame : uint8_t {
    IGNORE,   // not ours (not active, or still settling): leave it
    DISCARD,  // drop it and ask for the next one
    USE,      // upload this one; the LED is released (caller restores it now)
    EXPIRED,  // arrived after max_on_ms (tick() hadn't run yet): released, give up like TIMEOUT
  };

  explicit LitCapture(Timing t = Timing()) : t_(t) {}

  void begin(uint32_t now) {
    this->active_ = true;
    this->requested_ = false;
    this->start_ = now;
    this->left_ = this->t_.discard;
  }
  bool active() const { return this->active_; }
  // The LED must be held on exactly while this is true.
  bool holding() const { return this->active_; }
  const Timing &timing() const { return this->t_; }

  Tick tick(uint32_t now) {
    if (!this->active_)
      return Tick::NONE;
    const uint32_t on_for = now - this->start_;  // unsigned: survives the millis() wrap
    if (on_for >= this->t_.max_on_ms) {
      this->active_ = false;
      return Tick::TIMEOUT;
    }
    if (!this->requested_ && on_for >= this->t_.settle_ms) {
      this->requested_ = true;
      return Tick::REQUEST;
    }
    return Tick::NONE;
  }

  Frame on_frame(uint32_t now) {
    if (!this->active_ || !this->requested_)
      return Frame::IGNORE;
    if (now - this->start_ >= this->t_.max_on_ms) {  // tick() hasn't run yet: same as a timeout
      this->active_ = false;
      return Frame::EXPIRED;
    }
    if (this->left_ > 0) {
      this->left_--;
      return Frame::DISCARD;
    }
    this->active_ = false;
    return Frame::USE;
  }

  // Abandon (upload switched off, Wi-Fi dropped): the caller releases the LED.
  void cancel() { this->active_ = false; }

 protected:
  Timing t_;
  bool active_{false};
  bool requested_{false};
  uint32_t start_{0};
  uint8_t left_{0};
};

}  // namespace frame_light
