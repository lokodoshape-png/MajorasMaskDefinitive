package com.dishii.mm;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.Before;
import org.junit.Test;

public class PortInstallTest {

    // An APK's assets in memory: path -> contents.
    static final class FakeSource implements PortInstall.Source {
        final Map<String, String> files = new LinkedHashMap<>();
        String failOn; // open() of this path throws
        String truncate; // open() of this path stops half way with an error

        @Override
        public String[] list(String path) {
            List<String> names = new ArrayList<>();
            String prefix = path.isEmpty() ? "" : path + "/";
            for (String file : files.keySet()) {
                if (file.startsWith(prefix)) {
                    String rest = file.substring(prefix.length());
                    String name = rest.contains("/") ? rest.substring(0, rest.indexOf('/')) : rest;
                    if (!names.contains(name)) {
                        names.add(name);
                    }
                }
            }
            return names.toArray(new String[0]);
        }

        @Override
        public InputStream open(String path) throws IOException {
            if (path.equals(failOn)) {
                throw new IOException("falha de leitura simulada");
            }
            String text = files.get(path);
            if (text == null) {
                throw new IOException("não existe: " + path);
            }
            byte[] data = text.getBytes(StandardCharsets.UTF_8);
            if (path.equals(truncate)) {
                return new InputStream() {
                    int pos = 0;

                    @Override
                    public int read() throws IOException {
                        if (pos >= data.length / 2) {
                            throw new IOException("leitura interrompida");
                        }
                        return data[pos++];
                    }
                };
            }
            return new ByteArrayInputStream(data);
        }
    }

    File root;
    FakeSource apk;
    List<String> log = new ArrayList<>();

    @Before
    public void setUp() throws IOException {
        root = TestFiles.tempDir();
        apk = new FakeSource();
        apk.files.put("assets/Config_N64_US.xml", "<config v1/>");
        apk.files.put("assets/xml/a.xml", "a1");
        apk.files.put("assets/xml/sub/b.xml", "b1");
        apk.files.put("assets/filelists/list.txt", "list1");
        apk.files.put("2ship.o2r", "PORT-ARCHIVE-v1");
    }

    PortInstall install() {
        return new PortInstall(root, log::add);
    }

    @Test
    public void freshInstallIsCompleteAndStamped() throws IOException {
        install().install(apk, "40/1");
        assertNull(PortInstall.problem(root, true));
        assertEquals("40/1", install().installedStamp());
        assertEquals("b1", TestFiles.read(new File(root, "assets/xml/sub/b.xml")));
        assertFalse(new File(root, PortInstall.STAGING).exists());
        assertFalse(new File(root, PortInstall.PREVIOUS).exists());
    }

    @Test
    public void updateReplacesFilesAndDropsOldOnes() throws IOException {
        install().install(apk, "40/1");
        apk.files.remove("assets/xml/a.xml");
        apk.files.put("2ship.o2r", "PORT-ARCHIVE-v2");
        install().install(apk, "40/2");
        assertEquals("40/2", install().installedStamp());
        assertEquals("PORT-ARCHIVE-v2", TestFiles.read(new File(root, "2ship.o2r")));
        assertFalse("a file the new APK does not carry must go", new File(root, "assets/xml/a.xml").exists());
    }

    @Test
    public void failedCopyKeepsTheWorkingInstall() throws IOException {
        install().install(apk, "40/1");
        apk.files.put("2ship.o2r", "PORT-ARCHIVE-v2-longer");
        apk.truncate = "2ship.o2r";
        try {
            install().install(apk, "40/2");
            fail("the broken copy must fail");
        } catch (IOException expected) {
        }
        assertEquals("40/1", install().installedStamp());
        assertEquals("PORT-ARCHIVE-v1", TestFiles.read(new File(root, "2ship.o2r")));
        assertNull(PortInstall.problem(root, true));
        assertFalse(new File(root, PortInstall.STAGING).exists());
    }

