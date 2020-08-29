	 
#define ATRACE_TAG ATRACE_TAG_ALWAYS

#define LOG_NDEBUG 0
#define LOG_TAG "SFTest"


#include <stdint.h>
#include <inttypes.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>
#include <time.h>

#include <cutils/atomic.h>
#include <cutils/properties.h>

#include <binder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <android-base/properties.h>

#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/DisplayInfo.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/eglext.h>

#include "sftest.h"

namespace android {

//============================HWUI Context==========================
SFTest::SFTestHwuiContext::SFTestHwuiContext(sp<Surface>& surface)
    :mRenderNode(NULL), mProxy(NULL),mRecordingCanvas(NULL) {
    mRenderNode = new uirenderer::RenderNode();

    // set clip to bounds
    if (mRenderNode->mutateStagingProperties().setClipToBounds(false))
        mRenderNode->setPropertyFieldsDirty(uirenderer::RenderNode::GENERIC);

    SFtestContextFactory factory;
    mProxy = new uirenderer::renderthread::RenderProxy(false, mRenderNode, &factory);
    mProxy->loadSystemProperties();
    mProxy->setSwapBehavior
        (uirenderer::renderthread::SwapBehavior::kSwap_discardBuffer);

    mProxy->initialize(surface);
    mProxy->setup(0, 0, 0);
    mProxy->setLightCenter((uirenderer::Vector3){0, 0, 0});
}

// With lock
Canvas* SFTest::SFTestHwuiContext::lockCanvas(int width, int height) {
    if (mRecordingCanvas == NULL)
        mRecordingCanvas = Canvas::create_recording_canvas(width, height, mRenderNode);
    else 
        mRecordingCanvas->resetRecording(width, height, mRenderNode);	

    return mRecordingCanvas;
}

// With lock
void SFTest::SFTestHwuiContext::unlockAndPost(Canvas* canvas) {
    if (canvas != mRecordingCanvas) {
        ALOGE("canvas object must be the same instance that "
                "was previously returned by lockCanvas");
    }

    // 1.mRenderNode.end, get display list we record
    uirenderer::DisplayList* displaylist = canvas->finishRecording();
    mRenderNode->setStagingDisplayList(displaylist);

    // 2.nHwuiDraw(mHwuiRenderer) -> post to render thread
    nsecs_t vsync = systemTime(CLOCK_MONOTONIC);
    uirenderer::UiFrameInfoBuilder(mProxy->frameInfo())
            .setVsync(vsync, vsync)
            .addFlag(uirenderer::FrameInfoFlags::SurfaceCanvas);
    mProxy->syncAndDrawFrame();
}

Canvas* SFTest::lockHardwareCanvas() {
    Mutex::Autolock lock(mMutex);
    if (mHwuiContext == NULL) {
        ALOGD("create SFTestHwuiContext");
        mHwuiContext = new SFTestHwuiContext(mFlingerSurface);
    }

    return mHwuiContext->lockCanvas(
            ANativeWindow_getWidth(mFlingerSurface.get()),
            ANativeWindow_getHeight(mFlingerSurface.get()));
}

void SFTest::unlockHardwareCanvasAndPost(Canvas* canvas) {
    Mutex::Autolock lock(mMutex);

    if (mHwuiContext != NULL) {
        mHwuiContext->unlockAndPost(canvas);
    } else {
        ALOGE("Oops, no HWUI Context...");
    }
}

//===========================SkiaCanvas Software=================================

static inline SkColorType convertPixelFormat(PixelFormat format) {
    /* note: if PIXEL_FORMAT_RGBX_8888 means that all alpha bytes are 0xFF, then
        we can map to kN32_SkColorType, and optionally call
        bitmap.setAlphaType(kOpaque_SkAlphaType) on the resulting SkBitmap
        (as an accelerator)
    */
    switch (format) {
    case PIXEL_FORMAT_RGBX_8888:    return kN32_SkColorType;
    case PIXEL_FORMAT_RGBA_8888:    return kN32_SkColorType;
    case PIXEL_FORMAT_RGBA_FP16:    return kRGBA_F16_SkColorType;
    case PIXEL_FORMAT_RGB_565:      return kRGB_565_SkColorType;
    default:                        return kUnknown_SkColorType;
    }
}

Canvas* SFTest::lockSoftwareCanvas(sp<Surface> surface, Rect* inDirtyRect) {
    if (!Surface::isValid(surface)) {
        ALOGE("Oops, surface is invalid");
        return NULL;
    }

    Rect dirtyRect(Rect::EMPTY_RECT);
    Rect* dirtyRectPtr = NULL;

    if (inDirtyRect) {
        dirtyRect.left   = inDirtyRect->left;
        dirtyRect.top    = inDirtyRect->top;
        dirtyRect.right  = inDirtyRect->right;
        dirtyRect.bottom = inDirtyRect->bottom;
        dirtyRectPtr = &dirtyRect;
    }

    ANativeWindow_Buffer outBuffer;
    status_t err = surface->lock(&outBuffer, dirtyRectPtr);
    if (err < 0) {
        ALOGE("invalid lock");
        return NULL;
    }

    SkImageInfo info =
        SkImageInfo::Make(outBuffer.width, outBuffer.height,
                          convertPixelFormat(outBuffer.format),
                          outBuffer.format == PIXEL_FORMAT_RGBX_8888
                              ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
    SkBitmap bitmap;
    ssize_t bpr = outBuffer.stride * bytesPerPixel(outBuffer.format);
    bitmap.setInfo(info, bpr);
    if (outBuffer.width > 0 && outBuffer.height > 0) {
        bitmap.setPixels(outBuffer.bits);
    } else {
        // be safe with an empty bitmap.
        bitmap.setPixels(NULL);
    }

    if (mSkiaCanvas != NULL)
        mSkiaCanvas->setBitmap(bitmap);
    else
        mSkiaCanvas = SkiaCanvas::create_canvas(bitmap);

    if (dirtyRectPtr) {
        mSkiaCanvas->clipRect(dirtyRect.left, dirtyRect.top,
                dirtyRect.right, dirtyRect.bottom, SkClipOp::kIntersect);
    }
   
    return mSkiaCanvas;
}

void SFTest::unlockSoftwareCanvasAndPost(sp<Surface> surface, Canvas *canvas) {
    if (!Surface::isValid(surface)) {
        ALOGE("Oops, surface is invalid");
        return;
    }

    // detach the canvas from the surface  
    canvas->setBitmap(SkBitmap());

    // unlock surface
    status_t err = surface->unlockAndPost();
    if (err < 0) {
        ALOGE("Oops, invalid unlockAndPost");
    }
}

//===========================draw api=================================

#define RECT_SIZE 30.0f

void SFTest::calcNextRectPosition() {
    // calc next position
    if (mUiData->mX + RECT_SIZE + mUiData->mSpeedX >= mUiData->mScreenWidth
        || mUiData->mX + mUiData->mSpeedX <= 0.0f) {
        mUiData->mSpeedX = -(mUiData->mSpeedX);
    }
    if (mUiData->mY + RECT_SIZE + mUiData->mSpeedY >= mUiData->mScreenHeigh
        || mUiData->mY + mUiData->mSpeedY <= 0.0f) {
        mUiData->mSpeedY = -(mUiData->mSpeedY);
    }

    mUiData->mX += mUiData->mSpeedX;
    mUiData->mY += mUiData->mSpeedY;
}

void SFTest::calcNextCirclePosition() {
    // calc next position
    if (mUiData->mX + RECT_SIZE + mUiData->mSpeedX >= mUiData->mScreenWidth
        || mUiData->mX - RECT_SIZE + mUiData->mSpeedX <= 0.0f) {
        mUiData->mSpeedX = -(mUiData->mSpeedX);
    }
    if (mUiData->mY + RECT_SIZE + mUiData->mSpeedY >= mUiData->mScreenHeigh
        || mUiData->mY - RECT_SIZE + mUiData->mSpeedY <= 0.0f) {
        mUiData->mSpeedY = -(mUiData->mSpeedY);
    }

    mUiData->mX += mUiData->mSpeedX;
    mUiData->mY += mUiData->mSpeedY;
}


void SFTest::drawNativeWindowSW() {
    ATRACE_CALL();
    ANativeWindow_Buffer buffer;
    ARect rect;
    ssize_t bpr;

    // dequeue buffer
    mFlingerSurface->lock(&buffer, &rect);

    // draw background
    bpr = buffer.stride * bytesPerPixel(buffer.format);
    android_memset32((uint32_t*)buffer.bits, mBgColor, bpr*buffer.height);

    // draw moving box
    for (int i = 0; i < (int)RECT_SIZE; i++) {
        uint64_t start_addr = (uint64_t)buffer.bits 
            + (uint64_t)(bpr*(mUiData->mY+i))
            + (uint64_t)((mUiData->mX)*bytesPerPixel(buffer.format));
        android_memset32((uint32_t*)start_addr,
            mRectColor, ((int)RECT_SIZE)*bytesPerPixel(buffer.format));
    }

    // queue buffer
    mFlingerSurface->unlockAndPost();

    calcNextRectPosition();
}

void SFTest::drawNativeWindowSWCanvas() {
    ATRACE_CALL();
    Canvas* canvas = lockSoftwareCanvas(mFlingerSurface, NULL);

    mUiData->mPaint.setColor(mRectColor);	
    //canvas->drawColor(mBgColor, SkBlendMode::kClear);
    canvas->drawColor(mBgColor, SkBlendMode::kSrc);
    //canvas->drawRect(mUiData->mX, mUiData->mY,
    //mUiData->mX + RECT_SIZE, mUiData->mY + RECT_SIZE, mUiData->mPaint);
    canvas->drawCircle(mUiData->mX, mUiData->mY, 30.0f, mUiData->mPaint);

    unlockSoftwareCanvasAndPost(mFlingerSurface, canvas);

    calcNextCirclePosition();
}

void SFTest::drawNativeWindowHW() {
    ATRACE_CALL();
    Canvas* canvas = lockHardwareCanvas();

    mUiData->mPaint.setColor(mRectColor);	
    canvas->drawColor(mBgColor, SkBlendMode::kSrc);
    //canvas->drawRect(mUiData->mX, mUiData->mY,
    //	mUiData->mX + RECT_SIZE, mUiData->mY + RECT_SIZE, mUiData->mPaint);
    canvas->drawCircle(mUiData->mX, mUiData->mY, RECT_SIZE, mUiData->mPaint);

    if (mHwuiContext == NULL) {
        ALOGE("Oops, no HWUI Context...");
        return;
    }
    unlockHardwareCanvasAndPost(canvas);

    calcNextCirclePosition();
}

//===========================SFTest===============================
SFTest::SFTest(int drawApi, uint32_t bg_color, uint32_t rect_color)
        : Thread(false), mSession(NULL), mLooper(NULL), mAgrapher(NULL)
        , mHwuiContext(NULL), mUiData(NULL)
        , mDrawApi(drawApi), mBgColor(bg_color), mRectColor(rect_color) {
    ATRACE_BEGIN("SurfaceComposerClient");
    mSession = new SurfaceComposerClient();
    ATRACE_END();
}

void SFTest::onFirstRef() {
    status_t err = mSession->linkToComposerDeath(this);
    ALOGE_IF(err, "linkToComposerDeath failed (%s) ", strerror(-err));
    if (err == NO_ERROR) {
        // run("SFTest", PRIORITY_DISPLAY);
    }
}

sp<SurfaceComposerClient> SFTest::session() const {
    return mSession;
}

void SFTest::binderDied(const wp<IBinder>&) {
    // woah, surfaceflinger died!
    ALOGD("SurfaceFlinger died, exiting...");

    // calling requestExit() is not enough here because the Surface code
    // might be blocked on a condition variable that will never be updated.
    kill( getpid(), SIGKILL );
    requestExit();
}

status_t SFTest::readyToRun() {
    ATRACE_BEGIN("getBuiltInDisplay");
    sp<IBinder> dtoken(SurfaceComposerClient::getBuiltInDisplay(
            ISurfaceComposer::eDisplayIdMain));
    ATRACE_END();
    DisplayInfo dinfo;

    ATRACE_BEGIN("getDisplayInfo");
    status_t status = SurfaceComposerClient::getDisplayInfo(dtoken, &dinfo);
    ATRACE_END();

    if (status)
        return -1;

    ATRACE_BEGIN("createSurface");
    mFlingerSurfaceControl = session()->createSurface(String8("SFTest"),
            // dinfo.w, dinfo.h, PIXEL_FORMAT_RGB_565);
             dinfo.w, dinfo.h, PIXEL_FORMAT_RGBA_8888);
            // 800, 1600, PIXEL_FORMAT_RGBA_8888);

    mFlingerSurface = mFlingerSurfaceControl->getSurface();
    ATRACE_END();

    ATRACE_BEGIN("setLayer");
    SurfaceComposerClient::Transaction t;
    t.setLayer(mFlingerSurfaceControl, 0x40000000)
        .apply();
    ATRACE_END();

    if (mUiData == NULL) {
        int32_t width = ANativeWindow_getWidth(mFlingerSurface.get());
        int32_t heigh = ANativeWindow_getHeight(mFlingerSurface.get());
        mUiData = new UiData(width, heigh, RECT_SIZE, RECT_SIZE, 5.0f, 3.0f);
    }
	
    return NO_ERROR;
}


void SFTest::runOnThisThread() {
    readyToRun();
    threadLoop();
}

void SFTest::waitMessage() {
    do {
        IPCThreadState::self()->flushCommands();
        int32_t ret = mLooper->pollOnce(-1);
        switch (ret) {
            case Looper::POLL_WAKE:
            case Looper::POLL_CALLBACK:
                continue;
            case Looper::POLL_ERROR:
                ALOGE("Looper::POLL_ERROR");
                continue;
            case Looper::POLL_TIMEOUT:
                // timeout (should not happen)
                continue;
            default:
                // should not happen
                ALOGE("Looper::pollOnce() returned unknown status %d", ret);
                continue;
        }
    } while (true);
}

void SFTest::SFtestFrameCallback(long frameTimeNanos, void* data) {
    SFTest* sftest = (SFTest *)data;

    if (sftest->mDrawApi == DRAW_HWUI)
        sftest->drawNativeWindowHW();
    else if (sftest->mDrawApi == DRAW_SW_CANVAS)
        sftest->drawNativeWindowSWCanvas();
    else
    sftest->drawNativeWindowSW();

    AChoreographer_postFrameCallback(
        sftest->mAgrapher, SFtestFrameCallback, data);
}

bool SFTest::threadLoop() {
    mLooper = Looper::prepare(0);

    mAgrapher= AChoreographer_getInstance();

    AChoreographer_postFrameCallback(
        mAgrapher, SFtestFrameCallback, this);

    waitMessage();

    return true;
}

}
; // namespace android
