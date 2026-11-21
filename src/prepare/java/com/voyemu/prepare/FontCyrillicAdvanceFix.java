package com.voyemu.prepare;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Structural SFNT repair of the full-em advances of the Russian letters of a TrueType font. */
final class FontCyrillicAdvanceFix {
    static final int MAX_FONT_BYTES = 16 * 1024 * 1024;
    static final int CORE_RUSSIAN_LETTERS = 66;
    private static final long CHECKSUM_MAGIC = 0xb1b0afbaL;

    private FontCyrillicAdvanceFix() {}

    /** The repaired copy of source and the count of repaired glyphs; source stays unchanged. */
    record Result(byte[] bytes, int glyphsPatched, String reason) {
        boolean changed() {
            return glyphsPatched > 0;
        }

        static Result unchanged(byte[] bytes, String reason) {
            return new Result(bytes, 0, reason);
        }
    }

    record Table(int recordOffset, int offset, int length) {}

    private record GlyphMetric(int offset, int advance, int inkWidth) {}

    private record Layout(
            Table head,
            Table hmtx,
            Table cmap,
            Table loca,
            Table glyf,
            int unitsPerEm,
            int metricCount,
            int locaFormat) {}

    static Result patch(byte[] source) {
        if (source == null || source.length < 12 || source.length > MAX_FONT_BYTES) {
            return Result.unchanged(source, "font-size");
        }

        try {
            byte[] output = source.clone();

            if (u32(output, 0) != 0x00010000L) {
                return Result.unchanged(source, "sfnt-version");
            }

            Layout layout = layout(output);

            if (layout == null) {
                return Result.unchanged(source, "sfnt-metrics");
            }

            Set<Integer> glyphs = coreRussianGlyphs(output, layout.cmap());

            if (glyphs.size() != CORE_RUSSIAN_LETTERS) {
                return Result.unchanged(source, "cyrillic-coverage");
            }

            List<GlyphMetric> metrics = metrics(output, layout, glyphs);

            if (metrics == null) {
                return Result.unchanged(source, "cyrillic-structure");
            }

            long defective =
                    metrics.stream().filter(m -> isDefective(m, layout.unitsPerEm())).count();

            // The defect affects 63 of the 66 core Russian letters; requiring 90 percent keeps the
            // repair from normalizing other fonts.
            if (defective * 10 < CORE_RUSSIAN_LETTERS * 9) {
                return Result.unchanged(
                        source, defective == 0 ? "already-correct" : "unknown-metrics");
            }

            int patched = repair(output, metrics, layout.unitsPerEm());

            if (patched == 0) {
                return Result.unchanged(source, "already-correct");
            }

            return restoreChecksums(output, layout)
                    ? new Result(output, patched, "patched")
                    : Result.unchanged(source, "checksum");
        } catch (IllegalArgumentException malformed) {
            return Result.unchanged(source, "malformed-sfnt");
        }
    }

    // The metric of each Russian letter, or null when a glyph lacks its outline or metric.
    private static List<GlyphMetric> metrics(byte[] font, Layout layout, Set<Integer> glyphs) {
        List<GlyphMetric> metrics = new ArrayList<>();

        for (int glyph : glyphs) {
            GlyphMetric metric = metric(font, layout, glyph);

            if (metric == null) {
                return null;
            }

            metrics.add(metric);
        }

        return metrics;
    }

    // The tables and metric counts that the repair reads, or null when they disagree.
    private static Layout layout(byte[] font) {
        Map<String, Table> tables = tables(font);
        Table head = required(tables, "head", 54);
        Table hhea = required(tables, "hhea", 36);
        Table maxp = required(tables, "maxp", 6);
        Table hmtx = required(tables, "hmtx", 4);
        Table loca = required(tables, "loca", 4);
        int unitsPerEm = u16(font, head.offset() + 18);
        int glyphCount = u16(font, maxp.offset() + 4);
        int metricCount = u16(font, hhea.offset() + 34);
        int locaFormat = i16(font, head.offset() + 50);
        boolean valid =
                unitsPerEm >= 16
                        && unitsPerEm <= 16384
                        && glyphCount > 0
                        && metricCount > 0
                        && metricCount <= glyphCount
                        && hmtx.length() >= metricCount * 4L + (glyphCount - metricCount) * 2L
                        && (locaFormat == 0 || locaFormat == 1)
                        && loca.length() >= (glyphCount + 1L) * (locaFormat == 0 ? 2L : 4L);

        return valid
                ? new Layout(
                        head,
                        hmtx,
                        required(tables, "cmap", 4),
                        loca,
                        required(tables, "glyf", 1),
                        unitsPerEm,
                        metricCount,
                        locaFormat)
                : null;
    }