    @Test
    public void missingAssetFailsWithoutTouchingTheInstall() throws IOException {
        install().install(apk, "40/1");
        apk.failOn = "assets/xml/a.xml";
        try {
            install().install(apk, "40/2");
            fail();
        } catch (IOException expected) {
        }
        assertEquals("40/1", install().installedStamp());
    }

    @Test
    public void apkWithoutRequiredFilesIsRejected() throws IOException {
        apk.files.remove("assets/Config_N64_US.xml");
        try {
            install().install(apk, "40/1");
            fail();
        } catch (IOException expected) {
        }
        assertNull(install().installedStamp());
    }

    @Test
    public void legacyInstallWithoutManifestNeedsReinstallAndIsReplaced() throws IOException {
        TestFiles.write(new File(root, "assets/Config_N64_US.xml"), "old");
        new File(root, "assets/xml").mkdirs();
        new File(root, "assets/filelists").mkdirs();
        TestFiles.write(new File(root, "2ship.o2r"), "old archive");
        assertNull("no manifest: not trusted", install().installedStamp());
        install().install(apk, "40/1");
        assertEquals("40/1", install().installedStamp());
    }

    @Test
    public void quickCheckSeesATruncatedFile() throws IOException {
        install().install(apk, "40/1");
        TestFiles.write(new File(root, "2ship.o2r"), "PORT");
        assertNotNull(PortInstall.problem(root, false));
        assertNull(install().installedStamp());
    }

    @Test
    public void fullCheckSeesChangedContentsOfTheSameSize() throws IOException {
        install().install(apk, "40/1");
        TestFiles.write(new File(root, "assets/xml/a.xml"), "zz");
        assertNull(PortInstall.problem(root, false));
        assertNotNull(PortInstall.problem(root, true));
    }

    @Test
    public void recoverPutsBackTheOldCopyAfterACrashMidSwap() throws IOException {
        install().install(apk, "40/1");
        // State of a swap killed after the old copy was moved aside and only assets/ of the new one moved in.
        File previous = new File(root, PortInstall.PREVIOUS);
        previous.mkdirs();
        FileOps.move(new File(root, PortInstall.MANIFEST), new File(previous, PortInstall.MANIFEST));
        FileOps.move(new File(root, PortInstall.PORT_O2R), new File(previous, PortInstall.PORT_O2R));
        FileOps.move(new File(root, PortInstall.ASSETS), new File(previous, PortInstall.ASSETS));
        TestFiles.write(new File(root, "assets/xml/a.xml"), "half new");
        assertNull(install().installedStamp());

        install().recover();
        assertEquals("40/1", install().installedStamp());
        assertNull(PortInstall.problem(root, true));
        assertFalse(previous.exists());
    }

    @Test
    public void recoverAfterFinishedSwapOnlyCleansUp() throws IOException {
        install().install(apk, "40/1");
        File previous = new File(root, PortInstall.PREVIOUS);
        TestFiles.write(new File(previous, "2ship.o2r"), "stale");
        TestFiles.write(new File(root, PortInstall.STAGING + "/assets/x"), "half");
        install().recover();
        assertEquals("40/1", install().installedStamp());
        assertFalse(previous.exists());
        assertFalse(new File(root, PortInstall.STAGING).exists());
    }

    @Test
    public void manifestRejectsPathsOutsideTheFolder() {
        try {
            PortInstall.Manifest.parse("stamp x\n1 ab ../evil\n".getBytes(StandardCharsets.UTF_8));
            fail();
        } catch (IOException expected) {
        }
    }

    @Test
    public void untouchedFilesBesideTheInstallSurvive() throws IOException {
        File save = TestFiles.write(new File(root, "saves/file1.sav"), "SAVE");
        File mod = TestFiles.write(new File(root, "mods/hd.o2r"), "MOD");
        File game = TestFiles.write(new File(root, "mm.o2r"), "GAME");
        install().install(apk, "40/1");
        install().install(apk, "40/2");
        assertTrue(save.isFile() && mod.isFile() && game.isFile());
        assertEquals("SAVE", TestFiles.read(save));
    }
}
