package com.dishii.mm;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

// File helpers that report failure instead of ignoring it: java.io.File's mkdirs(), listFiles(), renameTo() and
// delete() only return false or null, which the old setup code never looked at. Plain Java (no Android classes), so
// the unit tests in src/test run on any JVM.
final class FileOps {

    private FileOps() {
    }

    static void mkdirs(File dir) throws IOException {
        if (dir.isDirectory()) {
            return;
        }
        if (!dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("não foi possível criar a pasta " + dir.getPath());
        }
    }

    static File[] list(File dir) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) {
            throw new IOException("não foi possível ler a pasta " + dir.getPath());
        }
        return children;
    }

    // True when nothing is left at `file`.
    static boolean deleteRecursive(File file) {
        boolean ok = true;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    ok &= deleteRecursive(child);
                }
            }
        }
        if (file.exists() && !file.delete()) {
            ok = false;
        }
        return ok && !file.exists();
    }

    // Same-volume move. On Android and Linux it replaces a file at `to` in one step.
    static void move(File from, File to) throws IOException {
        if (!from.renameTo(to)) {
            throw new IOException("não foi possível mover " + from.getPath() + " para " + to.getPath());
        }
    }

    static MessageDigest sha256Digest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e); // every Java and Android runtime has SHA-256
        }
    }

    static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            s.append(Character.forDigit((b >> 4) & 0xF, 16)).append(Character.forDigit(b & 0xF, 16));
        }
        return s.toString();
    }

    static final class Copied {
        final long size;
        final String sha256;

        Copied(long size, String sha256) {
            this.size = size;
            this.sha256 = sha256;
        }
    }

    // Writes `in` to `dest` (flushed to the disk), refusing more than `maxBytes`. Returns the size and SHA-256 of
    // what was read, to compare with the file once it is on the disk.
    static Copied copy(InputStream in, File dest, long maxBytes) throws IOException {
        MessageDigest digest = sha256Digest();
        long total = 0;
        try (FileOutputStream out = new FileOutputStream(dest)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) != -1) {
                total += read;
                if (total > maxBytes) {
                    throw new IOException("arquivo maior que o limite (" + maxBytes + " bytes): " + dest.getName());
                }
                digest.update(buffer, 0, read);
                out.write(buffer, 0, read);
            }
            out.flush();
            out.getFD().sync();
        }
        return new Copied(total, hex(digest.digest()));
    }

    static String sha256(File file) throws IOException {
        MessageDigest digest = sha256Digest();
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) != -1) {
                digest.update(buffer, 0, read);
            }
        }
        return hex(digest.digest());
    }

    static byte[] readAll(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        }
    }

    // Replaces `target` with `data` without a moment where it is half written.
    static void writeAtomically(File target, byte[] data) throws IOException {
        File parent = target.getAbsoluteFile().getParentFile();
        if (parent != null) {
            mkdirs(parent);
        }
        File tmp = new File(parent, target.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
            out.flush();
            out.getFD().sync();
        }
        move(tmp, target);
    }

    // Lowercase extension with the dot, or "".
    static String extension(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? "" : name.substring(dot).toLowerCase(java.util.Locale.ROOT);
    }

    // A file name that cannot leave its folder: no separators, no "..", no control characters.
    static String safeFileName(String name, String fallback) {
        if (name == null) {
            return fallback;
        }
        StringBuilder s = new StringBuilder();
        for (char c : name.toCharArray()) {
            s.append(c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
                    || c == '>' || c == '|' ? '_' : c);
        }
        String out = s.toString().trim();
        while (out.endsWith(".")) {
            out = out.substring(0, out.length() - 1);
        }
        while (out.startsWith(".")) {
            out = out.substring(1);
        }
        return out.isEmpty() ? fallback : out;
    }

    // True when `child` is inside `folder` once both are resolved (symbolic links, "..").
    static boolean isInside(File folder, File child) throws IOException {
        String base = folder.getCanonicalPath() + File.separator;
        return child.getCanonicalPath().startsWith(base);
    }
}
