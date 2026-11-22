#include "hle/no100f_particles_update.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace No100fParticlesUpdate;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace {
// A channel at 30 fps, as the game does it: every frame, the frame's time.
uint8_t Fade30(uint8_t channel, float rate, float seconds) {
    const int frames = static_cast<int>(seconds * 30.0f + 0.5f);
    for (int i = 0; i < frames; ++i) channel = FadeChannel(channel, rate, 1.0f / 30.0f);
    return channel;
}

// A channel at 60 fps through the clock.
uint8_t Fade60(uint8_t channel, float rate, float seconds) {
    StepClock clock;
    const int frames = static_cast<int>(seconds * 60.0f + 0.5f);
    for (int i = 0; i < frames; ++i) channel = FadeChannel(channel, rate, clock.Step(i, 1.0f / 60.0f));
    return channel;
}

// A channel at 60 fps with no clock: the bug.
uint8_t Fade60Raw(uint8_t channel, float rate, float seconds) {
    const int frames = static_cast<int>(seconds * 60.0f + 0.5f);
    for (int i = 0; i < frames; ++i) channel = FadeChannel(channel, rate, 1.0f / 60.0f);
    return channel;
}

// World distance a particle falls in the given time: velocity changes, then moves, as PARS command order 2, 3 does.
float Fall(float acceleration, int framesPerSecond, float scale, float seconds) {
    const float dt = 1.0f / static_cast<float>(framesPerSecond);
    const int frames = static_cast<int>(seconds * static_cast<float>(framesPerSecond) + 0.5f);
    float velocity = 0.0f, position = 0.0f;
    for (int i = 0; i < frames; ++i) {
        velocity += acceleration * (dt * scale);
        position += velocity;
    }
    return position;
}

// A run of game frames with fps60's timing table, calling a command's model whenever the table says it runs.
template <typename Command>
void RunTable(uint32_t type, float seconds, Command command) {
    StepClock clock;
    const int frames = static_cast<int>(seconds * 60.0f + 0.5f);
    for (int i = 0; i < frames; ++i) {
        const float dt = 1.0f / 60.0f;
        const CommandTime when = CommandTimeFps60(type, dt, clock.Step(static_cast<uint32_t>(i), dt));
        command(when.run, when.time);
    }
}

// Command 13's timer (0x800C2ED8): counts down with the time, steps at 0 or below, reloads with no carry. Interval 0
// steps on every call. Returns the steps taken.
struct TextureAnimation {
    float interval, timer = 0.0f;
    int steps = 0;
    void Call(float time) {
        if (interval > 0.0f) {
            timer -= time;
            if (timer > 0.0f) return;
            timer = interval;
        }
        ++steps;
    }
};

int TextureSteps(float interval, int framesPerSecond, float seconds) {
    TextureAnimation animation{interval};
    const int frames = static_cast<int>(seconds * static_cast<float>(framesPerSecond) + 0.5f);
    for (int i = 0; i < frames; ++i) animation.Call(1.0f / static_cast<float>(framesPerSecond));
    return animation.steps;
}

// Command 21 (0x800C3938): angle byte = (int)(angle + rate * time), low 8 bits.
uint8_t Spin(uint8_t angle, float rate, float time) {
    return static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(angle) + rate * time));
}

// Total spin in bytes at 30 fps, as the game does it.
int Spin30(float rate, float seconds) {
    uint8_t angle = 0;
    int total = 0;
    const int frames = static_cast<int>(seconds * 30.0f + 0.5f);
    for (int i = 0; i < frames; ++i) {
        const uint8_t next = Spin(angle, rate, 1.0f / 30.0f);
        total += static_cast<int8_t>(static_cast<uint8_t>(next - angle));
        angle = next;
    }
    return total;
}

