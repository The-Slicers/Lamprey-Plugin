#include "com_example_Janus_Janus.h"
#include "../Janus.h"

extern "C" {
	JNIEXPORT void JNICALL Java_Janus_Janus_PrintData(JNIEnv *env, jclass janus_class) {
		PrintData();
		std::cout.flush();
	}
}