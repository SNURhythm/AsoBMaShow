package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiDeviceStatus;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

final class AsoBMaShowMidiManager {
    private final Activity activity;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Map<Integer, DeviceConnection> connections = new HashMap<>();
    private final Set<Integer> pendingDeviceIds = new HashSet<>();

    private MidiManager midiManager;
    private MidiManager.DeviceCallback deviceCallback;
    private boolean started;
    private long generation;

    AsoBMaShowMidiManager(Activity activity) {
        this.activity = activity;
    }

    synchronized String start() {
        if (started) {
            return "ok";
        }
        Object service = activity.getSystemService(Context.MIDI_SERVICE);
        if (!(service instanceof MidiManager)) {
            return "Android MIDI service is unavailable.";
        }
        midiManager = (MidiManager) service;
        started = true;
        generation++;
        final long callbackGeneration = generation;
        deviceCallback = new MidiManager.DeviceCallback() {
            @Override
            public void onDeviceAdded(MidiDeviceInfo info) {
                scheduleOpen(info, callbackGeneration);
            }

            @Override
            public void onDeviceRemoved(MidiDeviceInfo info) {
                removeDevice(info.getId(), true);
            }

            @Override
            public void onDeviceStatusChanged(MidiDeviceStatus status) {
                // Port availability is represented by device add/remove and open callbacks.
            }
        };
        try {
            midiManager.registerDeviceCallback(deviceCallback, mainHandler);
            for (MidiDeviceInfo info : midiManager.getDevices()) {
                scheduleOpen(info, callbackGeneration);
            }
        } catch (RuntimeException exception) {
            stopLocked();
            String message = exception.getMessage();
            return message == null || message.isEmpty()
                    ? "Android MIDI initialization failed."
                    : "Android MIDI initialization failed: " + message;
        }
        return "ok";
    }

    synchronized void stop() {
        stopLocked();
    }

    private void stopLocked() {
        started = false;
        generation++;
        if (midiManager != null && deviceCallback != null) {
            try {
                midiManager.unregisterDeviceCallback(deviceCallback);
            } catch (RuntimeException ignored) {
                // The MIDI service may already be shutting down with the activity.
            }
        }
        deviceCallback = null;
        pendingDeviceIds.clear();
        for (DeviceConnection connection : new ArrayList<>(connections.values())) {
            connection.close(false);
        }
        connections.clear();
        midiManager = null;
    }

    private synchronized void scheduleOpen(MidiDeviceInfo info, long callbackGeneration) {
        if (!started || generation != callbackGeneration || midiManager == null
                || !hasOutputPort(info) || connections.containsKey(info.getId())
                || !pendingDeviceIds.add(info.getId())) {
            return;
        }
        try {
            midiManager.openDevice(info,
                    device -> completeOpen(info, device, callbackGeneration),
                    mainHandler);
        } catch (RuntimeException exception) {
            pendingDeviceIds.remove(info.getId());
        }
    }

    private synchronized void completeOpen(MidiDeviceInfo info, MidiDevice device,
                                           long callbackGeneration) {
        boolean expected = pendingDeviceIds.remove(info.getId());
        if (!expected || !started || generation != callbackGeneration || device == null) {
            closeDevice(device);
            return;
        }

        DeviceConnection connection = new DeviceConnection(device);
        String deviceStableId = stableDeviceId(info);
        String baseDisplayName = displayName(info);
        for (MidiDeviceInfo.PortInfo portInfo : info.getPorts()) {
            if (portInfo.getType() != MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                continue;
            }
            MidiOutputPort outputPort = device.openOutputPort(portInfo.getPortNumber());
            if (outputPort == null) {
                continue;
            }
            String stableId = deviceStableId + ":out:" + portInfo.getPortNumber();
            String portName = portInfo.getName();
            String portDisplayName = portName == null || portName.trim().isEmpty()
                    ? baseDisplayName
                    : baseDisplayName + " — " + portName.trim();
            PortReceiver receiver = new PortReceiver(stableId);
            try {
                outputPort.connect(receiver);
                connection.ports.add(new PortConnection(
                        outputPort, receiver, stableId, portDisplayName));
                nativeMidiDevice(stableId, portDisplayName, true);
            } catch (RuntimeException exception) {
                receiver.closeReceiver();
                closePort(outputPort);
            }
        }
        if (connection.ports.isEmpty()) {
            connection.close(false);
            return;
        }
        connections.put(info.getId(), connection);
    }

    private synchronized void removeDevice(int deviceId, boolean notifyNative) {
        pendingDeviceIds.remove(deviceId);
        DeviceConnection connection = connections.remove(deviceId);
        if (connection != null) {
            connection.close(notifyNative);
        }
    }

    private boolean hasOutputPort(MidiDeviceInfo info) {
        for (MidiDeviceInfo.PortInfo portInfo : info.getPorts()) {
            if (portInfo.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                return true;
            }
        }
        return false;
    }

