package com.dishii.mm;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.Set;

// Brings the files of an older install into the data folder: saves, mods, translations, music, settings and
// anything else, each top-level item (category) on its own, so one that fails does not stop the others.
//
// Nothing in the data folder is ever overwritten. When both sides have a file with different contents, the data
// folder's stays where it is and the older install's goes to <data>/migracao/conflitos/<source>/<path>, so nothing
// is lost and the person can pick. Files already there with the same contents count as done, which makes a
// migration that was interrupted safe to run again: it carries on where it stopped. Every step is logged.
//
// Plain Java, tested in src/test.
final class DataMigration {

    interface Log {
        void line(String text);
    }

    static final String FOLDER = "migracao";
    static final String CONFLICTS = "conflitos";

    int moved, copied, same, conflicts, failed;

    private final File dest;
    private final Log log;

    DataMigration(File dest, Log log) {
        this.dest = dest;
        this.log = log;
    }

    // Moves (move true) or copies the contents of `source` into the data folder. Items named in `skip` are left
    // alone. Returns true when every item made it (moved, copied, already there, or kept as a conflict).
    boolean migrate(File source, String label, boolean move, Set<String> skip) {
        File[] children = source.listFiles();
        if (children == null) {
            log.line("Migração " + label + ": nada para migrar em " + source.getPath());
            return true;
        }
        if (skip == null) {
            skip = Collections.emptySet();
        }
        try {
            FileOps.mkdirs(dest);
        } catch (IOException e) {
            log.line("Migração " + label + ": " + e.getMessage());
            failed++;
            return false;
        }
        int failedBefore = failed;
        log.line("Migração " + label + ": de " + source.getPath() + " (" + (move ? "movendo" : "copiando") + ")");
        for (File child : children) {
            String name = child.getName();
            if (skip.contains(name)) {
                log.line("  " + name + ": ignorado (o app recria)");
                continue;
            }
            try {
                merge(child, new File(dest, name), label, name, move);
            } catch (IOException e) {
                failed++;
                log.line("  " + name + ": ERRO " + e.getMessage());
            }
        }
        if (move) {
            source.delete(); // only goes when empty
        }
        log.line("Migração " + label + ": " + moved + " movidos, " + copied + " copiados, " + same
                + " já estavam lá, " + conflicts + " conflitos, " + failed + " falhas");
        return failed == failedBefore;
    }

    private void merge(File src, File dst, String label, String rel, boolean move) throws IOException {
        if (src.isDirectory()) {
            if (move && !dst.exists() && src.renameTo(dst)) {
                moved++;
                log.line("  " + rel + "/: movido inteiro");
                return;
            }
            if (dst.exists() && !dst.isDirectory()) {
                keepAsConflict(src, label, rel, move);
                return;
            }
            FileOps.mkdirs(dst);
            for (File child : FileOps.list(src)) {
                try {
                    merge(child, new File(dst, child.getName()), label, rel + "/" + child.getName(), move);
                } catch (IOException e) {
                    failed++;
                    log.line("  " + rel + "/" + child.getName() + ": ERRO " + e.getMessage());
                }
            }
            if (move) {
                src.delete(); // only when empty
            }
            return;
        }
        if (!src.isFile()) {
            return;
        }
        if (!dst.exists()) {
            if (move && src.renameTo(dst)) {
                moved++;
                return;
            }
            copyChecked(src, dst);
            if (move) {
                if (!src.delete()) {
                    log.line("  " + rel + ": copiado, mas o original não pôde ser apagado");
                }
                moved++;
            } else {
                copied++;
            }
            return;
        }
        if (dst.isFile() && sameContents(src, dst)) {
            same++;
            if (move) {
                src.delete();
            }
            return;
        }
        keepAsConflict(src, label, rel, move);
    }

    // The data folder's file stays; the other one is kept beside the migration log.
    private void keepAsConflict(File src, String label, String rel, boolean move) throws IOException {
        File conflict = new File(new File(new File(dest, FOLDER), CONFLICTS), label + "/" + rel);
        String newer = src.lastModified() > new File(dest, rel).lastModified() ? "o da instalação antiga"
                : "o da pasta atual";
        if (conflict.exists()) {
            if (conflict.isFile() && src.isFile() && sameContents(src, conflict)) {
                if (move) {
                    src.delete();
                }
                conflicts++;
                return;
            }
            conflict = new File(conflict.getPath() + "." + System.currentTimeMillis());
        }
        FileOps.mkdirs(conflict.getParentFile());
        if (src.isDirectory()) {
            if (!(move && src.renameTo(conflict))) {
                copyTree(src, conflict);
            }
        } else if (!(move && src.renameTo(conflict))) {
            copyChecked(src, conflict);
            if (move) {
                src.delete();
            }
        }
        conflicts++;
        log.line("  " + rel + ": já existe com outro conteúdo; mantido o da pasta atual, o antigo foi para "
                + conflict.getPath() + " (mais novo: " + newer + ")");
    }

    private static void copyTree(File src, File dst) throws IOException {
        FileOps.mkdirs(dst);
        for (File child : FileOps.list(src)) {
            File target = new File(dst, child.getName());
            if (child.isDirectory()) {
                copyTree(child, target);
            } else {
                copyChecked(child, target);
            }
        }
    }

    // Through a temporary name, so an interrupted copy never looks like a finished file.
    static void copyChecked(File src, File dst) throws IOException {
        File tmp = new File(dst.getPath() + ".migrando");
        FileOps.Copied c;
        try (InputStream in = new FileInputStream(src)) {
            c = FileOps.copy(in, tmp, Long.MAX_VALUE);
        } catch (IOException e) {
            tmp.delete();
            throw e;
        }
        if (c.size != src.length()) {
            tmp.delete();
            throw new IOException("cópia incompleta de " + src.getName());
        }
        FileOps.move(tmp, dst);
        dst.setLastModified(src.lastModified());
    }

    static boolean sameContents(File a, File b) throws IOException {
        return a.length() == b.length() && FileOps.sha256(a).equals(FileOps.sha256(b));
    }
}
