package com.voyemu.prepare;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Moves the outline of each Russian letter so that its left edge is the left side bearing of hmtx.
 * The advance repair narrows the advances and sets the side bearings, which FreeType honours; a
 * rasterizer that places outlines at their own coordinates, such as stb_truetype of the window,
 * needs the outlines themselves at those side bearings.
 */
final class FontCyrillicOutlineFix {
    private static final int ON_CURVE = 0x01;
    private static final int X_SHORT = 0x02;
    private static final int Y_SHORT = 0x04;
    private static final int REPEAT = 0x08;
    private static final int X_SAME = 0x10;
    private static final int Y_SAME = 0x20;
    private static final int OVERLAP = 0x40;
    private static final long CHECKSUM_MAGIC = 0xb1b0afbaL;

    private FontCyrillicOutlineFix() {}

    /** The font with every Russian letter outline at its side bearing; source stays unchanged. */
    static byte[] align(byte[] source) {
        ByteBuffer font = ByteBuffer.wrap(source);
        Map<String, FontCyrillicAdvanceFix.Table> tables = FontCyrillicAdvanceFix.tables(source);
        FontCyrillicAdvanceFix.Table head = FontCyrillicAdvanceFix.required(tables, "head", 54);
        FontCyrillicAdvanceFix.Table hhea = FontCyrillicAdvanceFix.required(tables, "hhea", 36);
        FontCyrillicAdvanceFix.Table maxp = FontCyrillicAdvanceFix.required(tables, "maxp", 6);
        FontCyrillicAdvanceFix.Table hmtx = FontCyrillicAdvanceFix.required(tables, "hmtx", 4);
        FontCyrillicAdvanceFix.Table loca = FontCyrillicAdvanceFix.required(tables, "loca", 4);
        FontCyrillicAdvanceFix.Table glyf = FontCyrillicAdvanceFix.required(tables, "glyf", 1);
        Set<Integer> letters =
                FontCyrillicAdvanceFix.coreRussianGlyphs(
                        source, FontCyrillicAdvanceFix.required(tables, "cmap", 4));
        int glyphs = font.getShort(maxp.offset() + 4) & 0xffff;
        int metrics = font.getShort(hhea.offset() + 34) & 0xffff;
        boolean longLoca = font.getShort(head.offset() + 50) == 1;
        ByteArrayOutputStream outlines = new ByteArrayOutputStream(glyf.length());
        int[] offsets = new int[glyphs + 1];

        for (int glyph = 0; glyph < glyphs; glyph++) {
            int start = locaEntry(font, loca, longLoca, glyph);
            int end = locaEntry(font, loca, longLoca, glyph + 1);
            FontCyrillicFix.require(
                    start <= end && end <= glyf.length(), "Font glyph outside its table");
            byte[] outline = new byte[end - start];
            font.get(glyf.offset() + start, outline);

            if (letters.contains(glyph) && outline.length > 0 && glyph < metrics) {
                int sideBearing = font.getShort(hmtx.offset() + glyph * 4 + 2);
                outline = translate(outline, sideBearing - ByteBuffer.wrap(outline).getShort(2));
            }

            offsets[glyph] = outlines.size();
            outlines.writeBytes(outline);

            while (outlines.size() % 4 != 0) {
                outlines.write(0);
            }
        }

        offsets[glyphs] = outlines.size();
        ByteBuffer index = ByteBuffer.allocate((glyphs + 1) * (longLoca ? 4 : 2));

        for (int offset : offsets) {
            if (longLoca) {
                index.putInt(offset);
            } else {
                FontCyrillicFix.require(offset / 2 <= 0xffff, "Short loca cannot hold the font");
                index.putShort((short) (offset / 2));
            }
        }

        return rebuild(
                source, tables, Map.of("glyf", outlines.toByteArray(), "loca", index.array()));
    }

    private static int locaEntry(
            ByteBuffer font, FontCyrillicAdvanceFix.Table loca, boolean longLoca, int glyph) {
        return longLoca
                ? font.getInt(loca.offset() + glyph * 4)
                : (font.getShort(loca.offset() + glyph * 2) & 0xffff) * 2;
    }

    // The flags and absolute x of each point of a simple outline, and its y coordinate bytes.
    private record Points(int[] flags, int[] xs, int yStart, int yEnd) {}

    // Moves a simple outline by dx; its points, flags and bounds are encoded again.
    static byte[] translate(byte[] outline, int dx) {
        if (dx == 0) {
            return outline;
        }

        ByteBuffer glyph = ByteBuffer.wrap(outline);
        int contours = glyph.getShort(0);
        FontCyrillicFix.require(contours >= 0, "Composite Russian letter outline");
        int prefixLength = 12 + contours * 2 + (glyph.getShort(10 + contours * 2) & 0xffff);
        Points points = points(outline, contours, prefixLength);
        byte[] prefix = Arrays.copyOf(outline, prefixLength);
        ByteBuffer.wrap(prefix).putShort(2, (short) (glyph.getShort(2) + dx));
        ByteBuffer.wrap(prefix).putShort(6, (short) (glyph.getShort(6) + dx));
        ByteArrayOutputStream result = new ByteArrayOutputStream(outline.length + 8);
        result.writeBytes(prefix);
        encode(points, dx, result);
        result.write(outline, points.yStart(), points.yEnd() - points.yStart());

        return result.toByteArray();
    }

