package com.voyemu.prepare;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

public final class FontCyrillicFixTest {
    private static Path fixtures;

    static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    static byte[] fixture(String name) throws Exception {
        return Files.readAllBytes(fixtures.resolve(name));
    }

    static void unchanged(byte[] value, String reason) {
        byte[] before = value == null ? null : value.clone();
        FontCyrillicAdvanceFix.Result result = FontCyrillicAdvanceFix.patch(value);

        check(!result.changed(), "unchanged " + reason);
        check(reason.equals(result.reason()), "reason " + result.reason() + " for " + reason);
        check(result.bytes() == value && Arrays.equals(before, value), "input kept " + reason);
    }

    static int tableOffset(byte[] font, String tag) {
        int count = ((font[4] & 0xff) << 8) | (font[5] & 0xff);

        for (int i = 0; i < count; i++) {
            int entry = 12 + i * 16;

            if (tag.equals(
                    new String(font, entry, 4, java.nio.charset.StandardCharsets.US_ASCII))) {
                return java.nio.ByteBuffer.wrap(font).getInt(entry + 8);
            }
        }

        throw new AssertionError("missing table: " + tag);
    }

    static void putU16(byte[] bytes, int offset, int value) {
        bytes[offset] = (byte) (value >>> 8);
        bytes[offset + 1] = (byte) value;
    }

    static void putU32(byte[] bytes, int offset, long value) {
        putU16(bytes, offset, (int) (value >>> 16));
        putU16(bytes, offset + 2, (int) value);
    }

    static void syntheticFonts() throws Exception {
        byte[] regular = fixture("regular-defective.ttf");
        byte[] before = regular.clone();
        FontCyrillicAdvanceFix.Result repaired = FontCyrillicAdvanceFix.patch(regular);

        check(repaired.glyphsPatched() == 64, "regular repairs 64 letters");
        check(
                Arrays.equals(fixture("regular-corrected.ttf"), repaired.bytes()),
                "regular matches its corrected fixture");
        check(!FontCyrillicAdvanceFix.patch(repaired.bytes()).changed(), "regular is idempotent");
        check(Arrays.equals(before, regular), "regular input kept");

        byte[] semibold = fixture("semibold-defective.ttf");
        FontCyrillicAdvanceFix.Result result = FontCyrillicAdvanceFix.patch(semibold);
        check(result.glyphsPatched() == 63 && "patched".equals(result.reason()), "semibold 63");
        check(
                Arrays.equals(fixture("semibold-corrected.ttf"), result.bytes()),
                "semibold matches its corrected fixture");
        check(
                "4266f1e6b585645b484a467938ed88301d530be05862fb15b86dd607747d1632"
                        .equals(FontCyrillicFix.sha256(result.bytes())),
                "semibold corrected identity");
        byte[] corrected = fixture("semibold-corrected.ttf");
        FontCyrillicAdvanceFix.Result again = FontCyrillicAdvanceFix.patch(corrected);
        check(
                !again.changed()
                        && again.bytes() == corrected
                        && ("already-correct".equals(again.reason())
                                || "unknown-metrics".equals(again.reason())),
                "corrected font stays unchanged");
    }

    // Only the two OEM originals have a corrected font; a synthetic font is not one of them.
    static void identities() throws Exception {
        for (String name : new String[] {"regular-defective.ttf", "semibold-defective.ttf"}) {
            byte[] source = fixture(name);
            byte[] before = source.clone();
            check(FontCyrillicFix.identity(source) == null, "synthetic font is no original");
            StorageJsonTest.rejected(() -> FontCyrillicFix.correct(source));
            check(Arrays.equals(before, source), "rejected input kept");
        }
    }

    static void malformedInputs() throws Exception {
        FontCyrillicAdvanceFix.Result missing = FontCyrillicAdvanceFix.patch(null);

        check(!missing.changed() && "font-size".equals(missing.reason()), "null input");
        unchanged(new byte[0], "font-size");
        unchanged(new byte[11], "font-size");
        unchanged(fixture("malformed-sfnt.bin"), "sfnt-version");
        unchanged(new byte[FontCyrillicAdvanceFix.MAX_FONT_BYTES + 1], "font-size");
        byte[] source = fixture("semibold-defective.ttf");

        for (int length :
                new int[] {12, 27, 63, 171, 255, 1023, source.length / 2, source.length - 1}) {
            byte[] truncated = Arrays.copyOf(source, length);
            FontCyrillicAdvanceFix.Result result = FontCyrillicAdvanceFix.patch(truncated);
            check(!result.changed() && result.bytes() == truncated, "truncated " + length);
        }

        byte[] noTables = source.clone();
        noTables[4] = 0;
        noTables[5] = 0;
        unchanged(noTables, "malformed-sfnt");
        byte[] duplicateTag = source.clone();
        System.arraycopy(duplicateTag, 12, duplicateTag, 28, 4);
        unchanged(duplicateTag, "malformed-sfnt");
        byte[] overlapping = source.clone();
        System.arraycopy(overlapping, 20, overlapping, 36, 4);
        unchanged(overlapping, "malformed-sfnt");
        byte[] unaligned = source.clone();
        unaligned[23] = (byte) (unaligned[23] | 1);
        unchanged(unaligned, "malformed-sfnt");

        for (int offset :
                new int[] {
                    0,
                    4,
                    11,
                    12,
                    20,
                    24,
                    144,
                    tableOffset(source, "head") + 18,
                    tableOffset(source, "hmtx") + 4,
                    source.length - 1
                }) {
            byte[] candidate = source.clone();
            candidate[offset] ^= 0x01;
            byte[] before = candidate.clone();
            FontCyrillicAdvanceFix.Result result = FontCyrillicAdvanceFix.patch(candidate);
            check(Arrays.equals(before, candidate), "bit flip input kept " + offset);

            if (!result.changed()) {
                check(result.bytes() == candidate, "bit flip unchanged " + offset);
            }
        }
    }

