package com.voyemu.prepare;

import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Generates the Passion vehicle field catalogue from the pinned OEM SOME/IP service table
 * /etc/exd/adapter_config/signaldefine/S32G_8155/8155_qnx/H53_SOMEIP_ServiceTable_V6.0.0_OIB_8155_QNX_20240103-142433.json
 * of system_la.img of the Passion profile: one field per signal of the Notify, NotifyCycle and
 * NotifyOnChg events, the inputs of the cluster, in table order. The name is the signal path
 * without the 8155QNX prefix, the width is the byte length of the type in bits and the default is
 * the initial value of the table, else 0.
 *
 * <p>Usage: java -cp CLASSES com.voyemu.prepare.GeneratePassionCatalog TABLE OUTPUT, with CLASSES
 * compiled from src/prepare/java/com/voyemu/prepare/Json.java and this file.
 */
public final class GeneratePassionCatalog {
    static final String TABLE_SHA256 =
            "8018e59b0457f653e0d4925bef95ce7483ad276b70b261e3d38c0f1d91c5cbb4";
    static final int FIELDS = 1598;

    private GeneratePassionCatalog() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            throw new IllegalArgumentException("Expected TABLE OUTPUT");
        }

        byte[] table = Files.readAllBytes(Path.of(args[0]));
        String digest =
                HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(table));
        require(digest.equals(TABLE_SHA256), "Table differs from its pin: " + digest);
        // The physical limits of the 64-bit signals reach 2^64 - 1, beyond the long of Json.
        String text =
                new String(table, StandardCharsets.UTF_8).replaceAll(": (\\d{19,})", ": \"$1\"");
        Files.writeString(Path.of(args[1]), generate(Json.object(Json.parse(text))));
    }

    static String generate(Map<String, Object> table) {
        require(
                Json.str(table, "signalType").equals("8155QNXSignal")
                        && Json.str(table, "version").equals("1.0.0"),
                "Table signal type");
        var options = new LinkedHashMap<String, Integer>();
        var optionTables = new StringBuilder();
        var rows = new StringBuilder();
        var names = new HashSet<String>();
        int count = 0;

        for (Object serviceValue : Json.list(table.get("servicedefine"))) {
            var service = Json.object(serviceValue);
            String serviceName = Json.str(service, "ServiceName");

            for (Object eventValue : Json.list(service.get("method_event_define"))) {
                var event = Json.object(eventValue);
                String eventName = Json.str(event, "Method_Event_Name");

                if (!eventName.split("_")[0].toLowerCase(Locale.ROOT).startsWith("notify")) {
                    continue;
                }

                for (Object signalValue : Json.list(event.get("Signals"))) {
                    var signal = Json.object(signalValue);
                    String prefix = "8155QNX." + serviceName + "." + eventName + ".";
                    String fullName = Json.str(signal, "name");
                    require(fullName.startsWith(prefix), "Signal path " + fullName);
                    String name = fullName.substring("8155QNX.".length());
                    require(names.add(name), "Duplicate signal " + name);
                    String type = Json.str(signal, "type");
                    long bytes = Json.integer(signal, "len");
                    require(
                            switch (type) {
                                case "uint8", "int8", "boolean" -> bytes == 1;
                                case "uint16", "int16" -> bytes == 2;
                                case "uint32", "int32", "float", "uint32_stub" -> bytes == 4;
                                case "uint64" -> bytes == 8;
                                default -> false;
                            },
                            "Signal type " + name);
                    int width = (int) bytes * 8;
                    var physical = Json.obj(signal, "PhysicalConvert");
                    Long initial = raw(physical, "Initial_Value", width);
                    require(
                            initial == null || initial <= 0xffffffffL,
                            "Initial value wider than 32 bits " + name);
                    String optionTable = "{}";
                    var discrete = Json.obj(signal, "Discrete_Value_Defination");

                    if (Boolean.TRUE.equals(discrete.get("valid"))) {
                        String key = optionsText(Json.obj(discrete, "options"));
                        Integer index = options.get(key);

                        if (index == null) {
                            index = options.size();
                            options.put(key, index);
                            optionTables
                                    .append("static constexpr SignalOption passion_options_")
                                    .append(index)
                                    .append("[] = {")
                                    .append(key)
                                    .append("};\n");
                        }

                        optionTable = "passion_options_" + index;
                    }

                    count++;
                    rows.append("        {")
                            .append(quote(name))
                            .append(", ")
                            .append(width)
                            .append(", ")
                            .append(initial == null ? 0 : initial)
                            .append(", {")
                            .append(Json.integer(signal, "SignalID"))
                            .append(", ")
                            .append(quote(type))
                            .append(", ")
                            .append(number(physical, "Resolution"))
                            .append(", ")
                            .append(number(physical, "Offset"))
                            .append(", ")
                            .append(number(physical, "Physical_Min"))
                            .append(", ")
                            .append(number(physical, "Physical_Max"))
                            .append(", ")
                            .append(invalid(physical, width))
                            .append(", ")
                            .append(quote(unit(physical)))
                            .append(", ")
                            .append(optionTable)
                            .append("}},\n");
                }
            }
        }

        require(count == FIELDS, "Cluster input signal count " + count);

        return "// Generated by GeneratePassionCatalog.java from the Passion SOME/IP service table"
                + " with SHA-256\n// "
                + TABLE_SHA256
                + "; do not edit.\n"
                + optionTables
                + "static constexpr PassionSignalRow passion_rows[] = {\n"
                + rows
                + "};\n";
    }

    // A raw value of the table: a nonnegative integer that the width holds, or null when the table
    // declares none.
    private static Long raw(Map<String, Object> physical, String key, int width) {
        var item = Json.obj(physical, key);

        if (!Boolean.TRUE.equals(item.get("valid"))) {
            return null;
        }

        long value = Json.integer(item, "value");
        require(value >= 0 && (width == 64 || value < 1L << width), "Raw value range " + key);

        return value;
    }

    // ic_oilPercent declares the invalid value 65535 for one byte; a value the width cannot hold
    // never reaches the wire and stays undeclared.
    private static String invalid(Map<String, Object> physical, int width) {
        var item = Json.obj(physical, "Invalid_Value");

        if (!Boolean.TRUE.equals(item.get("valid"))) {
            return "{}";
        }

        long value = Json.integer(item, "value");
        require(value >= 0, "Invalid value sign");

        return width == 64 || value < 1L << width ? value + "U" : "{}";
    }

    private static String number(Map<String, Object> physical, String key) {
        var item = Json.obj(physical, key);

        if (!Boolean.TRUE.equals(item.get("valid"))) {
            return "{}";
        }

        Object value = item.get("value");

        if (value instanceof Long || value instanceof String) {
            return value + ".0";
        }

        require(value instanceof BigDecimal, "Number " + key);
        String text = ((BigDecimal) value).toPlainString();

        return text.contains(".") ? text : text + ".0";
    }

    private static String unit(Map<String, Object> physical) {
        var item = Json.obj(physical, "Unit");

        return Boolean.TRUE.equals(item.get("valid")) ? Json.str(item, "value") : "";
    }

    private static String optionsText(Map<String, Object> options) {
        require(!options.isEmpty(), "Empty enumeration");
        var entries = new ArrayList<String>();

        for (var entry : options.entrySet()) {
            String key = entry.getKey();
            require(key.startsWith("0x") && key.length() > 2, "Enumeration value " + key);
            entries.add(
                    "{"
                            + Long.parseUnsignedLong(key.substring(2), 16)
                            + "U, "
                            + quote((String) entry.getValue())
                            + "}");
        }

        return String.join(", ", entries);
    }

    private static String quote(String text) {
        var output = new StringBuilder("\"");

        for (char c : text.toCharArray()) {
            require(c >= 0x20 && c < 0x7f, "Non-ASCII text " + text);

            if (c == '"' || c == '\\') {
                output.append('\\');
            }

            output.append(c);
        }

        return output.append('"').toString();
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
