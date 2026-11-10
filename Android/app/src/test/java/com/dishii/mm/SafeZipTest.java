package com.dishii.mm;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;
import org.junit.Before;
import org.junit.Test;

public class SafeZipTest {

    File dir;

    @Before
    public void setUp() throws IOException {
        dir = TestFiles.tempDir();
    }

    // name, contents pairs; contents may be a byte[] or a String.
    File zip(String file, Object... entries) throws IOException {
        File out = new File(dir, file);
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(out))) {
            for (int i = 0; i < entries.length; i += 2) {
                zip.putNextEntry(new ZipEntry((String) entries[i]));
                Object data = entries[i + 1];
                zip.write(data instanceof byte[] ? (byte[]) data : ((String) data).getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
        }
        return out;
    }

    byte[] innerO2r(String text) throws IOException {
        File f = zip("inner-" + text.hashCode() + ".o2r", "textures/a.png", text);
        return java.nio.file.Files.readAllBytes(f.toPath());
    }

    @Test
    public void entryNames() {
        assertTrue(SafeZip.isSafeEntryName("mods/hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("../hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("a/../../hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("/sdcard/hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("C:/hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("a\\..\\hd.o2r"));
        assertFalse(SafeZip.isSafeEntryName("a\u0001.o2r"));
        assertEquals("hd.o2r", SafeZip.baseName("x/y/hd.o2r"));
    }

    @Test
    public void wrapperZipHasItsModsTakenOut() throws IOException {
        File z = zip("pack.zip", "readme.txt", "hi", "pack/hd.o2r", innerO2r("HD"), "pack/fix.o2r", innerO2r("FIX"));
        File out = new File(dir, "out");
        out.mkdirs();
        try (ZipFile zip = new ZipFile(z)) {
            SafeZip.Scan scan = SafeZip.scan(zip);
            assertTrue(scan.wrapsArchives());
            List<File> files = SafeZip.extractArchives(zip, scan, out);
            assertEquals(2, files.size());
            for (File f : files) {
                assertNull(SafeZip.archiveProblem(f));
                assertEquals(out.getCanonicalFile(), f.getParentFile().getCanonicalFile());
            }
        }
    }

    @Test
    public void modArchiveZipIsNotAWrapper() throws IOException {
        File z = zip("textures.zip", "textures/a.png", "x", "textures/b.bin", "y");
        try (ZipFile zip = new ZipFile(z)) {
            assertFalse(SafeZip.scan(zip).wrapsArchives());
        }
        assertNull(SafeZip.archiveProblem(z));
    }

    @Test
    public void traversalNamesAreRefused() throws IOException {
        File z = zip("evil.zip", "../../evil.o2r", innerO2r("E"));
        File out = new File(dir, "out");
        out.mkdirs();
        try (ZipFile zip = new ZipFile(z)) {
            SafeZip.Scan scan = SafeZip.scan(zip);
            SafeZip.extractArchives(zip, scan, out);
            fail("a name leaving the folder must be refused");
        } catch (IOException expected) {
        }
        assertFalse(new File(dir.getParentFile(), "evil.o2r").exists());
    }

    @Test
    public void zipBombIsRefused() throws IOException {
        byte[] zeros = new byte[16 << 20]; // 16 MiB of zeros deflates to a few KiB
        File z = zip("bomb.zip", "bomb.o2r", zeros);
        File out = new File(dir, "out");
        out.mkdirs();
        try (ZipFile zip = new ZipFile(z)) {
            SafeZip.Scan scan = SafeZip.scan(zip);
            SafeZip.extractArchives(zip, scan, out);
            fail("an entry growing more than MAX_RATIO times must be refused");
        } catch (IOException expected) {
            assertTrue(expected.getMessage(), expected.getMessage().contains("limite"));
        }
    }

    @Test
    public void tooManyArchivesAreRefused() throws IOException {
        Object[] entries = new Object[(SafeZip.MAX_ARCHIVES + 1) * 2];
        for (int i = 0; i <= SafeZip.MAX_ARCHIVES; i++) {
            entries[i * 2] = "m" + i + ".o2r";
            entries[i * 2 + 1] = "x";
        }
        File z = zip("many.zip", entries);
        try (ZipFile zip = new ZipFile(z)) {
            SafeZip.extractArchives(zip, SafeZip.scan(zip), dir);
            fail();
        } catch (IOException expected) {
        }
    }

    @Test
    public void damagedArchivesAreSeen() throws IOException {
        File broken = TestFiles.write(new File(dir, "broken.o2r"), "PK not really a zip");
        assertNotNull(SafeZip.archiveProblem(broken));
        File empty = TestFiles.write(new File(dir, "empty.o2r"), "");
        assertNotNull(SafeZip.archiveProblem(empty));
        File otr = TestFiles.write(new File(dir, "old.otr"), "MPQ\u001a....");
        assertNull(SafeZip.archiveProblem(otr));
        File badOtr = TestFiles.write(new File(dir, "bad.otr"), "nope");
        assertNotNull(SafeZip.archiveProblem(badOtr));
    }

    @Test
    public void duplicateNamesInOneZipAreRefused() throws IOException {
        File z = zip("dup.zip", "a/hd.o2r", innerO2r("1"), "b/hd.o2r", innerO2r("2"));
        File out = new File(dir, "out");
        out.mkdirs();
        try (ZipFile zip = new ZipFile(z)) {
            SafeZip.extractArchives(zip, SafeZip.scan(zip), out);
            fail();
        } catch (IOException expected) {
        }
    }

    @Test
    public void storedEntriesAreNotHeldToTheRatio() throws IOException {
        // .o2r inside a zip are often stored (not deflated): no ratio applies, only the size limits.
        File out = new File(dir, "stored.zip");
        byte[] data = innerO2r("STORED");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(out))) {
            ZipEntry e = new ZipEntry("hd.o2r");
            e.setMethod(ZipEntry.STORED);
            e.setSize(data.length);
            CRC32 crc = new CRC32();
            crc.update(data);
            e.setCrc(crc.getValue());
            zip.putNextEntry(e);
            zip.write(data);
            zip.closeEntry();
        }
        File dest = new File(dir, "o");
        dest.mkdirs();
        try (ZipFile zip = new ZipFile(out)) {
            assertEquals(1, SafeZip.extractArchives(zip, SafeZip.scan(zip), dest).size());
        }
    }
}
