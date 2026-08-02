package com.winlator.renderer;

import androidx.annotation.Keep;

import com.winlator.xserver.Drawable;

import java.nio.ByteBuffer;

public class GPUImage extends Texture {
    private long hardwareBufferPtr;
    private long imageKHRPtr;
    private ByteBuffer virtualData;
    private short stride;
    private boolean locked = false;
    private int nativeHandle;

    static {
        System.loadLibrary("winlator");
    }

    public GPUImage(Drawable owner) {
        this(owner, true, true);
    }

    public GPUImage(Drawable owner, boolean cpuAccess) {
        this(owner, cpuAccess, true);
    }

    public GPUImage(Drawable owner, boolean cpuAccess, boolean useHALPixelFormatBGRA8888) {
        super(owner);
        hardwareBufferPtr = createHardwareBuffer(owner.width, owner.height, cpuAccess, useHALPixelFormatBGRA8888);
        if (cpuAccess && hardwareBufferPtr != 0) {
            virtualData = lockHardwareBuffer(hardwareBufferPtr);
            locked = true;
        }
    }

    @Override
    public void allocateTexture(short width, short height, ByteBuffer data) {
        if (isAllocated()) return;
        super.allocateTexture(width, height, null);
        imageKHRPtr = createImageKHR(hardwareBufferPtr, textureId);
    }

    @Override
    public void updateFromDrawable() {
        if (!isAllocated() && owner != null) allocateTexture(owner.width, owner.height, null);
        needsUpdate = false;
    }

    public short getStride() {
        return stride;
    }

    @Keep
    private void setStride(short stride) {
        this.stride = stride;
    }

    public int getNativeHandle() {
        return nativeHandle;
    }

    @Keep
    private void setNativeHandle(int nativeHandle) {
        this.nativeHandle = nativeHandle;
    }

    public ByteBuffer getVirtualData() {
        return virtualData;
    }

    /**
     * Release the permanent CPU lock taken at construction so Vulkan can write the AHB.
     * After this call {@link #getVirtualData()} is null; Canvas compositors must copy via lock.
     */
    public void releaseCpuLock() {
        if (locked && hardwareBufferPtr != 0) {
            unlockHardwareBuffer(hardwareBufferPtr);
            locked = false;
            virtualData = null;
        }
    }

    public boolean isLocked() {
        return locked;
    }

    @Override
    public void destroy() {
        destroyImageKHR(imageKHRPtr);
        destroyHardwareBuffer(hardwareBufferPtr, locked);
        virtualData = null;
        imageKHRPtr = 0;
        hardwareBufferPtr = 0;
        super.destroy();
    }

    public long getHardwareBufferPtr() {
        return hardwareBufferPtr;
    }

    private native long createHardwareBuffer(short width, short height, boolean cpuAccess, boolean useHALPixelFormatBGRA8888);

    private native void destroyHardwareBuffer(long hardwareBufferPtr, boolean locked);

    private native ByteBuffer lockHardwareBuffer(long hardwareBufferPtr);

    private native void unlockHardwareBuffer(long hardwareBufferPtr);

    private native long createImageKHR(long hardwareBufferPtr, int textureId);

    private native void destroyImageKHR(long imageKHRPtr);
}
