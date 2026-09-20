package com.example.Janus;
import java.io.*; import java.nio.file.*;

public class NativeLoader {
    static void load(String lib) {
        try { System.loadLibrary(lib); return; } catch (UnsatisfiedLinkError ignored) {}
        String os = System.getProperty("os.name").toLowerCase();
        // String file = os.contains("win") ? lib + ".dll"
        //             : os.contains("mac") ? "lib" + lib + ".dylib" : "lib" + lib + ".so";
        String file = lib + ".dll";
        try (InputStream in = NativeLoader.class.getResourceAsStream("/native/" + file)) {
            if (in == null) throw new UnsatisfiedLinkError("no bundled " + file);
            Path tmp = Files.createTempFile(lib, file.substring(file.lastIndexOf('.')));
            Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
            tmp.toFile().deleteOnExit();
            System.load(tmp.toAbsolutePath().toString());
        } catch (IOException e) { throw new UnsatisfiedLinkError(e.getMessage()); }
    }
}