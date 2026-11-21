package com.voyemu.prepare;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.List;

/**
 * The Russian letters of the DFP King Gothic GB Regular and Semibold fonts of the car advance a
 * full em, which spaces Russian text out; each known original becomes its one corrected font.
 */
final class FontCyrillicFix {
    /** An accepted original and the corrected font it becomes. */
    record Identity(
            String name,
            int inputBytes,
            String inputSha256,
            int outputBytes,
            String outputSha256,
            int glyphs,
            boolean expandMetrics) {}

    // Regular stores the advances of its last glyphs once; they are expanded before the repair.
    static final Identity REGULAR =
            new Identity(
                    "regular",
                    1_697_868,
                    "b90b145194f1206b3d515e05d2bc2f27f2411aa0b6897f61755d7b812fe083c1",
                    1_712_864,
                    "4f03e27101265730adf4634746350eb44aef725e8d8601a48a366a03cf8b6bd0",
                    64,
                    true);
    static final Identity SEMIBOLD =
            new Identity(
                    "semibold",
                    1_644_272,
                    "305f942097a1b65d1dca1c357c0197acf62346eea83e2fbe95cc55f62821f000",
                    1_644_272,
                    "e8e460cdb59f41367d7c345aa1004f7d65c14c27685e60da5646a1385e846777",
                    63,
                    false);
    static final List<Identity> IDENTITIES = List.of(REGULAR, SEMIBOLD);

    private FontCyrillicFix() {}

    static void require(boolean value, String reason) {
        Failure.require(value, "FONT_CYRILLIC", reason, 3);
    }

    static String sha256(byte[] bytes) {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 unavailable", error);
        }
    }

    /** The accepted original that source is, or null. */
    static Identity identity(byte[] source) {
        String digest = sha256(source);

        for (Identity known : IDENTITIES) {
            if (source.length == known.inputBytes() && digest.equals(known.inputSha256())) {
                return known;
            }
        }

        return null;
    }

    /** The corrected font of an accepted original; any other input fails. */
    static byte[] correct(byte[] source) {
        Identity known = identity(source);

        require(known != null, "Font is not an accepted DFP King Gothic GB original");
        byte[] input = known.expandMetrics() ? expandMetrics(source) : source;
        FontCyrillicAdvanceFix.Result result = FontCyrillicAdvanceFix.patch(input);
        require(
                result.glyphsPatched() == known.glyphs()
                        && result.bytes().length == known.outputBytes()
                        && sha256(result.bytes()).equals(known.outputSha256())
                        && !FontCyrillicAdvanceFix.patch(result.bytes()).changed(),
                "Corrected " + known.name() + " font differs: " + result.reason());

        return result.bytes();
    }

    // Gives every glyph its own advance in hmtx, moving the tables behind it.
    static byte[] expandMetrics(byte[] source) {
        ByteBuffer input = ByteBuffer.wrap(source).order(ByteOrder.BIG_ENDIAN);
        int hmtx = record(input, "hmtx");
        int hhea = record(input, "hhea");
        int maxp = record(input, "maxp");
        int count = input.getShort(input.getInt(maxp + 8) + 4) & 0xffff;
        int metrics = input.getShort(input.getInt(hhea + 8) + 34) & 0xffff;
        int offset = input.getInt(hmtx + 8);
        int oldLength = (input.getInt(hmtx + 12) + 3) & ~3;
        int length = count * 4;
        int delta = length - oldLength;
        byte[] expanded = new byte[source.length + delta];

        System.arraycopy(source, 0, expanded, 0, offset);
        System.arraycopy(
                source,
                offset + oldLength,
                expanded,
                offset + length,
                source.length - offset - oldLength);
        ByteBuffer output = ByteBuffer.wrap(expanded).order(ByteOrder.BIG_ENDIAN);
        int advance = input.getShort(offset + (metrics - 1) * 4) & 0xffff;

        for (int glyph = 0; glyph < count; glyph++) {
            if (glyph < metrics) {
                output.putInt(offset + glyph * 4, input.getInt(offset + glyph * 4));
            } else {
                output.putShort(offset + glyph * 4, (short) advance);
                output.putShort(
                        offset + glyph * 4 + 2,
                        input.getShort(offset + metrics * 4 + (glyph - metrics) * 2));
            }
        }

        int tables = input.getShort(4) & 0xffff;

        for (int i = 0; i < tables; i++) {
            int entry = 12 + i * 16;
            int tableOffset = input.getInt(entry + 8);

            if (tableOffset >= offset + oldLength) {
                output.putInt(entry + 8, tableOffset + delta);
            }
        }

        output.putInt(hmtx + 12, length);
        output.putShort(output.getInt(hhea + 8) + 34, (short) count);
        output.putInt(
                hhea + 4,
                (int)
                        FontCyrillicAdvanceFix.checksum(
                                expanded, output.getInt(hhea + 8), output.getInt(hhea + 12)));

        return expanded;
    }

    private static int record(ByteBuffer font, String tag) {
        int tables = font.getShort(4) & 0xffff;
        int expected =
                (tag.charAt(0) << 24)
                        | (tag.charAt(1) << 16)
                        | (tag.charAt(2) << 8)
                        | tag.charAt(3);

        for (int i = 0; i < tables; i++) {
            int offset = 12 + i * 16;

            if (font.getInt(offset) == expected) {
                return offset;
            }
        }

        throw new Failure("FONT_CYRILLIC", "Font table missing: " + tag, 3);
    }
}
