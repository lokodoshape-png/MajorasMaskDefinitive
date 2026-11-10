package com.dishii.mm;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// The port's own files in the data folder (assets/ for the ROM extractor and 2ship.o2r), copied out of the APK.
//
// They are installed as one unit and never half replaced:
//   1. the APK's copy goes into <data>/.instalacao-nova, and every file is read back and compared (size, SHA-256)
//      with what was read from the APK;
//   2. a manifest (port-files.manifest: the APK build it came from, then size, SHA-256 and path of every file) is
//      written next to them;
//   3. the working copy is moved aside to <data>/.instalacao-anterior (manifest first, so it stops counting as
//      valid), the new one is moved in (manifest last, so it only counts as valid once complete);
//   4. only then is the old copy deleted. Any failure moves the old copy back.
// If the app is killed in the middle, recover() at the next start puts things back in the state of step 1 or 4.
//
// Plain Java (no Android classes): MainActivity passes the APK's assets as a Source. Tested in src/test.
final class PortInstall {

    interface Source {
        // Names inside a folder of the APK's assets ("" is the top); empty for a file.
        String[] list(String path) throws IOException;

        InputStream open(String path) throws IOException;
    }

    interface Log {
        void line(String text);
    }

    static final String ASSETS = "assets";
    static final String PORT_O2R = "2ship.o2r";
    static final String MANIFEST = "port-files.manifest";
    static final String STAGING = ".instalacao-nova";
    static final String PREVIOUS = ".instalacao-anterior";

    // Moved out of the live folder in this order, into it in the reverse one.
    private static final String[] ITEMS = { MANIFEST, PORT_O2R, ASSETS };

    // No single file the APK carries comes near this; it only stops a broken source from filling the storage.
    static final long MAX_FILE_BYTES = 1L << 30;

    // What the extractor and the game cannot start without (the old sentinels, still checked on top of the
    // manifest).
    private static final String[] REQUIRED = { ASSETS + "/Config_N64_US.xml", PORT_O2R };
    private static final String[] REQUIRED_DIRS = { ASSETS + "/xml", ASSETS + "/filelists" };

    static final class Entry {
        final long size;
        final String sha256;

        Entry(long size, String sha256) {
            this.size = size;
            this.sha256 = sha256;
        }
    }

    static final class Manifest {
        String stamp = "";
        final Map<String, Entry> files = new LinkedHashMap<>();

        byte[] toBytes() {
            StringBuilder s = new StringBuilder();
            s.append("stamp ").append(stamp).append('\n');
            for (Map.Entry<String, Entry> e : files.entrySet()) {
                s.append(e.getValue().size).append(' ').append(e.getValue().sha256).append(' ').append(e.getKey())
                        .append('\n');
            }
            return s.toString().getBytes(StandardCharsets.UTF_8);
        }

        static Manifest parse(byte[] data) throws IOException {
            Manifest m = new Manifest();
            String[] lines = new String(data, StandardCharsets.UTF_8).split("\n");
            if (lines.length == 0 || !lines[0].startsWith("stamp ")) {
                throw new IOException("manifesto inválido");
            }
            m.stamp = lines[0].substring(6).trim();
            for (int i = 1; i < lines.length; i++) {
                String line = lines[i];
                if (line.isEmpty()) {
                    continue;
                }
                int a = line.indexOf(' ');
                int b = a < 0 ? -1 : line.indexOf(' ', a + 1);
                if (a <= 0 || b <= a) {
                    throw new IOException("manifesto inválido na linha " + (i + 1));
                }
                String path = line.substring(b + 1);
                if (path.startsWith("/") || path.contains("..") || path.contains("\\")) {
                    throw new IOException("caminho inválido no manifesto: " + path);
                }
                try {
                    m.files.put(path, new Entry(Long.parseLong(line.substring(0, a)), line.substring(a + 1, b)));
                } catch (NumberFormatException e) {
                    throw new IOException("manifesto inválido na linha " + (i + 1));
                }
            }
            return m;
        }
    }

    private final File root;
    private final Log log;

    PortInstall(File root, Log log) {
        this.root = root;
        this.log = log;
    }

    // ----- Checking

    static Manifest readManifest(File folder) {
        File file = new File(folder, MANIFEST);
        if (!file.isFile()) {
            return null;
        }
        try {
            return Manifest.parse(FileOps.readAll(file));
        } catch (IOException e) {
            return null;
        }
    }

    // Null when the copy in `folder` is complete: every file of its manifest there with its size (and, with
    // fullHash, its SHA-256), plus the files the game needs. Otherwise the first problem found.
    static String problem(File folder, boolean fullHash) {
        Manifest m = readManifest(folder);
        if (m == null) {
            return "sem manifesto";
        }
        if (m.files.isEmpty()) {
            return "manifesto vazio";
        }
        for (String required : REQUIRED) {
            if (!m.files.containsKey(required)) {
                return "manifesto sem " + required;
            }
        }
        for (String dir : REQUIRED_DIRS) {
            if (!new File(folder, dir).isDirectory()) {
                return "faltando " + dir;
            }
        }
        for (Map.Entry<String, Entry> e : m.files.entrySet()) {
            File file = new File(folder, e.getKey());
            if (!file.isFile()) {
                return "faltando " + e.getKey();
            }
            if (file.length() != e.getValue().size) {
                return "tamanho errado: " + e.getKey();
            }
            if (fullHash) {
                try {
                    if (!FileOps.sha256(file).equals(e.getValue().sha256)) {
                        return "conteúdo diferente: " + e.getKey();
                    }
                } catch (IOException ex) {
                    return "não foi possível ler " + e.getKey();
                }
            }
        }
        if (new File(folder, PORT_O2R).length() == 0) {
            return PORT_O2R + " vazio";
        }
        return null;
    }