    private String stableDeviceId(MidiDeviceInfo info) {
        String fingerprint = deviceFingerprint(info);
        int matchingDevices = 0;
        if (midiManager != null) {
            for (MidiDeviceInfo candidate : midiManager.getDevices()) {
                if (deviceFingerprint(candidate).equals(fingerprint)) {
                    matchingDevices++;
                }
            }
        }
        String result = "midi:android:" + sha256Prefix(fingerprint);
        if (matchingDevices > 1) {
            // Truly indistinguishable devices have no persistent discriminator.
            // The framework ID keeps simultaneous copies distinct for this session.
            result += ":device:" + info.getId();
        }
        return result;
    }

    private String deviceFingerprint(MidiDeviceInfo info) {
        Bundle properties = info.getProperties();
        StringBuilder value = new StringBuilder();
        value.append(info.getType());
        appendProperty(value, properties, MidiDeviceInfo.PROPERTY_MANUFACTURER);
        appendProperty(value, properties, MidiDeviceInfo.PROPERTY_PRODUCT);
        appendProperty(value, properties, MidiDeviceInfo.PROPERTY_VERSION);
        appendProperty(value, properties, MidiDeviceInfo.PROPERTY_SERIAL_NUMBER);
        appendProperty(value, properties, MidiDeviceInfo.PROPERTY_NAME);
        for (MidiDeviceInfo.PortInfo portInfo : info.getPorts()) {
            if (portInfo.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                value.append('|').append(portInfo.getPortNumber()).append(':');
                if (portInfo.getName() != null) {
                    value.append(portInfo.getName().trim());
                }
            }
        }
        return value.toString();
    }

    private void appendProperty(StringBuilder value, Bundle properties, String key) {
        Object property = properties.get(key);
        value.append('|');
        if (property != null) {
            value.append(property.toString().trim());
        }
    }

    private String displayName(MidiDeviceInfo info) {
        Bundle properties = info.getProperties();
        String product = propertyText(properties, MidiDeviceInfo.PROPERTY_PRODUCT);
        if (!product.isEmpty()) {
            return product;
        }
        String name = propertyText(properties, MidiDeviceInfo.PROPERTY_NAME);
        if (!name.isEmpty()) {
            return name;
        }
        String manufacturer = propertyText(properties, MidiDeviceInfo.PROPERTY_MANUFACTURER);
        if (!manufacturer.isEmpty()) {
            return manufacturer + " MIDI";
        }
        return String.format(Locale.ROOT, "Android MIDI %d", info.getId());
    }

    private String propertyText(Bundle properties, String key) {
        Object value = properties.get(key);
        return value == null ? "" : value.toString().trim();
    }

    private String sha256Prefix(String value) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256")
                    .digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder result = new StringBuilder(32);
            for (int index = 0; index < 16; index++) {
                result.append(String.format(Locale.ROOT, "%02x", digest[index] & 0xff));
            }
            return result.toString();
        } catch (NoSuchAlgorithmException impossible) {
            return Integer.toHexString(value.hashCode());
        }
    }

    private static void closePort(MidiOutputPort port) {
        if (port == null) {
            return;
        }
        try {
            port.close();
        } catch (IOException ignored) {
            // Device removal commonly races port closure.
        }
    }

    private static void closeDevice(MidiDevice device) {
        if (device == null) {
            return;
        }
        try {
            device.close();
        } catch (IOException ignored) {
            // Device removal commonly races device closure.
        }
    }

    private final class DeviceConnection {
        final MidiDevice device;
        final List<PortConnection> ports = new ArrayList<>();

        DeviceConnection(MidiDevice device) {
            this.device = device;
        }

        void close(boolean notifyNative) {
            for (PortConnection port : ports) {
                port.receiver.closeReceiver();
                closePort(port.outputPort);
                if (notifyNative) {
                    nativeMidiDevice(port.stableId, port.displayName, false);
                }
            }
            ports.clear();
            closeDevice(device);
        }
    }

    private static final class PortConnection {
        final MidiOutputPort outputPort;
        final PortReceiver receiver;
        final String stableId;
        final String displayName;

        PortConnection(MidiOutputPort outputPort, PortReceiver receiver,
                       String stableId, String displayName) {
            this.outputPort = outputPort;
            this.receiver = receiver;
            this.stableId = stableId;
            this.displayName = displayName;
        }
    }

    private static final class PortReceiver extends MidiReceiver {
        private final String stableId;
        private volatile boolean open = true;

        PortReceiver(String stableId) {
            this.stableId = stableId;
        }

        void closeReceiver() {
            open = false;
        }

        @Override
        public void onSend(byte[] message, int offset, int count, long timestamp)
                throws IOException {
            if (open && count > 0) {
                nativeMidiPacket(stableId, message, offset, count, timestamp);
            }
        }
    }

    private static native void nativeMidiDevice(
            String stableId, String displayName, boolean connected);

    private static native void nativeMidiPacket(
            String stableId, byte[] data, int offset, int count, long timestampNanos);
}
