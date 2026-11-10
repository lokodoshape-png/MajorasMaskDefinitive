package com.dishii.mm;

import android.content.Context;
import android.content.res.AssetManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

// Copies out of the APK's assets and between folders. Every mkdirs(), listFiles() and stream that fails throws,
// so a copy never ends "successfully" with files missing. The port's own files are installed by PortInstall,
// which also checks them; these are for other copies.
public class AssetCopyUtil {

    public static void copyAssetsToExternal(Context context, String assetsFolderPath, String externalFolderPath) throws IOException {
        AssetManager assetManager = context.getAssets();
        String[] assetFiles = assetManager.list(assetsFolderPath);
        if (assetFiles == null) {
            throw new IOException("não foi possível listar " + assetsFolderPath + " no APK");
        }
        FileOps.mkdirs(new File(externalFolderPath));

        for (String assetFile : assetFiles) {
            String assetPath = assetsFolderPath + File.separator + assetFile;
            String externalPath = externalFolderPath + File.separator + assetFile;
            String[] children = assetManager.list(assetPath);

            if (children != null && children.length > 0) {
                // A directory
                copyAssetsToExternal(context, assetPath, externalPath);
            } else {
                File externalFile = new File(externalPath);
                if (!externalFile.exists()) {
                    try (InputStream in = assetManager.open(assetPath)) {
                        FileOps.copy(in, externalFile, Long.MAX_VALUE);
                    }
                }
            }
        }
    }

    public static void copyDirectory(File sourceDir, File targetDir) throws IOException {
        FileOps.mkdirs(targetDir);
        for (File file : FileOps.list(sourceDir)) {
            File dest = new File(targetDir, file.getName());
            if (file.isDirectory()) {
                copyDirectory(file, dest);
            } else {
                copyFile(file, dest);
            }
        }
    }

    public static void copyFile(File source, File dest) throws IOException {
        long size;
        try (InputStream in = new FileInputStream(source)) {
            size = FileOps.copy(in, dest, Long.MAX_VALUE).size;
        }
        if (size != source.length()) {
            throw new IOException("cópia incompleta de " + source.getPath());
        }
    }
}
