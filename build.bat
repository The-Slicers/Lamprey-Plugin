# using msvc compiler in visual studio developer prompts
call cl Janus.cpp /LD /DLL /EHsc
@REM call mkdir classes
call mkdir jni
call javac -d . -h jni .\Janus\Janus.java
call mkdir res\\native
@REM call jar cf Janus.jar -C . . -C res .
call cp Janus.dll res\native
@REM call mkdir com\example\Janus
call cp Janus.class classes\com\example\Janus
call javac Main.java
call java Main