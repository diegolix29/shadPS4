package com.winlator.alsaserver;

import com.winlator.sysvshm.SysVSharedMemory;
import com.winlator.xconnector.ConnectedClient;
import com.winlator.xconnector.RequestHandler;
import com.winlator.xconnector.XConnectorEpoll;
import com.winlator.xconnector.XInputStream;
import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;

import java.io.IOException;
import java.nio.ByteBuffer;

public class ALSARequestHandler implements RequestHandler {
    private int maxSHMemoryId = 0;

    @Override
    public boolean handleRequest(ConnectedClient client) throws IOException {
        ALSAClient alsaClient = (ALSAClient)client.getTag();
        XInputStream inputStream = client.getInputStream();
        XOutputStream outputStream = client.getOutputStream();

        if (inputStream.available() < 5) return false;
        byte requestCode = inputStream.readByte();
        int requestLength = inputStream.readInt();

        switch (requestCode) {
            case RequestCodes.CLOSE:
                alsaClient.release();
                break;
            case RequestCodes.START:
                alsaClient.start();
                break;
            case RequestCodes.STOP:
                alsaClient.stop();
                break;
            case RequestCodes.PAUSE:
                alsaClient.pause();
                break;
            case RequestCodes.PREPARE:
                if (inputStream.available() < requestLength) return false;

                alsaClient.setChannels(inputStream.readByte());
                alsaClient.setDataType(ALSAClient.DataType.values()[inputStream.readByte()]);
                alsaClient.setSampleRate(inputStream.readInt());
                alsaClient.setBufferSize(inputStream.readInt());
                alsaClient.prepare();

                if (ALSAClient.USE_SHARED_MEMORY) createSharedMemory(alsaClient, outputStream);
                break;
            case RequestCodes.WRITE:
                ByteBuffer sharedBuffer = alsaClient.getSharedBuffer();
                if (sharedBuffer != null) {
                    copySharedBuffer(alsaClient, requestLength, outputStream);
                    alsaClient.writeDataToTrack(alsaClient.getAuxBuffer());
                    sharedBuffer.putInt(0, alsaClient.pointer());
                }
                else {
                    if (inputStream.available() < requestLength) return false;
                    alsaClient.writeDataToTrack(inputStream.readByteBuffer(requestLength));
                }
                break;
            case RequestCodes.DRAIN:
                alsaClient.drain();
                break;
            case RequestCodes.POINTER:
                try (XStreamLock lock = outputStream.lock()) {
                    outputStream.writeInt(alsaClient.pointer());
                }
                break;
            case RequestCodes.MIN_BUFFER_SIZE:
                byte channels = inputStream.readByte();
                ALSAClient.DataType dataType = ALSAClient.DataType.values()[inputStream.readByte()];
                int sampleRate = inputStream.readInt();
                int minBufferSize = ALSAClient.latencyMillisToBufferSize(alsaClient.options.latencyMillis, channels, dataType, sampleRate);

                try (XStreamLock lock = outputStream.lock()) {
                    outputStream.writeInt(minBufferSize);
                }
                break;
        }
        return true;
    }

    private void copySharedBuffer(ALSAClient alsaClient, int requestLength, XOutputStream outputStream) throws IOException {
        ByteBuffer sharedBuffer = alsaClient.getSharedBuffer();
        ByteBuffer auxBuffer = alsaClient.getAuxBuffer();

        auxBuffer.position(0).limit(requestLength);
        sharedBuffer.position(ALSAClient.BUFFER_OFFSET).limit(ALSAClient.BUFFER_OFFSET + requestLength);
        auxBuffer.put(sharedBuffer);

        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte((byte)1);
        }
    }

    private void createSharedMemory(ALSAClient alsaClient, XOutputStream outputStream) throws IOException {
        int shmSize = alsaClient.getBufferSizeInBytes() + ALSAClient.BUFFER_OFFSET;
        int fd = SysVSharedMemory.createMemoryFd("alsa-shm"+(++maxSHMemoryId), shmSize);

        if (fd >= 0) {
            ByteBuffer buffer = SysVSharedMemory.mapSHMSegment(fd, shmSize, 0, false);
            if (buffer != null) alsaClient.setSharedBuffer(buffer);
        }

        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte((byte)0);
            outputStream.setAncillaryFd(fd);
        }
        finally {
            if (fd >= 0) XConnectorEpoll.closeFd(fd);
        }
    }
}