// Command 27 (0x800C3650), one axis, toward the target at 0: pull by strength * time, die inside 0.25, and inside 2
// multiply the velocity by distance * 0.49 on each call. Then MOVE adds the velocity every frame. Returns seconds to die.
template <typename Timing>
float Warp(float start, float pull, int framesPerSecond, float velocityScale, Timing timing) {
    const float frameTime = 1.0f / static_cast<float>(framesPerSecond);
    float position = start, velocity = 0.0f;
    for (int i = 0; i < framesPerSecond * 30; ++i) {
        float time = frameTime;
        if (timing(i, frameTime, time)) {
            velocity -= pull * time * velocityScale;
            if (position < 0.25f) return static_cast<float>(i) * frameTime;
            if (position < 2.0f) velocity *= position * 0.49f;
        }
        position += velocity;
    }
    return 30.0f;
}

// Command 6 (0x800C4734) in the mode the disc uses, with a particle speeding up steadily in the world: the age at which
// speed squared passes limit * time. Speed is per frame, so it is halved at 60 frames a second.
float SpeedKillAge(float limit, float worldAcceleration, int framesPerSecond, float timeScale) {
    const float frameTime = 1.0f / static_cast<float>(framesPerSecond);
    for (int i = 0; i < framesPerSecond * 30; ++i) {
        const float age = static_cast<float>(i) * frameTime;
        const float speed = worldAcceleration * age * frameTime;
        if (speed * speed > limit * frameTime * timeScale) return age;
    }
    return 30.0f;
}
}  // namespace

