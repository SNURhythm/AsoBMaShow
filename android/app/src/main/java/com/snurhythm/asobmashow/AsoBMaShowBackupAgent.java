package com.snurhythm.asobmashow;

import android.app.backup.BackupAgent;
import android.app.backup.BackupDataInput;
import android.app.backup.BackupDataOutput;
import android.app.backup.FullBackupDataOutput;
import android.content.Context;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class AsoBMaShowBackupAgent extends BackupAgent {
    @Override
    public void onBackup(
            ParcelFileDescriptor oldState,
            BackupDataOutput data,
            ParcelFileDescriptor newState) {
        // Auto Backup uses onFullBackup; no key/value backup is maintained.
    }

    @Override
    public void onRestore(
            BackupDataInput data,
            int appVersionCode,
            ParcelFileDescriptor newState) {
        // Auto Backup restores through onRestoreFile.
    }

    @Override
    public void onFullBackup(FullBackupDataOutput data) throws IOException {
        Context credentialContext = this;
        Context deviceContext = createDeviceProtectedStorageContext();
        File externalRoot = getExternalFilesDir(null);

        List<Path> credentialProfileRoots = profileRoots(
                credentialContext, deviceContext, externalRoot);
        Set<Path> visitedRoots = new HashSet<>();
        backupContext(credentialContext, credentialProfileRoots, visitedRoots, data);
        backupContext(deviceContext, credentialProfileRoots, visitedRoots, data);
        if (externalRoot != null) {
            backupTree(
                    externalRoot.toPath(),
                    new ArrayList<Path>(),
                    credentialProfileRoots,
                    visitedRoots,
                    data);
        }
    }

    @Override
    public void onRestoreFile(
            ParcelFileDescriptor data,
            long size,
            File destination,
            int type,
            long mode,
            long modificationTime) throws IOException {
        Context credentialContext = this;
        Context deviceContext = createDeviceProtectedStorageContext();
        if (AsoBMaShowBackupPathPolicy.isCredentialArtifact(
                destination.toPath(),
                profileRoots(
                        credentialContext,
                        deviceContext,
                        getExternalFilesDir(null)))) {
            consume(data, size);
            return;
        }
        super.onRestoreFile(data, size, destination, type, mode, modificationTime);
    }

    private void backupContext(
            Context context,
            Collection<Path> credentialProfileRoots,
            Set<Path> visitedRoots,
            FullBackupDataOutput data) throws IOException {
        List<Path> excludedRoots = new ArrayList<>();
        excludedRoots.add(context.getCacheDir().toPath());
        excludedRoots.add(context.getCodeCacheDir().toPath());
        excludedRoots.add(context.getNoBackupFilesDir().toPath());
        if (context.getApplicationInfo().nativeLibraryDir != null) {
            excludedRoots.add(new File(
                    context.getApplicationInfo().nativeLibraryDir).toPath());
        }
        backupTree(
                context.getDataDir().toPath(),
                excludedRoots,
                credentialProfileRoots,
                visitedRoots,
                data);
    }

    private void backupTree(
            Path root,
            Collection<Path> excludedRoots,
            Collection<Path> credentialProfileRoots,
            Set<Path> visitedRoots,
            FullBackupDataOutput data) throws IOException {
        Path normalizedRoot = root.toAbsolutePath().normalize();
        if (!visitedRoots.add(normalizedRoot)) {
            return;
        }
        AsoBMaShowBackupPathPolicy.visitBackupEntries(
                normalizedRoot,
                excludedRoots,
                credentialProfileRoots,
                path -> fullBackupFile(path.toFile(), data));
    }

    private static List<Path> profileRoots(
            Context credentialContext,
            Context deviceContext,
            File externalRoot) {
        List<Path> roots = new ArrayList<>();
        roots.add(credentialContext.getFilesDir().toPath().resolve("profiles"));
        roots.add(deviceContext.getFilesDir().toPath().resolve("profiles"));
        if (externalRoot != null) {
            roots.add(externalRoot.toPath().resolve("profiles"));
        }
        return roots;
    }

    @SuppressWarnings("resource")
    private static void consume(ParcelFileDescriptor data, long size)
            throws IOException {
        FileInputStream input = new FileInputStream(data.getFileDescriptor());
        byte[] buffer = new byte[8192];
        long remaining = size;
        while (remaining > 0) {
            int read = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
            if (read < 0) {
                throw new IOException("credential restore payload ended unexpectedly");
            }
            remaining -= read;
        }
    }
}