    // Narrows each defective advance to the ink width with a side bearing of 5 percent of the em.
    private static int repair(byte[] font, List<GlyphMetric> metrics, int unitsPerEm) {
        int sideBearing = Math.max(1, Math.round(unitsPerEm * 0.05f));
        int patched = 0;

        for (GlyphMetric metric : metrics) {
            int corrected = Math.min(0xffff, metric.inkWidth() + sideBearing * 2);

            if (isDefective(metric, unitsPerEm) && corrected < metric.advance()) {
                putU16(font, metric.offset(), corrected);
                putU16(font, metric.offset() + 2, sideBearing);
                patched++;
            }
        }

        return patched;
    }

    private static boolean restoreChecksums(byte[] font, Layout layout) {
        Table head = layout.head();
        Table hmtx = layout.hmtx();

        putU32(font, head.offset() + 8, 0);
        putU32(font, hmtx.recordOffset() + 4, checksum(font, hmtx.offset(), hmtx.length()));
        putU32(font, head.recordOffset() + 4, checksum(font, head.offset(), head.length()));
        putU32(
                font,
                head.offset() + 8,
                (CHECKSUM_MAGIC - checksum(font, 0, font.length)) & 0xffffffffL);

        return checksum(font, 0, font.length) == CHECKSUM_MAGIC;
    }

    private static boolean isDefective(GlyphMetric metric, int em) {
        return metric != null
                && metric.advance() >= Math.round(em * 0.90f)
                && metric.inkWidth() > 0
                && metric.inkWidth() <= Math.round(em * 0.85f);
    }

    private static GlyphMetric metric(byte[] font, Layout layout, int glyph) {
        if (glyph <= 0 || glyph >= layout.metricCount()) {
            return null;
        }

        int start = glyphOffset(font, layout.loca(), layout.locaFormat(), glyph);
        int end = glyphOffset(font, layout.loca(), layout.locaFormat(), glyph + 1);

        if (start < 0 || end < start || end > layout.glyf().length() || end - start < 10) {
            return null;
        }

        int glyphOffset = layout.glyf().offset() + start;
        int width = i16(font, glyphOffset + 6) - i16(font, glyphOffset + 2);
        int metricOffset = layout.hmtx().offset() + glyph * 4;

        return new GlyphMetric(metricOffset, u16(font, metricOffset), width);
    }

    private static Map<String, Table> tables(byte[] font) {
        int count = u16(font, 4);

        if (count <= 0 || count > 128 || 12L + count * 16L > font.length) {
            throw new IllegalArgumentException("table directory");
        }

        Map<String, Table> result = new HashMap<>();

        for (int i = 0; i < count; i++) {
            int entry = 12 + i * 16;
            String tag =
                    new String(
                            new char[] {
                                (char) u8(font, entry),
                                (char) u8(font, entry + 1),
                                (char) u8(font, entry + 2),
                                (char) u8(font, entry + 3)
                            });
            long offset = u32(font, entry + 8);
            long length = u32(font, entry + 12);

            if ((offset & 3) != 0
                    || offset > font.length
                    || length > font.length - offset
                    || result.containsKey(tag)) {
                throw new IllegalArgumentException("table record");
            }

            result.put(tag, new Table(entry, (int) offset, (int) length));
        }

        Table[] entries = result.values().toArray(new Table[0]);

        for (int left = 0; left < entries.length; left++) {
            for (int right = left + 1; right < entries.length; right++) {
                long leftEnd = entries[left].offset() + (long) entries[left].length();
                long rightEnd = entries[right].offset() + (long) entries[right].length();

                if (entries[left].length() > 0
                        && entries[right].length() > 0
                        && entries[left].offset() < rightEnd
                        && entries[right].offset() < leftEnd) {
                    throw new IllegalArgumentException("overlapping tables");
                }
            }
        }

        return result;
    }

    private static Table required(Map<String, Table> tables, String tag, int length) {
        Table table = tables.get(tag);

        if (table == null || table.length() < length) {
            throw new IllegalArgumentException("table " + tag);
        }

        return table;
    }

    private static Set<Integer> coreRussianGlyphs(byte[] font, Table cmap) {
        Set<Integer> glyphs = new LinkedHashSet<>();

        for (int cp = 0x0410; cp <= 0x044f; cp++) {
            addMapped(font, cmap, cp, glyphs);
        }

        addMapped(font, cmap, 0x0401, glyphs);
        addMapped(font, cmap, 0x0451, glyphs);

        return glyphs;
    }

    private static void addMapped(byte[] font, Table cmap, int codePoint, Set<Integer> glyphs) {
        Integer glyph = mappedGlyph(font, cmap, codePoint);

        if (glyph != null && glyph != 0) {
            glyphs.add(glyph);
        }
    }