int main() {
    try {
        // The bug: the fireworks' two alpha rates, as measured with fps60 on and no fix.
        Require(Fade60Raw(255, -51.0f, 4.1f) == 255, "FW1 does not fade at 60 fps without the clock");
        Require(Fade60Raw(255, -102.0f, 1.27f) == 179, "FW2 alpha at 1.27 s at 60 fps without the clock");

        // Fireworks at the end of their lives, 30 fps and fixed 60 fps.
        Require(Fade30(255, -51.0f, 5.0f) == 105, "FW1 at 30 fps ends at 105");
        Require(Fade60(255, -51.0f, 5.0f) == 105, "FW1 at 60 fps ends at 105");
        Require(Fade30(255, -102.0f, 2.5f) == 30, "FW2 at 30 fps ends at 30");
        Require(Fade60(255, -102.0f, 2.5f) == 30, "FW2 at 60 fps ends at 30");

        // Other rates from the disc, at every point in a life, and channels rising as well as falling.
        for (float rate : {-510.0f, -340.0f, -170.0f, -106.7f, -36.4f, -42.3f, 64.0f, 150.0f, 13.0f}) {
            for (float seconds = 0.0f; seconds < 2.0f; seconds += 1.0f / 30.0f) {
                Require(Fade30(128, rate, seconds) == Fade60(128, rate, seconds), "same fade at 30 and 60 fps");
            }
        }

        // Clamps
        Require(FadeChannel(1, -510.0f, 1.0f / 30.0f) == 0, "clamps at 0");
        Require(FadeChannel(254, 510.0f, 1.0f / 30.0f) == 255, "clamps at 255");

        // Zero life gives an infinite rate: fctiwz saturates and the add wraps, never undefined behaviour.
        const float inf = std::numeric_limits<float>::infinity();
        Require(FadeChannel(200, -inf, 1.0f / 30.0f) == 0, "negative infinity clamps to 0");
        Require(FadeChannel(200, inf, 1.0f / 30.0f) == 0, "positive infinity wraps as on the console");
        Require(FadeChannel(7, inf * 0.0f, 1.0f / 30.0f) == 0, "NaN acts as the most negative word");

        // Slowdown: a frame as long as a 30 Hz step gets the fade on its own.
        {
            StepClock clock;
            Require(clock.Step(1, 1.0f / 30.0f) == 1.0f / 30.0f, "a whole step fades at once");
            Require(clock.Step(2, 1.0f / 60.0f) == 0.0f, "half a step waits");
            Require(clock.Step(3, 1.0f / 30.0f) > 1.0f / 30.0f, "the wait is kept, not lost");
        }

        // Every group in one frame gets the same answer, and no time is counted twice.
        {
            StepClock clock;
            Require(clock.Step(10, 1.0f / 60.0f) == 0.0f, "first frame of a step");
            Require(clock.Step(10, 1.0f / 60.0f) == 0.0f, "second group, same frame");
            const float step = clock.Step(11, 1.0f / 60.0f);
            Require(std::fabs(step - 1.0f / 30.0f) < 1e-6f, "second frame gets the whole step");
            Require(clock.Step(11, 1.0f / 60.0f) == step, "second group gets it too");
            Require(clock.Step(12, 1.0f / 60.0f) == 0.0f, "next step starts over");
        }

        // A frame counter that starts at 0 still counts its first frame.
        {
            StepClock clock;
            clock.Step(0, 1.0f / 60.0f);
            Require(std::fabs(clock.Step(1, 1.0f / 60.0f) - 1.0f / 30.0f) < 1e-6f, "frame 0 counts");
        }

        // Fall: the fireworks' -0.03. Unscaled it falls twice as far at 60 fps; scaled it matches 30 fps.
        for (float seconds : {1.0f, 2.5f, 5.0f}) {
            const float at30 = Fall(-0.03f, 30, 1.0f, seconds);
            Require(std::fabs(Fall(-0.03f, 60, 1.0f, seconds) / at30 - 2.0f) < 0.05f, "unscaled falls twice as far");
            Require(std::fabs(Fall(-0.03f, 60, kVelocityScaleFps60, seconds) / at30 - 1.0f) < 0.03f, "scaled matches");
        }

        // The timing table. A command it doesn't name runs every frame with the frame's time.
        {
            const float dt = 1.0f / 60.0f, step = 1.0f / 30.0f;
            for (uint32_t type : {2u, 4u, 6u, 20u}) {
                const CommandTime when = CommandTimeFps60(type, dt, 0.0f);
                Require(when.run && when.time == dt * 0.5f, "velocity commands: every frame at half time");
            }
            for (uint32_t type : {13u, 15u, 21u, 27u}) {
                Require(!CommandTimeFps60(type, dt, 0.0f).run, "step commands wait for the step");
                Require(CommandTimeFps60(type, dt, step).run, "step commands run on the step");
            }
            Require(CommandTimeFps60(13, dt, step).time == step, "texture animation gets the whole step");
            Require(CommandTimeFps60(21, dt, step).time == step, "spin gets the whole step");
            Require(CommandTimeFps60(27, dt, step).time == step * 0.5f, "warp gets half the step");
            Require(CommandTimeFps60(15, dt, step).time == step, "random turn gets the whole step");

            // Command 15 turns the velocity by a random angle up to rate * time: how far its direction wanders goes with
            // turns a second * angle squared. Measured in i004: 0.71x as much at 60 fps without the table.
            const auto wander = [](float turnsPerSecond, float angle) { return turnsPerSecond * angle * angle; };
            const float at30 = wander(30.0f, step);
            Require(std::fabs(wander(30.0f, CommandTimeFps60(15, dt, step).time) / at30 - 1.0f) < 1e-4f, "turn matches");
            Require(std::fabs(wander(60.0f, dt) / at30 - 0.5f) < 1e-4f, "without the table it wanders half as much");
            for (uint32_t type : {3u, 11u, 12u, 18u, 19u, 23u, 24u, 25u, 26u, 28u}) {
                const CommandTime when = CommandTimeFps60(type, dt, step);
                Require(when.run && when.time == dt, "other commands are unchanged");
            }
        }

        // Commands 4 and 20 change velocity by a force * time, as command 2 does. A model of each through the table against
        // 30 fps: 20 with a steady wind, 4 with a push that weakens as the particle gets away.
        for (uint32_t type : {4u, 20u}) {
            const auto force = [type](float position) { return type == 20 ? 0.1f : 0.5f / (1.0f + position); };
            float at30 = 0.0f, velocity = 0.0f;
            for (int i = 0; i < 90; ++i) {
                velocity += force(at30) * (1.0f / 30.0f);
                at30 += velocity;
            }
            float through = 0.0f;
            velocity = 0.0f;
            RunTable(type, 3.0f, [&](bool run, float time) {
                if (run) velocity += force(through) * time;
                through += velocity;
            });
            Require(std::fabs(through / at30 - 1.0f) < 0.03f, "force on velocity matches 30 fps");
            float raw = 0.0f;
            velocity = 0.0f;
            for (int i = 0; i < 180; ++i) {
                velocity += force(raw) * (1.0f / 60.0f);
                raw += velocity;
            }
            Require(raw / at30 > 1.15f, "without the table the force moves it too far");
        }

        // Command 13: the steps of the disc's intervals match 30 fps through the table, and many don't without it.
        for (float interval : {0.0f, 0.01f, 0.03f, 0.04f, 0.05f, 0.06f, 0.1f, 0.3f}) {
            TextureAnimation animation{interval};
            RunTable(13, 6.0f, [&](bool run, float time) { if (run) animation.Call(time); });
            Require(animation.steps == TextureSteps(interval, 30, 6.0f), "texture animation matches 30 fps");
        }
        Require(TextureSteps(0.0f, 60, 6.0f) == 2 * TextureSteps(0.0f, 30, 6.0f), "interval 0 runs 2x without the table");
        Require(TextureSteps(0.05f, 60, 6.0f) > TextureSteps(0.05f, 30, 6.0f), "interval 0.05 runs fast without the table");

        // Command 21: rates from the disc's range, forward and back, match 30 fps to the byte.
        for (float degrees : {100.0f, 130.0f, 180.0f, 250.0f, 300.0f, 360.0f, 400.0f, 700.0f}) {
            const float rate = degrees * 255.0f / 360.0f;
            for (float signedRate : {rate, -rate}) {
                uint8_t angle = 0;
                int total = 0;
                RunTable(21, 4.0f, [&](bool run, float time) {
                    if (!run) return;
                    const uint8_t next = Spin(angle, signedRate, time);
                    total += static_cast<int8_t>(static_cast<uint8_t>(next - angle));
                    angle = next;
                });
                Require(total == Spin30(signedRate, 4.0f), "spin matches 30 fps");
            }
        }

        // Command 27: reaches the target when it does at 30 fps through the table, within one 30 Hz step. Without the
        // table the per-call multiply runs twice as often.
        {
            const auto every = [](int, float, float&) { return true; };
            const float at30 = Warp(6.0f, 0.1f, 30, 1.0f, every);
            StepClock clock;
            const auto table = [&clock](int frame, float dt, float& time) {
                const CommandTime when = CommandTimeFps60(27, dt, clock.Step(static_cast<uint32_t>(frame), dt));
                time = when.time;
                return when.run;
            };
            const float through = Warp(6.0f, 0.1f, 60, 1.0f, table);
            Require(at30 < 30.0f, "warp reaches the target at 30 fps");
            Require(std::fabs(through - at30) <= 1.0f / 30.0f + 1e-4f, "warp matches 30 fps");
        }

        // Command 6: half the frame time kills at the 30 fps speed; the full frame time lets it go about 1.41x as fast.
        {
            const float at30 = SpeedKillAge(0.004f, 0.2f, 30, 1.0f);
            const float fixed = SpeedKillAge(0.004f, 0.2f, 60, kVelocityScaleFps60);
            const float bug = SpeedKillAge(0.004f, 0.2f, 60, 1.0f);
            Require(std::fabs(fixed - at30) <= 1.0f / 30.0f, "speed kill matches 30 fps");
            Require(std::fabs(bug / at30 - std::sqrt(2.0f)) < 0.02f, "speed kill comes late without the table");
        }

        // The group update wrapper hands the game's fade rates scaled by step / dt. With a steady 60 fps frame time that
        // is 0 between steps and 2 at a step, and the game's rate * dt then gives exactly what rate * step would.
        {
            StepClock clock;
            const float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 4; ++frame) {
                const float step = clock.Step(frame, dt);
                const float scale = step / dt;
                Require(scale == (frame % 2 ? 2.0f : 0.0f), "scale is 0 between steps, 2 at a step");
                for (float rate = -600.0f; rate <= 600.0f; rate += 0.37f)
                    for (int channel = 0; channel < 256; channel += 51)
                        Require(FadeChannel(static_cast<uint8_t>(channel), rate * scale, dt) ==
                                    FadeChannel(static_cast<uint8_t>(channel), rate, step),
                                "a scaled rate fades exactly as the step time does");
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "no100f_particles_update_tests passed\n";
    return 0;
}
