#include "com_example_Janus_Janus.h"
#include "../Janus.h"

extern "C" {
	JNIEXPORT void JNICALL Java_com_example_Janus_printData(JNIEnv *env, jclass java_class) {
		PrintData();
		std::cout.flush();
	}
}