    static Integer mappedGlyph(byte[] font, Table cmap, int codePoint) {
        int count = u16(font, cmap.offset() + 2);

        if (cmap.offset() + 4L + count * 8L > cmap.offset() + (long) cmap.length()) {
            return null;
        }

        for (int i = 0; i < count; i++) {
            int entry = cmap.offset() + 4 + i * 8;
            long relative = u32(font, entry + 4);

            if (relative > cmap.length() - 2L) {
                continue;
            }

            int subtable = cmap.offset() + (int) relative;
            int format = u16(font, subtable);
            Integer glyph =
                    format == 4
                            ? format4(font, cmap, subtable, codePoint)
                            : format == 12 ? format12(font, cmap, subtable, codePoint) : null;

            if (glyph != null && glyph != 0) {
                return glyph;
            }
        }

        return null;
    }

    private static Integer format4(byte[] font, Table cmap, int subtable, int cp) {
        int length = u16(font, subtable + 2);
        int end = subtable + length;

        if (length < 16 || end > cmap.offset() + cmap.length()) {
            return null;
        }

        int segments = u16(font, subtable + 6) / 2;
        int ends = subtable + 14;
        int starts = ends + segments * 2 + 2;
        int deltas = starts + segments * 2;
        int ranges = deltas + segments * 2;

        if (segments <= 0 || ranges + segments * 2 > end) {
            return null;
        }

        for (int i = 0; i < segments; i++) {
            int start = u16(font, starts + i * 2);
            int finish = u16(font, ends + i * 2);

            if (cp < start || cp > finish) {
                continue;
            }

            int range = u16(font, ranges + i * 2);

            if (range == 0) {
                return (cp + i16(font, deltas + i * 2)) & 0xffff;
            }

            int address = ranges + i * 2 + range + (cp - start) * 2;

            if (address + 2 > end) {
                return null;
            }

            int glyph = u16(font, address);

            return glyph == 0 ? 0 : (glyph + i16(font, deltas + i * 2)) & 0xffff;
        }

        return null;
    }

    private static Integer format12(byte[] font, Table cmap, int subtable, int cp) {
        long length = u32(font, subtable + 4);
        long groups = u32(font, subtable + 12);
        long end = subtable + length;

        if (length < 16
                || end > cmap.offset() + (long) cmap.length()
                || groups > (length - 16) / 12) {
            return null;
        }

        for (int i = 0; i < groups; i++) {
            int group = subtable + 16 + i * 12;
            long start = u32(font, group);
            long finish = u32(font, group + 4);

            if (cp >= start && cp <= finish) {
                long glyph = u32(font, group + 8) + cp - start;

                return glyph <= 0xffff ? (int) glyph : null;
            }
        }

        return null;
    }

    private static int glyphOffset(byte[] font, Table loca, int format, int glyph) {
        int width = format == 0 ? 2 : 4;
        int address = loca.offset() + glyph * width;

        if (address < loca.offset() || address + width > loca.offset() + loca.length()) {
            return -1;
        }

        long value = format == 0 ? u16(font, address) * 2L : u32(font, address);

        return value <= Integer.MAX_VALUE ? (int) value : -1;
    }

    static long checksum(byte[] bytes, int offset, int length) {
        long sum = 0;
        int padded = (length + 3) & ~3;

        for (int cursor = 0; cursor < padded; cursor += 4) {
            long word = 0;

            for (int i = 0; i < 4; i++) {
                word = (word << 8) | (cursor + i < length ? u8(bytes, offset + cursor + i) : 0);
            }

            sum = (sum + word) & 0xffffffffL;
        }

        return sum;
    }

    private static int u8(byte[] bytes, int offset) {
        if (offset < 0 || offset >= bytes.length) {
            throw new IllegalArgumentException("offset");
        }

        return bytes[offset] & 0xff;
    }

    private static int u16(byte[] bytes, int offset) {
        return (u8(bytes, offset) << 8) | u8(bytes, offset + 1);
    }

    private static int i16(byte[] bytes, int offset) {
        return (short) u16(bytes, offset);
    }

    private static long u32(byte[] bytes, int offset) {
        return ((long) u16(bytes, offset) << 16) | u16(bytes, offset + 2);
    }

    private static void putU16(byte[] bytes, int offset, int value) {
        bytes[offset] = (byte) (value >>> 8);
        bytes[offset + 1] = (byte) value;
    }

    private static void putU32(byte[] bytes, int offset, long value) {
        putU16(bytes, offset, (int) (value >>> 16));
        putU16(bytes, offset + 2, (int) value);
    }
}
