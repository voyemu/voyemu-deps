package com.voyemu.prepare;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/** Writes the corrected font of an accepted DFP King Gothic GB original to a new file. */
public final class FontCyrillicMain {
    private FontCyrillicMain() {}

    public static void main(String[] args) {
        try {
            Failure.require(
                    args.length == 2, "FONT_CYRILLIC_USAGE", "FontCyrillicMain INPUT OUTPUT", 2);
            Path input = Path.of(args[0]).toAbsolutePath().normalize();
            Path output = Path.of(args[1]).toAbsolutePath().normalize();
            FontCyrillicFix.require(
                    Files.isRegularFile(input)
                            && !Files.isSymbolicLink(input)
                            && Files.size(input) <= FontCyrillicAdvanceFix.MAX_FONT_BYTES,
                    "Bounded font input required");
            byte[] corrected = FontCyrillicFix.correct(Files.readAllBytes(input));
            Files.write(output, corrected, StandardOpenOption.CREATE_NEW);
            System.out.println(
                    "{\"bytes\":"
                            + corrected.length
                            + ",\"sha256\":\""
                            + FontCyrillicFix.sha256(corrected)
                            + "\"}");
        } catch (Failure error) {
            System.err.println(error.code + ": " + error.getMessage());
            System.exit(error.exitCode);
        } catch (Exception error) {
            // checkstyle-allow IllegalCatch: process boundary reports every failure
            System.err.println("FONT_CYRILLIC: " + error);
            System.exit(1);
        }
    }
}
