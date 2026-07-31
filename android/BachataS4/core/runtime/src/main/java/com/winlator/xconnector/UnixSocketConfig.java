package com.winlator.xconnector;

import java.io.File;

public class UnixSocketConfig {
    public static final String SYSVSHM_SERVER_PATH = "/tmp/.sysvshm/SM0";
    public static final String ALSA_SERVER_PATH = "/tmp/.sound/AS0";
    public static final String PULSE_SERVER_PATH = "/tmp/.sound/PS0";
    public static final String XSERVER_PATH = "/tmp/.X11-unix/X0";
    public static final String VIRGL_SERVER_PATH = "/tmp/.virgl/V0";
    public static final String VORTEK_SERVER_PATH = "/tmp/.vortek/V0";
    public final String path;

    private UnixSocketConfig(String path) {
        this.path = path;
    }

    public static UnixSocketConfig create(String rootPath, String relativePath) {
        File socketFile = new File(rootPath, relativePath);

        File socketDir = socketFile.getParentFile();
        if (socketDir != null) {
            deleteRecursively(socketDir);
            socketDir.mkdirs();
        }
        else socketFile.delete();

        return new UnixSocketConfig(socketFile.getPath());
    }

    public static UnixSocketConfig createAbstract(String path) {
        if (!path.startsWith("/")) throw new IllegalArgumentException("Abstract X11 path must be absolute: " + path);
        return new UnixSocketConfig("abstract:" + path);
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        file.delete();
    }
}
