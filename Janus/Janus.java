package com.example.Janus;

public class Janus {
    static { NativeLoader.load("janus"); }   // see step 5
    public static native void printData();
}