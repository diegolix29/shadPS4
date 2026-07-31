package com.winlator.xconnector;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class UnixSocketConfigTest {
    @Test
    public void createAbstractTagsPathForJniSafeNativeDecode() {
        UnixSocketConfig config = UnixSocketConfig.createAbstract("/tmp/.X11-unix/X0");
        assertEquals("abstract:/tmp/.X11-unix/X0", config.path);
    }

    @Test
    public void createAbstractRejectsRelativePath() {
        try {
            UnixSocketConfig.createAbstract("X0");
            throw new AssertionError("expected IllegalArgumentException");
        } catch (IllegalArgumentException expected) {
            assertTrue(expected.getMessage().contains("absolute"));
        }
    }
}
