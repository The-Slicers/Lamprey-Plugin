#!/usr/env/bin bash

x86_64-w64-mingw32-g++ -shared -o janus.dll Janus.cpp # make windows dll
javac --release 8 -d classes -h jni src/com/example/*.java # omit release on jdk where version == 8, builds .class files
# implement com_example_Janus.h
# javac -d classes src/com/example/janus/*.java
mkdir -p res/native && cp janus.dll res/native/
jar cf janus.jar -C classes . -C res . # builds the jar

# testing
javac Main.java
java Main