@REM using msvc compiler in visual studio developer prompts
call mkdir jni
call mkdir res\native
call javac -d . -h jni .\plugins\Quorum\Libraries\Game\Graphics\Models\Printing\Slicer.java
call cl Slicer.cpp jni\plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer.cpp \LD \DLL \EHsc
@REM call mkdir classes
@REM call jar cf Janus.jar -C . . -C res .
@REM call cp Janus.dll res\native
@REM call mkdir com\example\Janus
@REM call cp Janus.class classes\com\example\Janus
call javac plugins\quorum\Libraries\Game\Graphics\Models\Printing\Slicer.java
call java plugins\quorum\Libraries\Game\Graphics\Models\Printing\Slicer