// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "fart_engine.h"

#include <algorithm>

#include <glib.h>

namespace
{
// libfart requires exactly one of data_dir / enrollment_json to be non-NULL.
// We always take the in-memory route so no plaintext template ever reaches the
// filesystem - TemplateStore owns persistence and encrypts it.
//
// The payload is a bare JSON array of floats (the normalised embedding), not an
// object. FaceDetector::import_enrollment_json() treats an *empty string* as
// "nothing enrolled" and succeeds; anything that fails to parse as an array -
// "{}" included - makes fart_create() throw. So the not-yet-enrolled case is the
// empty string, and a non-null pointer to it still selects JSON mode.
// Verified on device: passing "{}" fails with
//   Error in fart_create: Failed to import enrollment JSON
constexpr const char *kEmptyEnrollment = "";
} // namespace

FartEngine::~FartEngine() { destroy(); }

void FartEngine::destroy()
{
    if (handle_)
    {
        fart_destroy(handle_);
        handle_ = nullptr;
    }
}

bool FartEngine::init(const std::string &modelDir, const std::string &enrollmentJson)
{
    destroy();
    modelDir_ = modelDir;

    const std::string detection   = modelDir + "/detect-class1.tflite";
    const std::string recognition = modelDir + "/mobile_face_net.tflite";
    const std::string antiSpoof   = modelDir + "/mini_fas_net_v2.tflite";

    const std::string json = enrollmentJson.empty() ? std::string(kEmptyEnrollment) : enrollmentJson;

    handle_ = fart_create(detection.c_str(), recognition.c_str(), antiSpoof.c_str(),
                          /* data_dir */ nullptr, json.c_str());
    if (!handle_)
    {
        g_warning("fart_create failed (models in %s)", modelDir.c_str());
        return false;
    }

    g_message("libfart ready, enrolled=%s", fart_is_enrolled(handle_) ? "yes" : "no");
    return true;
}

int FartEngine::detect(const uint8_t *bgr, int width, int height, float *bestConfidence)
{
    if (bestConfidence)
        *bestConfidence = 0.0f;
    if (!handle_ || !bgr)
        return 0;

    Face *faces = fart_detect(handle_, bgr, width, height, 3);
    if (!faces)
        return 0;

    // face_count is carried on the first element; the array itself is only
    // valid when that count is non-zero.
    const int count = faces[0].face_count;
    if (bestConfidence)
    {
        for (int i = 0; i < count; ++i)
            *bestConfidence = std::max(*bestConfidence, faces[i].confidence);
    }
    fart_free_faces(faces);
    return count;
}

RecognitionState FartEngine::recognize(const uint8_t *bgr, int width, int height)
{
    if (!handle_)
        return RECOGNITION_FAIL;
    return fart_recognize(handle_, bgr, width, height, 3 /* BGR */);
}

EnrollmentState FartEngine::enroll(const uint8_t *bgr, int width, int height, int *progressOut)
{
    if (!handle_)
        return ENROLLMENT_FAIL;
    return fart_enroll(handle_, bgr, width, height, 3 /* BGR */, progressOut);
}

bool FartEngine::isEnrolled() const { return handle_ && fart_is_enrolled(handle_) == 1; }

std::string FartEngine::exportEnrollment() const
{
    if (!handle_)
        return {};

    char *json = fart_export_enrollment_json(handle_);
    if (!json)
        return {};

    std::string out(json);
    fart_free_string(json);
    return out;
}

bool FartEngine::clearEnrollment() { return init(modelDir_, kEmptyEnrollment); }
