/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef ANDROID_SFTest_H
#define ANDROID_SFTest_H

#include <stdint.h>
#include <sys/types.h>

#include <utils/Thread.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include <android/choreographer.h>
#include <android/native_window.h>
#include <utils/Looper.h>

#include <RenderNode.h>
#include <renderthread/RenderProxy.h>
#include <AnimationContext.h>
#include <Canvas.h>
#include <SkiaCanvas.h>
#include <Paint.h>
#include <FrameInfo.h>
#include <SkBlendMode.h>

#include <android/native_window.h>
#include <cutils/memory.h>

#include <cutils/trace.h>
#include <utils/Trace.h>

#define DRAW_HWUI 1
#define DRAW_SW_CANVAS 2
#define DRAW_SW_MEMSET 3

namespace android {

class Surface;
class SurfaceComposerClient;
class SurfaceControl;
class AnimationContext;

// ---------------------------------------------------------------------------

class SFtestContextFactory : public uirenderer::IContextFactory {
public:
    virtual uirenderer::AnimationContext* createAnimationContext
        (uirenderer::renderthread::TimeLord& clock) {
        return new uirenderer::AnimationContext(clock);
    }
};

class SFTest: public Thread, public IBinder::DeathRecipient
{
public:
    class SFTestHwuiContext {
    public:
        SFTestHwuiContext(sp<Surface>& surface);
        Canvas* lockCanvas(int width, int height);
        void unlockAndPost(Canvas* canvas);

        uirenderer::RenderNode* mRenderNode;
        uirenderer::renderthread::RenderProxy* mProxy;
        // Base Class (SkiaRecordingCanvas)
        Canvas* mRecordingCanvas;
    };

    class UiData {
    public:
        UiData(uint32_t width, uint32_t heigh, int x,
            int y, float speedX, float speedY)
            :mScreenWidth(width),mScreenHeigh(heigh),mX(x),
            mY(y),mSpeedX(speedX),mSpeedY(speedY),mPaint(){};

        uint32_t mScreenWidth;
        uint32_t mScreenHeigh;
        float mX;
        float mY;
        float mSpeedX;
        float mSpeedY;
        Paint mPaint;
    };

public:    
    SFTest(int hwAccelerate, uint32_t bg_color, uint32_t rect_color);
    ~SFTest() = default;

    sp<SurfaceComposerClient> session() const;
    virtual bool        threadLoop();
    virtual status_t    readyToRun();
    virtual void        onFirstRef();
    virtual void        binderDied(const wp<IBinder>& who);
    void waitMessage();
    virtual void        runOnThisThread();
    
    Canvas* lockHardwareCanvas();
    void unlockHardwareCanvasAndPost(Canvas* canvas);
    void drawNativeWindowSW();
    void drawNativeWindowSWCanvas();
    void drawNativeWindowHW();

    Canvas* lockSoftwareCanvas(sp<Surface> surface, Rect* inDirtyRect);
    void unlockSoftwareCanvasAndPost(sp<Surface> surface, Canvas *canvas);

    static void SFtestFrameCallback(long frameTimeNanos, void* data);
    void calcNextRectPosition();
    void calcNextCirclePosition();
    
    sp<SurfaceComposerClient> mSession;
    sp<Looper> mLooper;
    AChoreographer *mAgrapher;
    
    sp<SurfaceControl> mFlingerSurfaceControl;
    sp<Surface> mFlingerSurface;

    mutable Mutex mMutex;
    SFTestHwuiContext* mHwuiContext;

    UiData *mUiData;
    int mDrawApi;
    uint32_t mBgColor;
    uint32_t mRectColor;

    // for reuse SkiaCanvas
    Canvas* mSkiaCanvas; 

};

// ---------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_SFTest_H
