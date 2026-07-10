package com.snurhythm.asobmashow;

import static org.junit.Assert.assertTrue;

import java.lang.reflect.Method;
import java.util.Arrays;
import org.junit.Test;

public class AndroidPrivateStorageBridgeTest {
    @Test
    public void activityExposesCacheDirectoryPathToNativeCode() {
        Method[] methods = AsoBMaShowActivity.class.getMethods();
        boolean found = Arrays.stream(methods).anyMatch(method ->
                method.getName().equals("getCacheDirPath") &&
                        method.getParameterCount() == 0 &&
                        method.getReturnType().equals(String.class));

        assertTrue("Activity must expose the private cache path to JNI", found);
    }
}
