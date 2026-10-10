#pragma once
#include <jni.h>
inline JNIEnv androidGyroscopeTestEnv;
inline void *SDL_GetAndroidJNIEnv() { return &androidGyroscopeTestEnv; }
inline void *SDL_GetAndroidActivity() { return &androidGyroscopeTestEnv; }
