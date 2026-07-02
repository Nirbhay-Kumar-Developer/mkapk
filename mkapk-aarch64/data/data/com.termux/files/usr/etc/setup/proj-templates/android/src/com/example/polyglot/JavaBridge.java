package com.example.polyglot;

public class JavaBridge {
    static {
        // Direct link to the shared objects output mapped inside native.cpp
        System.loadLibrary("native");
    }

    // JNI Native Function declaration mapping directly to our C++ translation unit
    public native String stringFromNativeJNI();

    public String getCompiledMessageFromJava() {
        String nativeOutput = stringFromNativeJNI();
        return "Java Bridge Interop -> " + nativeOutput;
    }
}