    // The build the working copy came from, or null when it is not complete.
    String installedStamp() {
        if (problem(root, false) != null) {
            return null;
        }
        Manifest m = readManifest(root);
        return m == null ? null : m.stamp;
    }

    // ----- After an interrupted install

    // Puts things back after the app was closed during an install: a copy moved aside comes back if the working one
    // is not complete, a half-made new copy is dropped.
    void recover() {
        File previous = new File(root, PREVIOUS);
        if (previous.isDirectory()) {
            String liveProblem = problem(root, false);
            if (liveProblem == null) {
                log.line("Instalação: a troca anterior terminou; apagando a cópia antiga");
                FileOps.deleteRecursive(previous);
            } else {
                log.line("Instalação interrompida (" + liveProblem + "): voltando a cópia anterior");
                try {
                    restoreFrom(previous);
                    FileOps.deleteRecursive(previous);
                } catch (IOException e) {
                    log.line("Não foi possível voltar a cópia anterior: " + e.getMessage());
                }
            }
        }
        File staging = new File(root, STAGING);
        if (staging.exists()) {
            log.line("Instalação: apagando uma cópia nova incompleta");
            FileOps.deleteRecursive(staging);
        }
    }

    // Each item kept in `from` replaces the one in the working folder.
    private void restoreFrom(File from) throws IOException {
        for (int i = ITEMS.length - 1; i >= 0; i--) {
            File kept = new File(from, ITEMS[i]);
            if (!kept.exists()) {
                continue;
            }
            File live = new File(root, ITEMS[i]);
            if (live.exists() && !FileOps.deleteRecursive(live)) {
                throw new IOException("não foi possível apagar " + live.getPath());
            }
            FileOps.move(kept, live);
        }
    }

    // ----- Installing

    // Copies, checks and swaps in the APK's port files. On failure the working copy is as it was before, and the
    // exception says why.
    void install(Source source, String stamp) throws IOException {
        FileOps.mkdirs(root);
        recover();
        File staging = new File(root, STAGING);
        if (staging.exists() && !FileOps.deleteRecursive(staging)) {
            throw new IOException("não foi possível limpar " + staging.getPath());
        }
        FileOps.mkdirs(staging);

        Manifest manifest = new Manifest();
        manifest.stamp = stamp;
        try {
            copyTree(source, ASSETS, new File(staging, ASSETS), manifest);
            try (InputStream in = source.open(PORT_O2R)) {
                FileOps.Copied c = FileOps.copy(in, new File(staging, PORT_O2R), MAX_FILE_BYTES);
                manifest.files.put(PORT_O2R, new Entry(c.size, c.sha256));
            }
            FileOps.writeAtomically(new File(staging, MANIFEST), manifest.toBytes());
            String problem = problem(staging, true);
            if (problem != null) {
                throw new IOException("a cópia nova não confere (" + problem + ")");
            }
            log.line("Instalação: " + manifest.files.size() + " arquivos copiados e conferidos");
            swapIn(staging);
        } catch (IOException e) {
            FileOps.deleteRecursive(staging);
            throw e;
        }
        FileOps.deleteRecursive(staging);
    }

    private void copyTree(Source source, String path, File dest, Manifest manifest) throws IOException {
        String[] names = source.list(path);
        if (names == null || names.length == 0) {
            throw new IOException("o APK não tem " + path);
        }
        FileOps.mkdirs(dest);
        for (String name : names) {
            String child = path + "/" + name;
            String[] grandChildren = source.list(child);
            if (grandChildren != null && grandChildren.length > 0) {
                copyTree(source, child, new File(dest, name), manifest);
            } else {
                try (InputStream in = source.open(child)) {
                    FileOps.Copied c = FileOps.copy(in, new File(dest, name), MAX_FILE_BYTES);
                    manifest.files.put(child, new Entry(c.size, c.sha256));
                }
            }
        }
    }

    private void swapIn(File staging) throws IOException {
        File previous = new File(root, PREVIOUS);
        if (previous.exists() && !FileOps.deleteRecursive(previous)) {
            throw new IOException("não foi possível limpar " + previous.getPath());
        }
        FileOps.mkdirs(previous);
        List<String> movedIn = new ArrayList<>();
        try {
            for (String item : ITEMS) {
                File live = new File(root, item);
                if (live.exists()) {
                    FileOps.move(live, new File(previous, item));
                }
            }
            for (int i = ITEMS.length - 1; i >= 0; i--) {
                FileOps.move(new File(staging, ITEMS[i]), new File(root, ITEMS[i]));
                movedIn.add(ITEMS[i]);
            }
            String problem = problem(root, false);
            if (problem != null) {
                throw new IOException("a instalação nova não confere (" + problem + ")");
            }
        } catch (IOException e) {
            log.line("Instalação: falhou (" + e.getMessage() + "), voltando a cópia anterior");
            for (String item : movedIn) {
                FileOps.deleteRecursive(new File(root, item));
            }
            restoreFrom(previous);
            FileOps.deleteRecursive(previous);
            throw e;
        }
        if (!FileOps.deleteRecursive(previous)) {
            log.line("Instalação: não foi possível apagar toda a cópia antiga (fica para a próxima vez)");
        }
    }
}