    private static Points points(byte[] outline, int contours, int cursor) {
        ByteBuffer glyph = ByteBuffer.wrap(outline);
        int count = contours == 0 ? 0 : (glyph.getShort(10 + (contours - 1) * 2) & 0xffff) + 1;
        int[] flags = new int[count];

        for (int i = 0; i < count; ) {
            int flag = outline[cursor++] & 0xff;
            int repeat = 1 + ((flag & REPEAT) != 0 ? outline[cursor++] & 0xff : 0);

            for (int j = 0; j < repeat && i < count; j++) {
                flags[i++] = flag;
            }
        }

        int[] xs = new int[count];
        int x = 0;

        for (int i = 0; i < count; i++) {
            if ((flags[i] & X_SHORT) != 0) {
                int delta = outline[cursor++] & 0xff;
                x += (flags[i] & X_SAME) != 0 ? delta : -delta;
            } else if ((flags[i] & X_SAME) == 0) {
                x += glyph.getShort(cursor);
                cursor += 2;
            }

            xs[i] = x;
        }

        int yStart = cursor;

        for (int flag : flags) {
            cursor += (flag & Y_SHORT) != 0 ? 1 : (flag & Y_SAME) != 0 ? 0 : 2;
        }

        return new Points(flags, xs, yStart, cursor);
    }

    // Writes the flags, run-length encoded, and the x deltas of the points moved by dx.
    private static void encode(Points points, int dx, ByteArrayOutputStream result) {
        List<Integer> flags = new ArrayList<>();
        ByteArrayOutputStream xs = new ByteArrayOutputStream();
        int previous = 0;

        for (int i = 0; i < points.xs().length; i++) {
            int x = points.xs()[i] + dx;
            int delta = x - previous;
            int flag = points.flags()[i] & (ON_CURVE | Y_SHORT | Y_SAME | OVERLAP);
            previous = x;

            if (delta == 0) {
                flag |= X_SAME;
            } else if (Math.abs(delta) <= 255) {
                flag |= X_SHORT | (delta > 0 ? X_SAME : 0);
                xs.write(Math.abs(delta));
            } else {
                xs.write(delta >> 8);
                xs.write(delta);
            }

            flags.add(flag);
        }

        for (int i = 0; i < flags.size(); ) {
            int run = 1;

            while (i + run < flags.size() && flags.get(i + run).equals(flags.get(i)) && run < 256) {
                run++;
            }

            result.write(flags.get(i) | (run > 1 ? REPEAT : 0));

            if (run > 1) {
                result.write(run - 1);
            }

            i += run;
        }

        result.writeBytes(xs.toByteArray());
    }

    // The font with the given tables replaced, laid out again in their order with new checksums.
    private static byte[] rebuild(
            byte[] source,
            Map<String, FontCyrillicAdvanceFix.Table> tables,
            Map<String, byte[]> replaced) {
        List<Map.Entry<String, FontCyrillicAdvanceFix.Table>> order =
                new ArrayList<>(tables.entrySet());

        order.sort(Comparator.comparingInt(entry -> entry.getValue().offset()));
        int directory = 12 + tables.size() * 16;
        ByteArrayOutputStream body = new ByteArrayOutputStream(source.length);
        byte[] header = new byte[directory];
        System.arraycopy(source, 0, header, 0, directory);
        ByteBuffer records = ByteBuffer.wrap(header);
        int headOffset = 0;

        for (var entry : order) {
            FontCyrillicAdvanceFix.Table table = entry.getValue();
            byte[] data = replaced.get(entry.getKey());

            if (data == null) {
                data = new byte[table.length()];
                System.arraycopy(source, table.offset(), data, 0, table.length());
            }

            int offset = directory + body.size();

            if (entry.getKey().equals("head")) {
                headOffset = offset;
                ByteBuffer.wrap(data).putInt(8, 0);
            }

            records.putInt(
                    table.recordOffset() + 4,
                    (int) FontCyrillicAdvanceFix.checksum(data, 0, data.length));
            records.putInt(table.recordOffset() + 8, offset);
            records.putInt(table.recordOffset() + 12, data.length);
            body.writeBytes(data);

            while (body.size() % 4 != 0) {
                body.write(0);
            }
        }

        byte[] result = new byte[directory + body.size()];
        System.arraycopy(header, 0, result, 0, directory);
        System.arraycopy(body.toByteArray(), 0, result, directory, body.size());
        ByteBuffer.wrap(result)
                .putInt(
                        headOffset + 8,
                        (int)
                                ((CHECKSUM_MAGIC
                                                - FontCyrillicAdvanceFix.checksum(
                                                        result, 0, result.length))
                                        & 0xffffffffL));

        return result;
    }
}
