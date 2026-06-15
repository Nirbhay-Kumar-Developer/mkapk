#include <jni.h>
#include <string>

// External linkage linking straight to our localized pure C engine component
extern "C" {
    const char* get_low_level_c_string();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_polyglot_JavaBridge_stringFromNativeJNI(JNIEnv* env, jobject thiz) {
    // Collect the execution tracking data out from our C processing engine
    std::string core_c_message = get_low_level_c_string();
    std::string response = "[C++] Unified Native Pipeline Layer -> " + core_c_message;
    
    return env->NewStringUTF(response.c_str());
}