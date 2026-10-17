luneos-faced
============

Summary
-------
Face authentication for the LuneOS lockscreen, served as `com.webos.service.faceunlock`.

Description
-----------
Pulls frames from `com.webos.service.camera2` into shared memory without a
preview, runs them through libfart, and reports whether the face in front of the
camera matches the enrolled one.

## What this is, and what it is not

This is a **sensor adapter**, the same role `webos-fingerprint-adapter` plays for
the fingerprint HAL. It answers one question — "does the face in front of the
camera match the enrolled one?" — and nothing else.

It deliberately does **not**:

- decide whether the device unlocks,
- own a retry budget or lockout state,
- know anything about the passcode.

That is authentication *policy*, and it belongs in `luna-authmanager`, the
`Security` + `EASPolicyManager` carve-out from luna-sysmgr. Today that policy is
spread across `LockScreen.qml` (`maxFingerprintAttempts = 5`) and `Security.cpp`
(`s_defaultMaxRetries = 3`), which do not know about each other. Adding face as a
third independent counter makes that worse, which is why **face unlock should not
ship enabled by default until luna-authmanager exists.**

## Pipeline

```
com.webos.service.camera2   open(front) -> setFormat(640x480) -> startCamera
        |                   getFd("buffer") + getFd("signal")
        v
CameraSharedMemory          read() -> NV21 (droid HAL) or YUY2 (v4l2)
        v
cv::cvtColor                -> BGR24
        v
libfart                     detect -> MiniFASNet PAD -> MobileFaceNet -> match
        v
TemplateStore               AES-256-GCM enrollment blob
```

Frames never touch the filesystem, and the enrollment template is encrypted at
rest — a face embedding is invertible back into a recognisable face image, so a
leaked template is close to leaking both the user's face and an unlock credential.

## Security posture: this is Android "Class 1"

With an RGB-only front camera, no TEE and no trusted capture path, a well-built
face unlock lands at roughly Android's **Class 1 / Convenience** tier. Class 2 and
above require that embeddings never reach the application processor outside a TEE
and that the pipeline survive a kernel compromise; a userspace matcher reading
GStreamer frames satisfies neither, and vendor-signed trustlets mean we cannot
move the matcher into the TEE on these devices.

So: face unlock is a convenience layer. It must never gate keystore material or
payments, and the UI should say plainly that it is weaker than the PIN.

Presentation-attack detection today is libfart's passive MiniFASNet only, which
defeats casual print and replay attacks but generalises poorly to attack
materials outside its training set. The planned addition is an **active
screen-flash challenge** — a pre-recorded video cannot contain the reflection of
a colour chosen after it was recorded, which is a structural defence rather than
a statistical one.

## Methods

| Method | Subscribable | Notes |
|---|---|---|
| `getStatus` | yes | `available`, `enrolled`, `state` |
| `enroll` | yes | streams `progress` 0-100 and a `hint` (`noFace`, `multipleFaces`, `badLighting`) |
| `identify` | yes | one time-boxed attempt (3 s), replies `recognized` |
| `abort` | no | cancels the running operation |
| `clear` | no | removes the enrollment |

ACGs: `faceunlock.operation` (all methods), `faceunlock.identify`
(`getStatus`, `identify`, `abort`) and `faceunlock.query` (`getStatus`).
`com.webos.surfacemanager*` is granted `faceunlock.operation` and
`faceunlock.query`, as with fingerprint. An app that only lets its user in
with a face asks for `faceunlock.identify`, which cannot enroll or clear.

Identify updates go to every subscriber of `identify`, so such an app keeps
its subscription only while it is in front, and cancels it (and `abort`s)
when it is not.

`identify` does not distinguish a genuine mismatch from a spoof rejection.
libfart does not report the difference, and telling an attacker which one it was
would be a gift.

## Build dependencies

`luna-service2`, `glib-2.0`, `opencv`, `openssl`, `nlohmann-json`, `libfart`
(GPL-2.0-only, so this daemon is too) and `camera-utils` for
`CameraSharedMemory` / `LunaClient`.

## Known gaps

- **Camera arbitration.** If the camera app holds the device when the screen
  locks, `open` fails. The daemon fails soft and the shell must fall back to the
  PIN rather than presenting face unlock as broken.
- **Template key.** The AES key currently sits in a root-only file next to the
  ciphertext. It must move to passcode-derived material owned by
  `luna-authmanager`, ideally wrapped by gatekeeper/keymaster on Halium. That
  also gives "PIN required after boot" for free.
- **`fart_create` with no prior enrollment.** We pass `{}` for the not-yet-enrolled
  case; needs verifying against libfart on device.
- **camera2 outbound permissions.** If ls-hubd logs `LSHUB_NO_OUT_PERMS` for
  `com.webos.service.faceunlock`, add it to `outbound` in
  `com.webos.service.camera2.role.json.in`, exactly as LuneOS already does for
  `com.webos.app.camera-*`.
