#pragma once

#include <functional>
#include <string_view>

#define JNIEXPORT
#define JNICALL
using jlong = long long;
using jdouble = double;
using jboolean = unsigned char;
using jobject = void *;
using jclass = void *;
using jmethodID = const char *;
inline constexpr jboolean JNI_TRUE = 1;
inline std::function<void(std::string_view)> androidGyroscopeJavaCall;

struct JNIEnv {
  bool ExceptionCheck() { return false; }
  void ExceptionDescribe() {}
  void ExceptionClear() {}
  void DeleteLocalRef(void *) {}
  jclass GetObjectClass(jobject object) { return object; }
  jmethodID GetMethodID(jclass, const char *method, const char *) { return method; }
  jboolean CallBooleanMethod(jobject, jmethodID) { return JNI_TRUE; }
  void CallVoidMethod(jobject, jmethodID method) { androidGyroscopeJavaCall(method); }
};
