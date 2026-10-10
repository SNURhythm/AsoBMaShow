#pragma once

#include "NativeRawTouchInput.h"

// Retain the Android bridge API while sharing gesture ownership and UI delivery.
namespace input::android {
using native_touch::TouchEpoch;
using native_touch::TouchPhase;
using native_touch::RawTouchEvent;
using native_touch::UiTouchEvent;
using native_touch::UiCancellationBatch;
using native_touch::RawTouchRegistration;
using native_touch::kUiTouchQueueCapacity;
using native_touch::kMaximumUiTouchContacts;
using native_touch::isSdlFingerEvent;
}
