#include "webgpu/hdr_presentation.hpp"

#include <cstdio>
#include <limits>

namespace {
int failures = 0;
int checks = 0;

void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

bool Near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}
} // namespace

int main() {
    using namespace aurora::webgpu;
    Check(sizeof(HdrPresentationUniform) == 64 && alignof(HdrPresentationUniform) == 16,
          "uniform size and alignment match four WGSL vec4 fields");
    Check(offsetof(HdrPresentationUniform, paperRect) == 0 &&
          offsetof(HdrPresentationUniform, peakRect) == 16 &&
          offsetof(HdrPresentationUniform, parameters) == 32 &&
          offsetof(HdrPresentationUniform, picture) == 48, "uniform field offsets match WGSL");

    Check(!hdr_output_allowed(0), "a display that reports HDR off keeps the SDR picture");
    Check(hdr_output_allowed(1), "a display that reports HDR on gets HDR output");
    Check(hdr_output_allowed(-1), "an unknown display state (not Windows, mirrored, query failed) changes nothing");

    Check(valid_hdr_brightness(80.f, 80.f), "minimum brightness accepted");
    Check(valid_hdr_brightness(1000.f, 10000.f), "maximum brightness accepted");
    Check(valid_hdr_brightness(324.6f, 4035.8f), "fractional saved adjustments accepted");
    Check(valid_hdr_brightness(480.f, 400.f), "peak below paper accepted without rewriting saved intent");
    Check(!valid_hdr_brightness(79.9f, 1000.f), "paper below minimum rejected");
    Check(!valid_hdr_brightness(1000.1f, 1000.f), "paper above maximum rejected");
    Check(!valid_hdr_brightness(200.f, 79.9f), "peak below minimum rejected");
    Check(!valid_hdr_brightness(200.f, 10000.1f), "peak above maximum rejected");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        Check(!valid_hdr_brightness(invalid, 1000.f), "non-finite paper rejected");
        Check(!valid_hdr_brightness(200.f, invalid), "non-finite peak rejected");
    }

    const auto initial = hdr_presentation_uniform(200.f, 1000.f, 1280, 900);
    Check(Near(initial.picture[0], 2.5f) && Near(initial.picture[1], 4.f),
          "startup defaults retain existing picture mapping");
    Check(Near(initial.parameters[2], 1.f / 1280.f) && Near(initial.parameters[3], 1.f / 900.f),
          "calibration coordinates retain framebuffer dimensions");
    const auto brighter = hdr_presentation_uniform(480.f, 1000.f, 1280, 900);
    Check(Near(brighter.picture[0], 6.f) && Near(brighter.picture[0] / initial.picture[0], 2.4f),
          "new paper white changes next presentation scale");
    Check(Near(brighter.picture[1], 1000.f / 480.f - 1.f),
          "paper update also recalculates highlight boost");
    const auto peakUpdate = hdr_presentation_uniform(480.f, 4000.f, 1280, 900);
    Check(Near(peakUpdate.picture[0], brighter.picture[0]) &&
          peakUpdate.picture[1] > brighter.picture[1], "peak update preserves paper scale");
    Check(peakUpdate.paperRect == std::array<float, 4>{} &&
          peakUpdate.peakRect == std::array<float, 4>{} &&
          peakUpdate.parameters[0] == 0.f && peakUpdate.parameters[1] == 0.f &&
          peakUpdate.picture[0] == brighter.picture[0],
          "disabled calibration patches do not disable game-picture brightness");
    const auto zeroSize = hdr_presentation_uniform(480.f, 400.f, 0, 0);
    Check(zeroSize.parameters[2] == 1.f && zeroSize.parameters[3] == 1.f,
          "zero surface dimensions stay finite");
    Check(zeroSize.picture[0] == 6.f && zeroSize.picture[1] == 0.f,
          "effective peak cannot fall below paper white");

    for (float paper : {80.f, 200.f, 480.f, 1000.f}) {
        for (float peak : {80.f, 400.f, 1000.f, 10000.f}) {
            const auto uniform = hdr_presentation_uniform(paper, peak, 3840, 2160);
            Check(Near(uniform.picture[0] * (1.f + uniform.picture[1]), std::max(paper, peak) / 80.f),
                  "white endpoint matches existing effective-peak scRGB mapping");
        }
    }
    std::printf("HDR presentation: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
