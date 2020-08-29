
#define LOG_TAG "SFTest"

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <sys/resource.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <utils/threads.h>
#include <android-base/properties.h>
#include <stdio.h>

#include "sftest.h"

using namespace android;

int main(int argc, char *argv[])
{
    if (argc != 4) {
        printf("%s <1|2|3> <bg_hex> <rect_hex>\n", argv[0]);
        printf("1:hwui, 2:sw canvas, 3:sw memset\n");
        return -1;
    }

    int isHw = strtoul(argv[1], NULL, 10);
    uint32_t bgHex = strtoul(argv[2], NULL, 16);
    uint32_t rectHex = strtoul(argv[3], NULL, 16);

    ALOGD("using %s, bgHex = 0x%x, rectHex = 0x%x",
        isHw == DRAW_HWUI ? "hwui" : (isHw == DRAW_SW_CANVAS ? "sw canvas" : "sw memset"),
        bgHex, rectHex);
	
    sp<ProcessState> proc(ProcessState::self());
    ProcessState::self()->startThreadPool();

    sp<android::SFTest> sftest = new android::SFTest(isHw, bgHex, rectHex);

    // run on MainThread
    sftest->runOnThisThread();

    //IPCThreadState::self()->joinThreadPool();

    ALOGV("SFTest exit");
    return 0;
}
