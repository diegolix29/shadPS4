package com.winlator.alsaserver;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

import com.winlator.core.KeyValueSet;
import com.winlator.math.Mathf;
import com.winlator.sysvshm.SysVSharedMemory;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ALSAClient {
    private static final float DEFAULT_VOLUME = 1.0f;
    private static final short DEFAULT_LATENCY_MILLIS = 16;
    public static final boolean USE_SHARED_MEMORY = true;
    public static final byte BUFFER_OFFSET = 4;
    public enum DataType {
        U8(1), S16LE(2), S16BE(2), FLOATLE(4), FLOATBE(4);
        public final byte byteCount;

        DataType(int byteCount) {
            this.byteCount = (byte)byteCount;
        }
    }
    private DataType dataType = DataType.U8;
    private AudioTrack audioTrack = null;
    private byte channels = 2;
    private int sampleRate = 0;
    private int position;
    private int bufferSize;
    private byte frameBytes;
    private short previousUnderrunCount = 0;
    private int bufferCapacity;
    private ByteBuffer sharedBuffer;
    private ByteBuffer auxBuffer;
    protected final Options options;
    private static short framesPerBuffer = 256;

    public static class Options {
        public short latencyMillis = DEFAULT_LATENCY_MILLIS;
        public byte performanceMode = AudioTrack.PERFORMANCE_MODE_LOW_LATENCY;
        public float volume = DEFAULT_VOLUME;
        public boolean useSharedMemory = true;

        public static Options fromKeyValueSet(KeyValueSet config) {
            if (config == null || config.isEmpty()) return new Options();
            Options options = new Options();
            switch (config.get("performanceMode")) {
                case "0" :
                    options.performanceMode = AudioTrack.PERFORMANCE_MODE_NONE;
                    break;
                case "1" :
                    options.performanceMode = AudioTrack.PERFORMANCE_MODE_LOW_LATENCY;
                    break;
                case "2" :
                    options.performanceMode = AudioTrack.PERFORMANCE_MODE_POWER_SAVING;
                    break;
            }

            options.volume = config.getFloat("volume", DEFAULT_VOLUME);
            options.latencyMillis = (short)config.getInt("latencyMillis", DEFAULT_LATENCY_MILLIS);
            return options;
        }
    }

    public ALSAClient(Options options) {
        this.options = options;
    }

    public void release() {
        if (sharedBuffer != null) {
            SysVSharedMemory.unmapSHMSegment(sharedBuffer, sharedBuffer.capacity());
            sharedBuffer = null;
        }

        if (audioTrack != null) {
            audioTrack.pause();
            audioTrack.flush();
            audioTrack.release();
            audioTrack = null;
        }
    }

    public static int getPCMEncoding(DataType dataType) {
        switch (dataType) {
            case U8:
                return AudioFormat.ENCODING_PCM_8BIT;
            case S16LE:
            case S16BE:
                return AudioFormat.ENCODING_PCM_16BIT;
            case FLOATLE:
            case FLOATBE:
                return AudioFormat.ENCODING_PCM_FLOAT;
            default:
                return AudioFormat.ENCODING_DEFAULT;
        }
    }

    public static int getChannelConfig(int channels) {
        return channels <= 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
    }

    public void prepare() {
        position = 0;
        previousUnderrunCount = 0;
        frameBytes = (byte)(channels * dataType.byteCount);
        release();

        if (!isValidBufferSize()) return;

        AudioFormat format = new AudioFormat.Builder()
            .setEncoding(getPCMEncoding(dataType))
            .setSampleRate(sampleRate)
            .setChannelMask(getChannelConfig(channels))
            .build();

        audioTrack = new AudioTrack.Builder()
            .setPerformanceMode(options.performanceMode)
            .setAudioFormat(format)
            .setBufferSizeInBytes(getBufferSizeInBytes())
            .build();

        bufferCapacity = audioTrack.getBufferCapacityInFrames();
        if (options.volume != 1.0f) audioTrack.setVolume(options.volume);
        audioTrack.play();
    }

    public void start() {
        if (audioTrack != null && audioTrack.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) {
            audioTrack.play();
        }
    }

    public void stop() {
        if (audioTrack != null) {
            audioTrack.stop();
            audioTrack.flush();
        }
    }

    public void pause() {
        if (audioTrack != null) audioTrack.pause();
    }

    public void drain() {
        if (audioTrack != null) audioTrack.flush();
    }

    public void writeDataToTrack(ByteBuffer data) {
        if (dataType == DataType.S16LE || dataType == DataType.FLOATLE) {
            data.order(ByteOrder.LITTLE_ENDIAN);
        }
        else if (dataType == DataType.S16BE || dataType == DataType.FLOATBE) {
            data.order(ByteOrder.BIG_ENDIAN);
        }

        if (audioTrack != null) {
            int bytesWritten;
            data.position(0);

            do {
                try {
                    bytesWritten = audioTrack.write(data, data.remaining(), AudioTrack.WRITE_BLOCKING);
                    if (bytesWritten < 0) break;
                    increaseBufferSizeIfUnderrunOccurs();
                }
                catch (Exception e) {
                    break;
                }
            }
            while (data.position() != data.limit());

            position += data.position();
            data.rewind();
        }
    }

    private void increaseBufferSizeIfUnderrunOccurs() {
        int underrunCount = audioTrack.getUnderrunCount();
        if (underrunCount > previousUnderrunCount && bufferSize < bufferCapacity) {
            previousUnderrunCount = (short)underrunCount;
            bufferSize += framesPerBuffer;
            audioTrack.setBufferSizeInFrames(bufferSize);
        }
    }

    public int pointer() {
        return audioTrack != null ? position / frameBytes : 0;
    }

    public void setDataType(DataType dataType) {
        this.dataType = dataType;
    }

    public void setChannels(int channels) {
        this.channels = (byte)channels;
    }

    public void setSampleRate(int sampleRate) {
        this.sampleRate = sampleRate;
    }

    public void setBufferSize(int bufferSize) {
        this.bufferSize = bufferSize;
    }

    public ByteBuffer getSharedBuffer() {
        return sharedBuffer;
    }

    public void setSharedBuffer(ByteBuffer sharedBuffer) {
        if (sharedBuffer != null) {
            auxBuffer = ByteBuffer.allocateDirect(getBufferSizeInBytes()).order(ByteOrder.LITTLE_ENDIAN);
            this.sharedBuffer = sharedBuffer.order(ByteOrder.LITTLE_ENDIAN);
        }
        else {
            auxBuffer = null;
            this.sharedBuffer = null;
        }
    }

    public ByteBuffer getAuxBuffer() {
        return auxBuffer;
    }

    public DataType getDataType() {
        return dataType;
    }

    public byte getChannels() {
        return channels;
    }

    public int getSampleRate() {
        return sampleRate;
    }

    public int getBufferSize() {
        return bufferSize;
    }

    public int getBufferSizeInBytes() {
        return bufferSize * frameBytes;
    }

    public static int bufferSizeToLatencyMillis(int bufferSizeInBytes, int channels, DataType dataType, int sampleRate) {
        byte frameBytes = (byte)(channels * dataType.byteCount);
        float bufferSize = (float)bufferSizeInBytes / frameBytes;
        return (int)((bufferSize / sampleRate) * 1000);
    }

    public static int latencyMillisToBufferSize(int latencyMillis, int channels, DataType dataType, int sampleRate) {
        byte frameBytes = (byte)(channels * dataType.byteCount);
        int bufferSize = (int)Mathf.roundTo((latencyMillis * sampleRate) / 1000.0f, framesPerBuffer, false);
        return bufferSize * frameBytes;
    }

    private boolean isValidBufferSize() {
        return ((bufferSize % frameBytes) == 0) && bufferSize > 0;
    }

    public static void assignFramesPerBuffer(Context context) {
        try {
            AudioManager am = (AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
            String framesPerBufferStr = am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
            framesPerBuffer = Short.parseShort(framesPerBufferStr);
            if (framesPerBuffer == 0) framesPerBuffer = 256;
        }
        catch (Exception e) {
            framesPerBuffer = 256;
        }
    }
}
