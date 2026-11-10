package com.dishii.mm;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

// Limits and checks for the .zip files the app unpacks (mods picked by the user): how many entries, how much they
// unpack to, how much one entry may grow, and names that would leave the target folder. A .o2r is itself a zip;
// it is opened once to see that it is not damaged before it replaces anything.
//
// Plain Java, tested in src/test. The same limits are in mm/2s2h/Mods/ModDownloader.cpp for Drive downloads.
final class SafeZip {

    // A zip that only wraps mods holds a few .o2r and a readme.
    static final int MAX_ENTRIES = 10000;
    static final int MAX_ARCHIVES = 64;
    // HD texture packs can be large, but not this large.
    static final long MAX_TOTAL_BYTES = 8L << 30;
    static final long MAX_ENTRY_BYTES = 4L << 30;
    // An entry may unpack to at most this many times its packed size (normal data stays far below; a zip bomb
    // does not). Entries under 1 MiB are not held to it.
    static final long MAX_RATIO = 200;
    static final long RATIO_FREE_BYTES = 1L << 20;
    // Kept free on the storage after an install.
    static final long FREE_SPACE_MARGIN = 64L << 20;

    private SafeZip() {
    }

    static boolean isGameArchive(String name) {
        String ext = FileOps.extension(name);
        return ext.equals(".o2r") || ext.equals(".otr");
    }

    // Files that may ride along in a zip of mods without making it a mod archive itself.
    static boolean isSideFile(String name) {
        String ext = FileOps.extension(name);
        return ext.equals(".txt") || ext.equals(".md") || ext.equals(".pdf") || ext.equals(".png")
                || ext.equals(".jpg");
    }

    // Rejects absolute names, "..", backslashes and control characters, which a zip entry may carry to write
    // outside the target folder.
    static boolean isSafeEntryName(String name) {
        if (name == null || name.isEmpty() || name.startsWith("/") || name.contains("\\")) {
            return false;
        }
        if (name.length() > 1 && name.charAt(1) == ':') {
            return false;
        }
        for (String part : name.split("/")) {
            if (part.equals("..")) {
                return false;
            }
        }
        for (char c : name.toCharArray()) {
            if (c < 32) {
                return false;
            }
        }
        return true;
    }

    static String baseName(String entryName) {
        String trimmed = entryName.endsWith("/") ? entryName.substring(0, entryName.length() - 1) : entryName;
        int slash = trimmed.lastIndexOf('/');
        return slash < 0 ? trimmed : trimmed.substring(slash + 1);
    }

    // What the zip's directory says.
    static final class Scan {
        int entries;
        final List<ZipEntry> archives = new ArrayList<>(); // .o2r / .otr entries
        long archiveBytes; // what they say they unpack to
        boolean hasOther; // anything that is neither a game archive nor a side file

        // Only .o2r / .otr (and side files): its mods are taken out. Anything else is a mod archive itself.
        boolean wrapsArchives() {
            return !archives.isEmpty() && !hasOther;
        }
    }

    // Throws on a damaged zip.
    static Scan scan(ZipFile zip) throws IOException {
        Scan scan = new Scan();
        Enumeration<? extends ZipEntry> entries = zip.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            scan.entries++;
            if (entry.isDirectory()) {
                continue;
            }
            String base = baseName(entry.getName());
            if (isGameArchive(base)) {
                scan.archives.add(entry);
                scan.archiveBytes += entry.getSize() < 0 ? MAX_ENTRY_BYTES : entry.getSize();
            } else if (!isSideFile(base)) {
                scan.hasOther = true;
            }
        }
        if (scan.entries == 0) {
            throw new IOException("o zip está vazio");
        }
        return scan;
    }

    // Writes each .o2r / .otr of a zip that only wraps mods into `folder`, holding it to the limits. Returns the
    // files written; on an exception some may already be in `folder` (a temporary one), never anywhere else.
    static List<File> extractArchives(ZipFile zip, Scan scan, File folder) throws IOException {
        if (scan.entries > MAX_ENTRIES) {
            throw new IOException("o zip tem arquivos demais (mais de " + MAX_ENTRIES + ")");
        }
        if (scan.archives.size() > MAX_ARCHIVES) {
            throw new IOException("o zip tem mods demais (mais de " + MAX_ARCHIVES + ")");
        }
        List<File> written = new ArrayList<>();
        long total = 0;
        for (ZipEntry entry : scan.archives) {
            if (!isSafeEntryName(entry.getName())) {
                throw new IOException("o zip tem um caminho inválido: " + entry.getName());
            }
            String name = FileOps.safeFileName(baseName(entry.getName()), "mod" + written.size() + ".o2r");
            File out = new File(folder, name);
            if (!FileOps.isInside(folder, out)) {
                throw new IOException("caminho inválido: " + entry.getName());
            }
            for (File earlier : written) {
                if (earlier.getName().equalsIgnoreCase(name)) {
                    throw new IOException("o zip tem dois mods com o nome " + name);
                }
            }
            long limit = Math.min(MAX_ENTRY_BYTES, MAX_TOTAL_BYTES - total);
            long packed = entry.getCompressedSize();
            if (entry.getMethod() == ZipEntry.DEFLATED && packed > 0) {
                limit = Math.min(limit, Math.max(RATIO_FREE_BYTES, packed * MAX_RATIO));
            }
            FileOps.Copied copied;
            try (InputStream in = zip.getInputStream(entry)) {
                copied = FileOps.copy(in, out, limit);
            } catch (IOException e) {
                throw new IOException("não foi possível tirar " + name + " do zip: " + e.getMessage(), e);
            }
            total += copied.size;
            written.add(out);
        }
        return written;
    }

    // Null when the file looks like a working game archive: a .o2r or mod .zip must open as a zip with at least one
    // entry, a .otr must start with the MPQ signature.
    static String archiveProblem(File file) {
        if (!file.isFile() || file.length() == 0) {
            return "arquivo vazio";
        }
        if (FileOps.extension(file.getName()).equals(".otr")) {
            try (InputStream in = new FileInputStream(file)) {
                byte[] magic = new byte[4];
                if (in.read(magic) != 4 || magic[0] != 'M' || magic[1] != 'P' || magic[2] != 'Q') {
                    return "não é um arquivo .otr válido";
                }
            } catch (IOException e) {
                return "não foi possível ler o arquivo";
            }
            return null;
        }
        try (ZipFile zip = new ZipFile(file)) {
            if (zip.size() == 0) {
                return "arquivo vazio";
            }
        } catch (IOException e) {
            return "arquivo danificado (" + e.getMessage() + ")";
        }
        return null;
    }

    // `folder` must exist (an absent path reports no space).
    static boolean hasFreeSpace(File folder, long bytes) {
        return folder.getUsableSpace() >= bytes + FREE_SPACE_MARGIN;
    }
}
