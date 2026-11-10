package com.dishii.mm;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.Arrays;
import java.util.List;
import org.junit.Before;
import org.junit.Test;

public class DataMigrationTest {

    File source, dest;
    List<String> log = new ArrayList<>();

    @Before
    public void setUp() throws IOException {
        source = new File(TestFiles.tempDir(), "old");
        dest = new File(TestFiles.tempDir(), "new");
        source.mkdirs();
    }

    DataMigration migration() {
        return new DataMigration(dest, log::add);
    }

    @Test
    public void everyCategoryMovesWithoutASavesFolder() throws IOException {
        // No saves/ at all: mods, music, translations and settings must still come over.
        TestFiles.write(new File(source, "mods/hd.o2r"), "MOD");
        TestFiles.write(new File(source, "musicas/tema.ogg"), "OGG");
        TestFiles.write(new File(source, "traducoes/pt-BR/pt.o2r"), "PT");
        TestFiles.write(new File(source, "2ship2harkinian.json"), "{}");
        assertTrue(migration().migrate(source, "teste", true, null));
        assertEquals("MOD", TestFiles.read(new File(dest, "mods/hd.o2r")));
        assertEquals("OGG", TestFiles.read(new File(dest, "musicas/tema.ogg")));
        assertEquals("PT", TestFiles.read(new File(dest, "traducoes/pt-BR/pt.o2r")));
        assertEquals("{}", TestFiles.read(new File(dest, "2ship2harkinian.json")));
        assertFalse("an emptied source goes", source.exists());
    }

    @Test
    public void existingFilesAreNeverOverwritten() throws IOException {
        TestFiles.write(new File(dest, "saves/file1.sav"), "NEW SAVE");
        File old = TestFiles.write(new File(source, "saves/file1.sav"), "OLD SAVE");
        old.setLastModified(System.currentTimeMillis() + 100000); // even when the old one looks newer
        TestFiles.write(new File(source, "saves/file2.sav"), "SECOND");
        DataMigration m = migration();
        assertTrue(m.migrate(source, "teste", true, null));
        assertEquals("NEW SAVE", TestFiles.read(new File(dest, "saves/file1.sav")));
        assertEquals("SECOND", TestFiles.read(new File(dest, "saves/file2.sav")));
        File conflict = new File(dest, "migracao/conflitos/teste/saves/file1.sav");
        assertEquals("the other version is kept, not lost", "OLD SAVE", TestFiles.read(conflict));
        assertEquals(1, m.conflicts);
    }

    @Test
    public void copyModeLeavesTheSourceAndIsSafeToRunAgain() throws IOException {
        TestFiles.write(new File(source, "saves/file1.sav"), "S1");
        TestFiles.write(new File(source, "mods/a.o2r"), "A");
        assertTrue(migration().migrate(source, "teste", false, null));
        assertTrue(new File(source, "saves/file1.sav").isFile());
        // Interrupted run, then again: same files count as done, nothing duplicated or overwritten.
        TestFiles.write(new File(dest, "mods/a.o2r"), "A");
        DataMigration again = migration();
        assertTrue(again.migrate(source, "teste", false, null));
        assertEquals(2, again.same);
        assertEquals(0, again.conflicts);
        assertFalse(new File(dest, "migracao").exists());
    }

    @Test
    public void interruptedCopyLeftoverIsNotMistakenForTheFile() throws IOException {
        TestFiles.write(new File(source, "saves/file1.sav"), "FULL SAVE");
        TestFiles.write(new File(dest, "saves/file1.sav.migrando"), "FU");
        assertTrue(migration().migrate(source, "teste", false, null));
        assertEquals("FULL SAVE", TestFiles.read(new File(dest, "saves/file1.sav")));
        assertFalse(new File(dest, "saves/file1.sav.migrando").exists());
    }

    @Test
    public void skippedItemsStay() throws IOException {
        TestFiles.write(new File(source, "assets/x.xml"), "X");
        TestFiles.write(new File(source, "2ship.o2r"), "P");
        TestFiles.write(new File(source, "saves/f.sav"), "S");
        assertTrue(migration().migrate(source, "teste", true,
                new HashSet<>(Arrays.asList("assets", "2ship.o2r"))));
        assertFalse(new File(dest, "assets").exists());
        assertTrue(new File(source, "2ship.o2r").isFile());
        assertEquals("S", TestFiles.read(new File(dest, "saves/f.sav")));
    }

    @Test
    public void fileWhereAFolderIsExpectedBecomesAConflict() throws IOException {
        TestFiles.write(new File(dest, "mods"), "not a folder");
        TestFiles.write(new File(source, "mods/a.o2r"), "A");
        DataMigration m = migration();
        assertTrue(m.migrate(source, "teste", false, Collections.<String>emptySet()));
        assertEquals("not a folder", TestFiles.read(new File(dest, "mods")));
        assertEquals("A", TestFiles.read(new File(dest, "migracao/conflitos/teste/mods/a.o2r")));
    }

    @Test
    public void everyStepIsLogged() throws IOException {
        TestFiles.write(new File(source, "saves/f.sav"), "S");
        migration().migrate(source, "teste", true, null);
        assertTrue(log.get(0).startsWith("Migração teste"));
        assertTrue(log.get(log.size() - 1).contains("falhas"));
    }
}
