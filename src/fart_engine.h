// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Thin C++ wrapper around libfart (FuriLabs' face-analysis-recognition-toolkit).
// libfart owns detection, passive presentation-attack detection (MiniFASNet V2)
// and MobileFaceNet embedding + match. It does no capture of its own: we hand it
// BGR frames from CameraSource.

#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include <fart/fart.h>
#include <fart/fart_enums.h>
}

class FartEngine
{
public:
    FartEngine() = default;
    ~FartEngine();

    FartEngine(const FartEngine &)            = delete;
    FartEngine &operator=(const FartEngine &) = delete;

    // modelDir holds detect-class1.tflite, mini_fas_net_v2.tflite and
    // mobile_face_net.tflite; enrollmentJson is the decrypted template store
    // (empty when nothing is enrolled yet).
    bool init(const std::string &modelDir, const std::string &enrollmentJson);

    // Frames are tightly packed BGR24.
    RecognitionState recognize(const uint8_t *bgr, int width, int height);

    // Detection only - no enrollment or matching side effects. Returns the
    // number of faces found and, via bestConfidence, how sure the detector was
    // of the strongest one. FrameOrientation uses this to work out which way up
    // this device's sensor delivers frames.
    int detect(const uint8_t *bgr, int width, int height, float *bestConfidence = nullptr);
    EnrollmentState enroll(const uint8_t *bgr, int width, int height, int *progressOut);

    bool isEnrolled() const;

    // Serialised enrollment, for TemplateStore to encrypt. Empty on failure.
    std::string exportEnrollment() const;

    // Drop all enrolled data by rebuilding the handle with an empty store.
    bool clearEnrollment();

    bool ready() const { return handle_ != nullptr; }

private:
    void destroy();

    FaceAnalysisRecognition *handle_{nullptr};
    std::string modelDir_;
};
