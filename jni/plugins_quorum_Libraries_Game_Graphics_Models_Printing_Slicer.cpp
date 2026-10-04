#include "plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer.h"
#include "../Slicer.h"

extern "C" {
	// TODO: support quorum packed Number32BitArray arguments
	JNIEXPORT void JNICALL Java_plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer_Slice(JNIEnv *env, jclass janus_class) {
		Slice(NULL);
		std::cout.flush();
	}
}
