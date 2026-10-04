#!/usr/bin/env bash

javac --release 8 -d . -h jni ./plugins/Quorum/Libraries/Game/Graphics/Models/Printing/Slicer.java # omit release on jdk where version == 8, builds .class files
x86_64-w64-mingw32-g++ -shared -o Slicer.dll Slicer.cpp jni/plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer.cpp # make windows dll
# implement com_example_Janus.h
# javac -d classes src/com/example/janus/*.java
# mkdir -p res/native && cp janus.dll res/native/
# jar cf janus.jar -C classes . -C res . # builds the jar

# testing
javac plugins/quorum/Libraries/Game/Graphics/Models/Printing/Slicer.java
java plugins/quorum/Libraries/Game/Graphics/Models/Printing/Slicer