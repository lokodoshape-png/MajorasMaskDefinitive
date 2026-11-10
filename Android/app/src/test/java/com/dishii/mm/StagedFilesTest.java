package com.dishii.mm;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Before;
import org.junit.Test;

public class StagedFilesTest {

    File root, target, works;
    List<String> log = new ArrayList<>();

    @Before
    public void setUp() throws IOException {
        root = TestFiles.tempDir();
        target = new File(root, "mods");
        works = new File(root, ".mods-instalando");
    }

    @Test
    public void replacesSameNameAndKeepsTheRest() throws IOException {
        TestFiles.write(new File(target, "hd.o2r"), "OLD");
        TestFiles.write(new File(target, "other.o2r"), "OTHER");
        File work = StagedFiles.newWorkFolder(works);
        File fresh = TestFiles.write(new File(work, "pronto/hd.o2r"), "NEW");
        StagedFiles.commit(work, Arrays.asList(fresh), target, false);
        assertEquals("NEW", TestFiles.read(new File(target, "hd.o2r")));
        assertEquals("OTHER", TestFiles.read(new File(target, "other.o2r")));
        assertFalse(work.exists());
    }

    @Test
    public void replaceAllSwapsTheWholeFolder() throws IOException {
        TestFiles.write(new File(target, "pt-old.o2r"), "OLD");
        File work = StagedFiles.newWorkFolder(works);
        File fresh = TestFiles.write(new File(work, "pronto/pt-new.o2r"), "NEW");
        StagedFiles.commit(work, Arrays.asList(fresh), target, true);
        assertFalse(new File(target, "pt-old.o2r").exists());
        assertEquals("NEW", TestFiles.read(new File(target, "pt-new.o2r")));
    }

    @Test
    public void failedMoveRestoresTheOldFiles() throws IOException {
        TestFiles.write(new File(target, "pt-old.o2r"), "OLD");
        File work = StagedFiles.newWorkFolder(works);
        File missing = new File(work, "pronto/never-written.o2r"); // the move of this one fails
        try {
            StagedFiles.commit(work, Arrays.asList(missing), target, true);
        } catch (IOException expected) {
        }
        assertEquals("OLD", TestFiles.read(new File(target, "pt-old.o2r")));
    }

    @Test
    public void recoverUndoesASwapStoppedHalfWay() throws IOException {
        TestFiles.write(new File(target, "pt-old.o2r"), "OLD");
        File work = StagedFiles.newWorkFolder(works);
        // Killed after the old file was moved aside and the new one moved in, before the marker went.
        TestFiles.write(new File(work, StagedFiles.COMMITTING), target.getAbsolutePath() + "\npt-new.o2r");
        new File(work, StagedFiles.BACKUP).mkdirs();
        FileOps.move(new File(target, "pt-old.o2r"), new File(work, StagedFiles.BACKUP + "/pt-old.o2r"));
        TestFiles.write(new File(target, "pt-new.o2r"), "NEW");
        assertEquals(1, StagedFiles.recover(works, log::add));
        assertEquals("OLD", TestFiles.read(new File(target, "pt-old.o2r")));
        assertFalse(new File(target, "pt-new.o2r").exists());
        assertFalse(work.exists());
    }

    @Test
    public void recoverDropsUnfinishedCopies() throws IOException {
        File work = StagedFiles.newWorkFolder(works);
        TestFiles.write(new File(work, "recebido.zip"), "half");
        assertEquals(0, StagedFiles.recover(works, log::add));
        assertFalse(work.exists());
        assertTrue(works.isDirectory());
    }
}
