package com.bachatas4.android.runtime.vortek;

import android.util.Log;

import androidx.annotation.Keep;
import androidx.annotation.Nullable;

import com.winlator.renderer.GPUImage;
import com.winlator.renderer.Texture;
import com.winlator.xserver.Drawable;
import com.winlator.xserver.Window;
import com.winlator.xserver.XServer;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * JNI window bridge matching Winlator {@code VortekRendererComponent} method signatures.
 * Called from the Vortek request-handler thread for WSI extent/AHB/present.
 */
public final class VortekWindowBridge {
    private static final String TAG = "Bachata.Vortek";

    private final XServer xServer;
    private final Object lock = new Object();

    static {
        System.loadLibrary("bachata_vortek_server");
    }

    public VortekWindowBridge(XServer xServer) {
        this.xServer = xServer;
    }

    public XServer getXServer() {
        return xServer;
    }

    @Keep
    public int getWindowWidth(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        return window != null ? window.getWidth() : 0;
    }

    @Keep
    public int getWindowHeight(int windowId) {
        Window window = xServer.windowManager.getWindow(windowId);
        return window != null ? window.getHeight() : 0;
    }

    /**
     * Ensure the window drawable has a GPUImage-backed AHardwareBuffer and return its pointer.
     * Matches Winlator Vortek signature {@code (IZ)J}.
     */
    @Keep
    public long getWindowHardwareBuffer(int windowId, boolean useHALPixelFormatBGRA8888) {
        synchronized (lock) {
            Window window = xServer.windowManager.getWindow(windowId);
            if (window == null) {
                Log.w(TAG, "getWindowHardwareBuffer missing windowId=" + windowId);
                return 0L;
            }
            Drawable drawable = window.getContent();
            if (drawable == null) {
                Log.w(TAG, "getWindowHardwareBuffer no content windowId=" + windowId);
                return 0L;
            }
            Texture texture = drawable.getTexture();
            if (!(texture instanceof GPUImage)) {
                if (texture != null) {
                    // Texture.destroy uses GLES; safe when no GL context (no-op if unallocated).
                    try {
                        texture.destroy();
                    } catch (Throwable ignored) {
                        // Best-effort; new GPUImage replaces the reference.
                    }
                }
                // cpuAccess=true so AHB has CPU_READ for Canvas present path.
                GPUImage gpuImage = new GPUImage(drawable, true, useHALPixelFormatBGRA8888);
                // Unlock permanent map so Vulkan import can write the buffer.
                gpuImage.releaseCpuLock();
                // Preserve a dedicated CPU buffer for SurfaceWindowRenderer.
                ensureCpuBuffer(drawable);
                drawable.setTexture(gpuImage);
                Log.i(TAG, "hardware_buffer=resolved windowId=" + windowId
                        + " size=" + drawable.width + "x" + drawable.height
                        + " bgra=" + useHALPixelFormatBGRA8888);
            }
            GPUImage gpuImage = (GPUImage) drawable.getTexture();
            long ptr = gpuImage.getHardwareBufferPtr();
            if (ptr != 0L) {
                Log.i(TAG, "hardware_buffer=resolved windowId=" + windowId);
            }
            return ptr;
        }
    }

    /**
     * After guest present: copy AHB pixels into drawable CPU buffer for the Canvas compositor.
     */
    @Keep
    public void updateWindowContent(int windowId) {
        synchronized (lock) {
            Window window = xServer.windowManager.getWindow(windowId);
            if (window == null) return;
            Drawable drawable = window.getContent();
            if (drawable == null) return;
            synchronized (drawable.renderLock) {
                Texture texture = drawable.getTexture();
                if (texture instanceof GPUImage) {
                    long ptr = ((GPUImage) texture).getHardwareBufferPtr();
                    ByteBuffer data = ensureCpuBuffer(drawable);
                    if (ptr != 0L && data != null) {
                        int rc = nativeCopyAhbToBuffer(
                                ptr,
                                data,
                                drawable.width,
                                drawable.height
                        );
                        if (rc != 0) {
                            Log.w(TAG, "ahb_copy_failed windowId=" + windowId + " rc=" + rc);
                        }
                    }
                }
                drawable.forceUpdate();
            }
        }
    }

    private static ByteBuffer ensureCpuBuffer(Drawable drawable) {
        ByteBuffer data = drawable.getData();
        int need = drawable.width * drawable.height * 4;
        if (data == null || !data.isDirect() || data.capacity() < need) {
            data = ByteBuffer.allocateDirect(need).order(ByteOrder.LITTLE_ENDIAN);
            drawable.setData(data);
        }
        return data;
    }

    private static native int nativeCopyAhbToBuffer(
            long hardwareBufferPtr,
            ByteBuffer directBuffer,
            int expectedWidth,
            int expectedHeight
    );
}