    static void format12Cmap() {
        byte[] cmap = new byte[40];

        putU16(cmap, 2, 1);
        putU16(cmap, 4, 3);
        putU16(cmap, 6, 10);
        putU32(cmap, 8, 12);
        putU16(cmap, 12, 12);
        putU32(cmap, 16, 28);
        putU32(cmap, 24, 1);
        putU32(cmap, 28, 0x0410);
        putU32(cmap, 32, 0x044f);
        putU32(cmap, 36, 100);
        var table = new FontCyrillicAdvanceFix.Table(0, 0, cmap.length);
        check(
                Integer.valueOf(100)
                        .equals(FontCyrillicAdvanceFix.mappedGlyph(cmap, table, 0x0410)),
                "format 12 first");
        check(
                Integer.valueOf(163)
                        .equals(FontCyrillicAdvanceFix.mappedGlyph(cmap, table, 0x044f)),
                "format 12 last");
        check(FontCyrillicAdvanceFix.mappedGlyph(cmap, table, 0x040f) == null, "format 12 below");
        check(FontCyrillicAdvanceFix.mappedGlyph(cmap, table, 0x0450) == null, "format 12 above");
        byte[] shortLength = cmap.clone();
        putU32(shortLength, 16, 15);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(shortLength, table, 0x0410) == null,
                "format 12 short");
        byte[] tooManyGroups = cmap.clone();
        putU32(tooManyGroups, 24, 2);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(tooManyGroups, table, 0x0410) == null,
                "format 12 groups");
        byte[] overflowing = cmap.clone();
        putU32(overflowing, 36, 0x10000);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(overflowing, table, 0x0410) == null,
                "format 12 glyph overflow");
        byte[] outOfBounds = cmap.clone();
        putU32(outOfBounds, 16, cmap.length + 4L);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(outOfBounds, table, 0x0410) == null,
                "format 12 bounds");
    }

    static byte[] format4(boolean explicitRange) {
        byte[] cmap = new byte[explicitRange ? 50 : 48];

        putU16(cmap, 2, 1);
        putU16(cmap, 4, 3);
        putU16(cmap, 6, 1);
        putU32(cmap, 8, 12);
        putU16(cmap, 12, 4);
        putU16(cmap, 14, explicitRange ? 38 : 36);
        putU16(cmap, 18, 2);
        putU16(cmap, 20, 2);
        putU16(cmap, 26, explicitRange ? 0x0410 : 0x044f);
        putU16(cmap, 30, 0x0410);
        putU16(cmap, 32, explicitRange ? 0 : (100 - 0x0410) & 0xffff);
        putU16(cmap, 34, explicitRange ? 2 : 0);

        if (explicitRange) {
            putU16(cmap, 36, 123);
        }

        return cmap;
    }

    static void format4Cmap() {
        byte[] delta = format4(false);
        var table = new FontCyrillicAdvanceFix.Table(0, 0, delta.length);

        check(
                Integer.valueOf(100)
                        .equals(FontCyrillicAdvanceFix.mappedGlyph(delta, table, 0x0410)),
                "format 4 first");
        check(
                Integer.valueOf(163)
                        .equals(FontCyrillicAdvanceFix.mappedGlyph(delta, table, 0x044f)),
                "format 4 last");
        check(FontCyrillicAdvanceFix.mappedGlyph(delta, table, 0x040f) == null, "format 4 below");
        byte[] range = format4(true);
        var rangeTable = new FontCyrillicAdvanceFix.Table(0, 0, range.length);
        check(
                Integer.valueOf(123)
                        .equals(FontCyrillicAdvanceFix.mappedGlyph(range, rangeTable, 0x0410)),
                "format 4 range");
        putU16(range, 36, 0);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(range, rangeTable, 0x0410) == null,
                "format 4 missing glyph");
        byte[] shortLength = delta.clone();
        putU16(shortLength, 14, 15);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(shortLength, table, 0x0410) == null,
                "format 4 short");
        byte[] noSegments = delta.clone();
        putU16(noSegments, 18, 0);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(noSegments, table, 0x0410) == null,
                "format 4 segments");
        byte[] outside = delta.clone();
        putU16(outside, 14, delta.length + 4);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(outside, table, 0x0410) == null,
                "format 4 bounds");
        byte[] badRange = format4(true);
        putU16(badRange, 34, 0x7fff);
        check(
                FontCyrillicAdvanceFix.mappedGlyph(
                                badRange,
                                new FontCyrillicAdvanceFix.Table(0, 0, badRange.length),
                                0x0410)
                        == null,
                "format 4 range bounds");
    }

    public static void main(String[] args) throws Exception {
        fixtures = Path.of(args[0], "src/prepare/test/fixtures/font-cyrillic");
        syntheticFonts();
        identities();
        malformedInputs();
        format12Cmap();
        format4Cmap();
        System.out.println("Cyrillic font repair passed");
    }
}
