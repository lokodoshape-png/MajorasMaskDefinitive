package com.dishii.mm;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

final class TestFiles {
    private TestFiles() {
    }

    static File tempDir() throws IOException {
        File dir = Files.createTempDirectory("mmade-test").toFile();
        dir.deleteOnExit();
        return dir;
    }

    static File write(File file, String text) throws IOException {
        file.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
        return file;
    }

    static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }
}
