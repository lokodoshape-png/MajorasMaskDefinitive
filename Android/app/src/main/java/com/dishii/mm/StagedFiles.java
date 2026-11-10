package com.dishii.mm;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

// Puts checked files into a folder the game reads (mods, translation packs) without a moment where the old ones are
// gone and the new ones are not there yet. Files are made and checked in a work folder outside the target (the game
// scans mods/ and traducoes/<language>/ recursively, so a half-copied file there would be loaded), then:
//   1. every file that will be replaced (or, with replaceAll, every file in the target) moves into <work>/anterior;
//   2. the new files move in;
//   3. <work> is deleted.
// A failure in 2 moves the old files back. If the app is closed in the middle, recover() does the same at the
// next start. Plain Java, tested in src/test.
final class StagedFiles {

    static final String BACKUP = "anterior";
    // Present while step 2 runs: recover() then puts the old files back.
    static final String COMMITTING = "trocando";

    private StagedFiles() {
    }

    static File newWorkFolder(File parent) throws IOException {
        FileOps.mkdirs(parent);
        for (int i = 0; i < 100; i++) {
            File work = new File(parent, System.currentTimeMillis() + "-" + i);
            if (!work.exists() && work.mkdirs()) {
                return work;
            }
        }
        throw new IOException("não foi possível criar uma pasta em " + parent.getPath());
    }

    static void commit(File work, List<File> files, File target, boolean replaceAll) throws IOException {
        FileOps.mkdirs(target);
        File backup = new File(work, BACKUP);
        FileOps.mkdirs(backup);
        List<File> toBackUp = new ArrayList<>();
        if (replaceAll) {
            for (File old : FileOps.list(target)) {
                if (old.isFile()) {
                    toBackUp.add(old);
                }
            }
        } else {
            for (File file : files) {
                File old = new File(target, file.getName());
                if (old.exists()) {
                    toBackUp.add(old);
                }
            }
        }
        File marker = new File(work, COMMITTING);
        StringBuilder record = new StringBuilder(target.getAbsolutePath());
        for (File file : files) {
            record.append('\n').append(file.getName());
        }
        FileOps.writeAtomically(marker, record.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        List<File> movedIn = new ArrayList<>();
        try {
            for (File old : toBackUp) {
                FileOps.move(old, new File(backup, old.getName()));
            }
            for (File file : files) {
                File dest = new File(target, file.getName());
                if (!FileOps.isInside(target, dest)) {
                    throw new IOException("caminho inválido: " + file.getName());
                }
                FileOps.move(file, dest);
                movedIn.add(dest);
            }
        } catch (IOException e) {
            for (File dest : movedIn) {
                dest.delete();
            }
            restore(backup, target);
            marker.delete();
            throw e;
        }
        if (!marker.delete()) {
            throw new IOException("não foi possível terminar a troca em " + work.getPath());
        }
        FileOps.deleteRecursive(work);
    }

    // Old files back into `target`, where nothing new took their place.
    private static void restore(File backup, File target) {
        File[] kept = backup.listFiles();
        if (kept == null) {
            return;
        }
        for (File old : kept) {
            File dest = new File(target, old.getName());
            if (dest.exists()) {
                dest.delete(); // a new, possibly incomplete file
            }
            old.renameTo(dest);
        }
    }

    // Leftovers of installs in `parent` (a folder of work folders): one stopped while files were being swapped has
    // its old files put back; the rest are deleted. Returns how many swaps were undone.
    static int recover(File parent, StagedFiles.Log log) {
        File[] works = parent.listFiles();
        if (works == null) {
            return 0;
        }
        int undone = 0;
        for (File work : works) {
            File marker = new File(work, COMMITTING);
            if (marker.isFile()) {
                try {
                    String[] record = new String(FileOps.readAll(marker),
                            java.nio.charset.StandardCharsets.UTF_8).split("\n");
                    String target = record[0].trim();
                    if (target.isEmpty()) {
                        throw new IOException("registro vazio");
                    }
                    // The new files that made it in go, the old ones come back.
                    for (int i = 1; i < record.length; i++) {
                        String name = FileOps.safeFileName(record[i], "");
                        if (!name.isEmpty()) {
                            new File(target, name).delete();
                        }
                    }
                    restore(new File(work, BACKUP), new File(target));
                    undone++;
                    log.line("Troca interrompida em " + target + ": arquivos anteriores de volta");
                } catch (IOException e) {
                    log.line("Não foi possível desfazer a troca em " + work.getPath() + ": " + e.getMessage());
                    continue; // kept for a later try
                }
            }
            FileOps.deleteRecursive(work);
        }
        return undone;
    }

    interface Log {
        void line(String text);
    }
}
