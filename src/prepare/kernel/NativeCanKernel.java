package com.voyemu.prepare;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Pattern;

final class NativeCanKernel {
    private static final Pattern KMI_LIST =
            Pattern.compile("android/abi_gki_aarch64(?:_[A-Za-z0-9]+)?");
    private static final Pattern KCONFIG =
            Pattern.compile("^(?:(CONFIG_[A-Z0-9_]+)=(.*)|# (CONFIG_[A-Z0-9_]+) is not set)$");
    private static final Pattern WHITELIST =
            Pattern.compile("(?m)^CONFIG_UNUSED_KSYMS_WHITELIST=.*$");
    private static final Pattern RELOCATION = Pattern.compile("R_AARCH64_[A-Z0-9_]+");
    private static final List<String> API30_REQUIRED =
            List.of(
                    "LTO_CLANG",
                    "CFI_CLANG",
                    "MODVERSIONS",
                    "TRIM_UNUSED_KSYMS",
                    "COMPAT_VDSO",
                    "GENERIC_COMPAT_VDSO",
                    "RELR",
                    "TOOLS_SUPPORT_RELR",
                    "SECURITY_SELINUX");

    private final Path recipe;
    private final Map<String, Object> component;
    private final Map<String, Object> inputs;
    private final boolean api30;

    /**
     * A variant recipe names a base recipe of a profile group and adds its own patches and
     * configuration; null for a base recipe.
     */
    private final Path variantRecipe;

    private final Map<String, Object> variant;

    private NativeCanKernel(Path directory) throws IOException {
        var declared = read(directory.resolve("component.json"));

        if ("development-kernel-variant".equals(declared.get("kind"))) {
            String base = Json.str(declared, "base");
            require(base.matches("common-[a-z0-9-]+"), "Invalid base recipe " + base);
            variantRecipe = directory;
            variant = declared;
            recipe = directory.resolve("../../..").normalize().resolve(base + "/components/kernel");
            component = read(recipe.resolve("component.json"));
            require(
                    Json.integer(component, "android_api") == 30,
                    "Variant recipes require an API 30 base");
        } else {
            variantRecipe = null;
            variant = null;
            recipe = directory;
            component = declared;
        }

        long api = Json.integer(component, "android_api");
        require(api == 28 || api == 30, "Unsupported CAN kernel API");
        api30 = api == 30;
        inputs = api30 ? Json.obj(component, "inputs") : Json.obj(component, "source");
    }

    public static void main(String[] args) throws Exception {
        try {
            run(args);
        } catch (Failure e) {
            System.err.println(e.code + ": " + e.getMessage());
            System.exit(e.exitCode);
        }
    }

    private static void run(String[] args) throws Exception {
        require(args.length >= 2, "Expected RECIPE COMMAND [ARGUMENTS]");
        NativeCanKernel tool = new NativeCanKernel(path(args[0]));

        switch (args[1]) {
            case "prepare" -> {
                require(
                        args.length == (tool.api30 ? 5 : 4),
                        "Expected PROJECT WORKSPACE and API 30 STOCK_ANDROID_BASE");
                tool.prepare(path(args[2]), path(args[3]), tool.api30 ? path(args[4]) : null);
            }
            case "build" -> {
                require(args.length == 5, "Expected PROJECT WORKSPACE JOBS");
                require(args[4].matches("[1-4]"), "Build budget is 1-4 jobs");
                tool.build(path(args[2]), path(args[3]), Integer.parseInt(args[4]));
            }
            case "publish" -> {
                require(args.length == 4, "Expected PROJECT WORKSPACE");
                tool.publish(path(args[2]), path(args[3]));
            }
            default -> throw new Failure("NATIVE_CAN_KERNEL", "Unknown command " + args[1], 2);
        }
    }

    private static Path path(String value) {
        return Path.of(value).toAbsolutePath().normalize();
    }

    private static Map<String, Object> read(Path file) throws IOException {
        return Json.object(Json.parse(Files.readString(file)));
    }

    private static void write(Path file, Map<String, ?> data) throws IOException {
        Storage.atomic(file, Json.write(data) + "\n");
    }

    private static void require(boolean condition, String message) {
        Failure.require(condition, "NATIVE_CAN_KERNEL", message, 3);
    }

    private static void pin(Path file, Object expected, String message) throws Exception {
        require(FilesSupport.hash(file).equals(expected), message + ": " + file);
    }

    private List<String> names(String key) {
        var names = new ArrayList<String>();

        for (Object name : Json.list(component.get(key))) {
            names.add((String) name);
        }

        return names;
    }

    /** Patches in application order: the base recipe first, then the variant. */
    private List<Path> patches() {
        var files = new ArrayList<Path>();

        for (String name : names("patches")) {
            files.add(Storage.confined(recipe, name));
        }

        if (variant != null) {
            for (Object name : Json.list(variant.get("patches"))) {
                files.add(Storage.confined(variantRecipe, (String) name));
            }
        }

        return files;
    }

    private List<String> drivers() {
        return names("driver");
    }

    private String releaseName() {
        return Json.str(variant == null ? component : variant, "release_name");
    }

    /** Kconfig values the variant sets over the stock configuration, as y, m or n. */
    private Map<String, String> variantConfig() {
        var values = new LinkedHashMap<String, String>();

        if (variant != null) {
            for (var entry : Json.obj(variant, "config").entrySet()) {
                String value = (String) entry.getValue();
                require(
                        entry.getKey().matches("CONFIG_[A-Z0-9_]+") && value.matches("[ymn]"),
                        "Invalid variant configuration " + entry.getKey());
                values.put(entry.getKey(), value);
            }
        }

        return values;
    }

    /** The stock configuration with the variant values replacing their stock lines. */
    private String configuration(String stock) {
        if (variant == null) {
            return stock;
        }

        var config = variantConfig();
        var lines = new ArrayList<String>();

        for (String line : stock.lines().toList()) {
            var entry = KCONFIG.matcher(line);

            if (!entry.matches()
                    || !config.containsKey(
                            entry.group(1) == null ? entry.group(3) : entry.group(1))) {
                lines.add(line);
            }
        }

        config.forEach((key, value) ->
                lines.add(value.equals("n") ? "# " + key + " is not set" : key + "=" + value));

        return String.join("\n", lines) + "\n";
    }

    /**
     * Hashes of the recipe files computed at prepare; later steps reject a recipe edited since. Own
     * files carry no pins; the published Image pin covers them.
     */
    private Map<String, Object> recipeHashes() throws Exception {
        var hashes = new LinkedHashMap<String, Object>();
        var files = new ArrayList<>(names("patches"));

        files.addAll(drivers());
        files.add("stock-embedded.config");

        for (String name : files) {
            hashes.put(name, FilesSupport.hash(Storage.confined(recipe, name)));
        }

        if (variant != null) {
            hashes.put(
                    "variant/component.json",
                    FilesSupport.hash(variantRecipe.resolve("component.json")));

            for (Object name : Json.list(variant.get("patches"))) {
                hashes.put(
                        "variant/" + name,
                        FilesSupport.hash(Storage.confined(variantRecipe, (String) name)));
            }
        }

        return hashes;
    }

    private void checkFrozen() {
        if (!api30) {
            require(
                    Boolean.FALSE.equals(component.get("freeze_pending")),
                    "Driver and helper source freeze pending");
        }
    }

    private void prepare(Path project, Path work, Path base) throws Exception {
        checkFrozen();
        Path config = recipe.resolve("stock-embedded.config");

        if (api30) {
            pin(
                    base.resolve("kernel-ranchu"),
                    Json.str(inputs, "stock_kernel_sha256"),
                    "Stock kernel differs");
            pin(
                    base.resolve("ramdisk.img"),
                    Json.str(inputs, "stock_ramdisk_sha256"),
                    "Stock ramdisk differs");
        }

        Files.createDirectories(work);
        Path probe = work.resolve(".voyemu-case-probe");
        Path alias = work.resolve(".VOYEMU-CASE-PROBE");
        require(Files.notExists(alias, LinkOption.NOFOLLOW_LINKS), "Existing case probe");
        Files.writeString(probe, "case", StandardOpenOption.CREATE_NEW);

        try {
            require(
                    Files.notExists(alias, LinkOption.NOFOLLOW_LINKS),
                    "Linux source requires a case-sensitive filesystem");
        } finally {
            Files.delete(probe);
        }

        Path source = work.resolve("common");
        Path output = work.resolve("out");
        require(
                Files.notExists(source, LinkOption.NOFOLLOW_LINKS)
                        && Files.notExists(output, LinkOption.NOFOLLOW_LINKS),
                "Fresh source and output directories required");
        Files.createDirectory(source);
        KernelSource.extract(project, inputs, source);

        if (api30) {
            // Kbuild 5.4 passes relative source paths only when the output directory sits
            // directly in the source tree. ThinLTO names promoted locals by a module hash, so
            // absolute source paths would put the workspace path into kallsyms.
            Files.createDirectory(source.resolve("out"));
            Files.createSymbolicLink(output, Path.of("common/out"));
        } else {
            Files.createDirectory(output);
        }

        // API 30 patches have a fixed order. API 28 uses its manifest order.
        for (Path file : patches()) {
            KernelSource.patch(source, file);
        }

        Path misc = source.resolve("drivers/misc");

        for (String name : drivers()) {
            Files.copy(recipe.resolve(name), misc.resolve(name));
        }

        Files.writeString(
                misc.resolve("Makefile"),
                "\n# Voyemu development CAN transport; no physical ECU.\nobj-y += cis_can_dev.o\n",
                StandardOpenOption.APPEND);
        Files.writeString(source.resolve(".scmversion"), releaseSuffix() + "\n");

        String elfOverride = System.getenv("VOYEMU_ELF_HEADER");
        Path elf =
                elfOverride == null
                        ? project.resolve(
                                ".local/toolchains/zig-0.15.2/lib/libc/include/generic-musl/elf.h")
                        : path(elfOverride);
        require(Files.isRegularFile(elf), "Host ELF declarations missing");
        Files.createDirectories(work.resolve("host-include"));
        Files.createSymbolicLink(work.resolve("host-include/elf.h"), elf);

        Map<String, Object> receipt = new LinkedHashMap<>();
        receipt.put("schema", 1);
        receipt.put("component_sha256", FilesSupport.hash(recipe.resolve("component.json")));
        receipt.put("host_elf_sha256", FilesSupport.hash(elf.toRealPath()));

        if (api30) {
            var lists = new LinkedHashSet<String>();
            KMI_LIST.matcher(Files.readString(source.resolve("build.config.gki.aarch64")))
                    .results()
                    .forEach(match -> lists.add(match.group()));
            var symbols = new TreeSet<String>();

            for (String name : lists) {
                for (String line : Files.readAllLines(source.resolve(name))) {
                    String symbol = line.strip();

                    if (symbol.isEmpty() || symbol.startsWith("#") || symbol.startsWith("[")) {
                        continue;
                    }

                    require(
                            symbol.matches("[A-Za-z_][A-Za-z0-9_]*"),
                            "Invalid KMI symbol " + symbol);
                    symbols.add(symbol);
                }
            }

            // A relative whitelist path is resolved in the source tree and keeps the embedded
            // configuration free of workspace paths.
            Path whitelist = source.resolve("abi_symbollist.raw");
            Files.writeString(whitelist, String.join("\n", symbols) + "\n");
            Files.writeString(
                    output.resolve(".config"),
                    whitelist(configuration(Files.readString(config)), "abi_symbollist.raw"));
            KernelStockModules.inventory(
                    base.resolve("ramdisk.img"), work.resolve("stock-modules.json"));
            receipt.put("recipe", recipeHashes());
            receipt.put("kernel_revision", Json.str(inputs, "commit"));
            receipt.put("kmi_lists", lists);
            receipt.put("kmi_symbols", symbols.size());
            receipt.put("kmi_whitelist_sha256", FilesSupport.hash(whitelist));
            write(work.resolve("source-inputs.json"), receipt);
            verifySource(work);
            System.out.println(
                    "Prepared "
                            + Json.str(inputs, "commit")
                            + ", "
                            + symbols.size()
                            + " KMI symbols, stock vendor modules retained");
        } else {
            Files.copy(config, output.resolve(".config"));
            receipt.put("source_revision", Json.str(inputs, "commit"));
            receipt.put("recipe", recipeHashes());
            write(work.resolve("source-inputs.json"), receipt);
            System.out.println(
                    "Prepared pinned Linux4.4 goldfish source and development transport");
        }
    }

    private String releaseSuffix() {
        return api30 ? Json.str(inputs, "kernel_release").replaceFirst("^5\\.4\\.86", "") : "";
    }

    private void verifySource(Path work) throws Exception {
        checkFrozen();
        var hashes = recipeHashes();
        require(
                hashes.equals(Json.obj(read(work.resolve("source-inputs.json")), "recipe")),
                "Kernel recipe changed since prepare");

        for (String name : drivers()) {
            require(
                    FilesSupport.hash(work.resolve("common/drivers/misc").resolve(name))
                            .equals(hashes.get(name)),
                    "Driver source differs: " + name);
        }

        if (api30) {
            require(
                    Files.readString(work.resolve("common/.scmversion"))
                            .strip()
                            .equals(releaseSuffix()),
                    "Kernel release suffix differs");
        }

        System.out.println("PASS recipe unchanged since prepare");
    }

    /** Builds the prepared tree with pinned tools only; the same steps run on every host. */
    private void build(Path project, Path work, int jobs) throws Exception {
        String componentSha = FilesSupport.hash(recipe.resolve("component.json"));

        verifySource(work);
        String host = KernelHostTools.host();
        var toolchain = Json.obj(component, "toolchain");
        var hosts = Json.obj(toolchain, "hosts");
        require(hosts.containsKey(host), "No pinned kernel toolchain for host " + host);
        var hostPins = Json.obj(hosts, host);
        var tools = KernelHostTools.install(project, host);
        pin(tools.make().toRealPath(), Json.str(hostPins, "make_sha256"), "GNU make differs");
        Path compiler;
        String cc;

        if (api30) {
            Path clang = Storage.confined(project, Json.str(hostPins, "clang_directory"));
            KernelHostTools.members(
                    clang, Storage.confined(recipe, Json.str(hostPins, "clang_manifest")), true);
            compiler = clang.resolve("bin");
            cc = compiler.resolve("clang-12") + " --target=aarch64-linux-gnu";
        } else {
            compiler =
                    KernelHostTools.ndk(project)
                            .resolve("toolchains/llvm/prebuilt/" + host + "/bin");
            pin(
                    compiler.resolve("clang").toRealPath(),
                    Json.str(hostPins, "clang_sha256"),
                    "Clang differs");
            pin(
                    compiler.resolve("ld.lld").toRealPath(),
                    Json.str(hostPins, "lld_sha256"),
                    "LLD differs");
            cc = compiler.resolve("clang").toString();
        }

        for (Path path : List.of(work, project)) {
            require(!path.toString().contains(" "), "Kbuild requires paths without spaces");
        }

        Path source = work.resolve("common");
        Path output = work.resolve("out");
        Path tmp = work.resolve("tmp");
        Files.createDirectories(tmp);
        var env = KernelHostTools.environment(project, tools.zig(), tools.make());
        env.put(
                "PATH",
                String.join(
                        ":",
                        tools.androidBuildTools().resolve("bin").toString(),
                        tools.sed().resolve("bin").toString(),
                        tools.gzip().resolve("bin").toString(),
                        compiler.toString(),
                        env.get("PATH")));
        env.put("TMPDIR", tmp.toString());
        env.put("M4", tools.androidBuildTools().resolve("bin/m4").toString());
        env.put("BISON_PKGDATADIR", tools.androidBuildTools().resolve("common/bison").toString());
        env.put("KBUILD_BUILD_TIMESTAMP", Json.str(toolchain, "kbuild_timestamp"));
        env.put("KBUILD_BUILD_USER", "build-user");
        env.put("KBUILD_BUILD_HOST", "build-host");
        env.put("KBUILD_BUILD_VERSION", "1");
        // Kbuild asks pkg-config for libcrypto; only the pinned OpenSSL is visible.
        Path pkgconfig = tools.openssl().resolve("lib/pkgconfig");
        env.put("PKG_CONFIG_LIBDIR", pkgconfig.toString());
        env.put("PKG_CONFIG_PATH", pkgconfig.toString());
        var args =
                new ArrayList<>(
                        List.of(
                                tools.make().toString(),
                                "-C",
                                source.toString(),
                                "SHELL=/bin/bash",
                                "O=" + output,
                                "ARCH=arm64",
                                "CC=" + cc,
                                "AR=" + compiler.resolve("llvm-ar"),
                                "NM=" + compiler.resolve("llvm-nm"),
                                "OBJCOPY=" + compiler.resolve("llvm-objcopy"),
                                "OBJDUMP=" + compiler.resolve("llvm-objdump"),
                                "STRIP=" + compiler.resolve("llvm-strip"),
                                "HOSTCC=" + tools.zig() + " cc " + KernelHostTools.zigTarget(),
                                "HOSTCXX=" + tools.zig() + " c++ " + KernelHostTools.zigTarget(),
                                "HOSTCFLAGS=" + hostFlags(work, tools),
                                "HOSTLDFLAGS=-L" + tools.openssl().resolve("lib"),
                                "CLANG_TRIPLE=aarch64-linux-gnu-",
                                "CROSS_COMPILE=aarch64-linux-gnu-",
                                // Workspace and compiler paths are relative in every output.
                                "KCFLAGS=" + prefixMaps(work, compiler),
                                "KAFLAGS=" + prefixMaps(work, compiler)));

        if (api30) {
            args.addAll(
                    List.of(
                            "LD="
                                    + compiler.resolve("ld.lld")
                                    + " --thinlto-jobs="
                                    + jobs
                                    + " --threads="
                                    + jobs,
                            "CC_COMPAT="
                                    + compiler.resolve("clang-12")
                                    + " --target=arm-linux-gnueabi -fuse-ld=lld"
                                    + " -Qunused-arguments "
                                    // The compat vDSO ignores KCFLAGS.
                                    + prefixMaps(work, compiler),
                            "CROSS_COMPILE_COMPAT=arm-linux-gnueabi-",
                            "LLVM_IAS=1"));
        } else {
            args.add(
                    "LD="
                            + compiler.resolve("ld.lld")
                            + " -m aarch64linux -z notext --threads="
                            + jobs);
        }

        var config =
                new ArrayList<>(
                        List.of(
                                "/bin/bash",
                                source.resolve("scripts/config").toString(),
                                "--file",
                                output.resolve(".config").toString()));
        config.addAll(
                api30
                        ? List.of(
                                "--enable",
                                "COMPAT_VDSO",
                                "--enable",
                                "GENERIC_COMPAT_VDSO",
                                "--enable",
                                "RELR")
                        : List.of("--disable", "CRYPTO_USER"));
        KernelHostTools.command(work, env, config.toArray(String[]::new));
        make(work, env, args, "olddefconfig");
        verifyConfig(output.resolve(".config"));

        if (api30) {
            make(work, env, args, "-j" + jobs, "headers");
        }

        make(work, env, args, "-j" + jobs, "Image");

        if (!api30) {
            var readelf =
                    new ProcessBuilder(
                                    compiler.resolve("llvm-readelf").toString(),
                                    "--relocations",
                                    "--wide",
                                    output.resolve("vmlinux").toString())
                            .redirectOutput(work.resolve("relocations.txt").toFile())
                            .redirectError(ProcessBuilder.Redirect.INHERIT);
            require(readelf.start().waitFor() == 0, "llvm-readelf failed");
        }

        recordBuild(work, componentSha);
        System.out.println(
                "Built Image "
                        + FilesSupport.hash(output.resolve("arch/arm64/boot/Image"))
                        + " on "
                        + host);
    }

    private static String prefixMaps(Path work, Path compiler) {
        return "-ffile-prefix-map="
                + work
                + "/= -ffile-prefix-map="
                + compiler.getParent()
                + "/=toolchain/";
    }

    /** Kbuild links single-file host programs such as extract-cert with HOSTCFLAGS only. */
    private String hostFlags(Path work, KernelHostTools.Tools tools) {
        String flags =
                "-O2 -I"
                        + work.resolve("host-include")
                        + " -I"
                        + tools.openssl().resolve("include")
                        + " -L"
                        + tools.openssl().resolve("lib");

        return api30 ? flags + " -I" + work.resolve("out/usr/include") : flags;
    }

    private static void make(Path work, Map<String, String> env, List<String> args, String... goals)
            throws Exception {
        var command = new ArrayList<>(args);

        command.addAll(List.of(goals));
        KernelHostTools.command(work, env, command.toArray(String[]::new));
    }

    private void verifyConfig(Path config) throws IOException {
        var flags = Files.readAllLines(config);
        var required = api30 ? API30_REQUIRED : List.of("SECURITY_SELINUX", "MODVERSIONS");

        for (String option : required) {
            require(flags.contains("CONFIG_" + option + "=y"), "Kernel mechanism lost: " + option);
        }

        if (api30) {
            require(!flags.contains("CONFIG_CFI_PERMISSIVE=y"), "Permissive CFI forbidden");
            require(
                    !flags.contains("CONFIG_SECURITY_SELINUX_DISABLE=y"),
                    "SELinux default disabled");
        } else {
            require(
                    !flags.contains("CONFIG_CRYPTO_USER=y"),
                    "CAN protocol21 collides with CRYPTO_USER");
        }
    }

    private static int verifyRelocations(Path file) throws IOException {
        int count = 0;

        try (var lines = Files.lines(file)) {
            for (String line : (Iterable<String>) lines::iterator) {
                var matches = RELOCATION.matcher(line);

                while (matches.find()) {
                    require(
                            matches.group().equals("R_AARCH64_RELATIVE"),
                            "Unsupported A9 relocation " + matches.group());
                    ++count;
                }
            }
        }

        require(count > 0, "Missing A9 kernel relocations");

        return count;
    }

    private void verifyModuleAbi(Path work) throws Exception {
        KernelStockModules.verifyAbi(
                work.resolve("stock-modules.json"),
                work.resolve("out/Module.symvers"),
                work.resolve("out/include/config/kernel.release"));
    }

    private void recordBuild(Path work, String componentSha) throws Exception {
        pin(recipe.resolve("component.json"), componentSha, "Recipe changed during build");
        verifySource(work);
        verifyConfig(work.resolve("out/.config"));

        if (api30) {
            verifyModuleAbi(work);
        } else {
            verifyRelocations(work.resolve("relocations.txt"));
        }

        Path output = work.resolve("out");
        Map<String, Object> receipt = new LinkedHashMap<>();
        receipt.put("schema", 1);
        receipt.put("component_sha256", componentSha);
        receipt.put("image_sha256", FilesSupport.hash(output.resolve("arch/arm64/boot/Image")));
        receipt.put("vmlinux_sha256", FilesSupport.hash(output.resolve("vmlinux")));
        receipt.put("recipe", recipeHashes());
        receipt.put("source_guard", true);
        receipt.put("runtime_accepted", false);

        if (api30) {
            receipt.put(
                    "module_symvers_sha256", FilesSupport.hash(output.resolve("Module.symvers")));
            receipt.put("stock_module_crc_guard", true);
        } else {
            receipt.put("relocation_guard", true);
        }

        write(work.resolve("build-complete.json"), receipt);
    }

    private static Map<String, String> configValues(String text) {
        Map<String, String> values = new LinkedHashMap<>();

        for (String line : text.lines().toList()) {
            var entry = KCONFIG.matcher(line);

            if (entry.matches()) {
                values.put(
                        entry.group(1) == null ? entry.group(3) : entry.group(1),
                        entry.group(1) == null ? "n" : entry.group(2));
            }
        }

        return values;
    }

    private static String whitelist(String config, String path) {
        return WHITELIST
                .matcher(config)
                .replaceFirst(
                        java.util.regex.Matcher.quoteReplacement(
                                "CONFIG_UNUSED_KSYMS_WHITELIST=\"" + path + "\""));
    }

    private void publish(Path project, Path work) throws Exception {
        verifySource(work);
        Path output = work.resolve("out");
        String config = Files.readString(output.resolve(".config"));
        Path stockPath = recipe.resolve("stock-embedded.config");
        String stock = Files.readString(stockPath);
        String release = Files.readString(output.resolve("include/config/kernel.release")).strip();
        String normalized;
        int relocations = 0;

        if (api30) {
            verifyModuleAbi(work);
            var original = configValues(configuration(stock));
            var actual = configValues(config);
            var changed = new TreeSet<>(original.keySet());
            changed.addAll(actual.keySet());
            changed.removeIf(key -> Objects.equals(original.get(key), actual.get(key)));
            changed.removeAll(
                    Set.of(
                            "CONFIG_CC_CAN_LINK",
                            "CONFIG_UAPI_HEADER_TEST",
                            "CONFIG_UNUSED_KSYMS_WHITELIST"));
            require(changed.isEmpty(), "Unexpected kernel config changes: " + changed);

            for (String key : List.of("CONFIG_CC_CAN_LINK", "CONFIG_UAPI_HEADER_TEST")) {
                require(
                        "y".equals(original.get(key)) && !actual.containsKey(key),
                        "Unexpected capability change: " + key);
            }

            normalized = whitelist(config, "abi_symbollist.raw");
        } else {
            require(
                    config.equals(
                            stock.replaceFirst(
                                    "(?m)^CONFIG_CRYPTO_USER=y$",
                                    "# CONFIG_CRYPTO_USER is not set")),
                    "Unexpected A9 configuration change");
            require(release.equals("4.4.302"), "Unexpected A9 kernel release");
            relocations = verifyRelocations(work.resolve("relocations.txt"));
            normalized = config;
        }

        Path image = output.resolve("arch/arm64/boot/Image");
        String imageSha = FilesSupport.hash(image);
        Path vmlinux = output.resolve("vmlinux");
        String vmlinuxSha = FilesSupport.hash(vmlinux);
        var complete = read(work.resolve("build-complete.json"));
        require(
                Json.str(complete, "component_sha256")
                                .equals(FilesSupport.hash(recipe.resolve("component.json")))
                        && Json.str(complete, "image_sha256").equals(imageSha)
                        && Json.str(complete, "vmlinux_sha256").equals(vmlinuxSha),
                "Stale kernel build receipt");

        Path destination =
                Storage.confined(project, ".local/profiles/common/development-kernel/" + imageSha);
        Storage.directory(destination);
        Path publishedImage = destination.resolve("Image");

        if (Files.notExists(publishedImage, LinkOption.NOFOLLOW_LINKS)) {
            Path temp = Files.createTempFile(destination, ".Image.", ".partial");

            try {
                Files.copy(
                        image,
                        temp,
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                        java.nio.file.StandardCopyOption.COPY_ATTRIBUTES);
                pin(temp, imageSha, "Kernel image changed during export");

                try {
                    Files.move(temp, publishedImage);
                } catch (java.nio.file.FileAlreadyExistsException ignored) {
                    // A concurrent export published first; the pin below verifies its content.
                }
            } finally {
                Files.deleteIfExists(temp);
            }
        }

        pin(publishedImage, imageSha, "Existing content-addressed Image differs");
        Storage.atomic(destination.resolve("build.config"), config);

        var provenance = new LinkedHashMap<String, Object>();
        provenance.put("source_revision", Json.str(inputs, "commit"));
        provenance.put("source_tree", Json.str(inputs, "tree"));
        provenance.put("source_content_sha256", Json.str(inputs, "content_sha256"));
        provenance.put("normalized_config_sha256", sha256(normalized));
        provenance.put("boundary_control_bytes", 176);
        provenance.put("kernel_release", release);

        var receipt = new LinkedHashMap<String, Object>();
        receipt.put("schema", 1);
        receipt.put("stage", "built-not-runtime-accepted");
        receipt.put("runtime_accepted", false);
        receipt.put("vmlinux_sha256", vmlinuxSha);
        receipt.put("source_inputs", read(work.resolve("source-inputs.json")));
        provenance.put("toolchain", Json.str(Json.obj(component, "toolchain"), "version"));

        if (api30) {
            var deviations =
                    new ArrayList<>(
                            List.of(
                                    "CONFIG_CC_CAN_LINK unset: host has no Linux userland link"
                                            + " sysroot; guest runtime unchanged by this capability"
                                            + " probe",
                                    "CONFIG_UAPI_HEADER_TEST unset by CONFIG_CC_CAN_LINK;"
                                            + " generated kernel UAPI headers still built",
                                    "CONFIG_UNUSED_KSYMS_WHITELIST rewritten to same 4 977"
                                            + " symbols from 18 pinned source KMI lists"));

            if (variant != null) {
                deviations.add(Json.str(variant, "deviation"));
            }

            provenance.put("configuration_deviations", deviations);
            provenance.put("stock_module_count", 37);
            provenance.put("stock_import_crc_count", 2594);
            Path symvers = output.resolve("Module.symvers");
            Storage.atomic(destination.resolve("Module.symvers"), Files.readString(symvers));
            receipt.put("config_sha256", sha256(config));
            receipt.put("module_symvers_sha256", FilesSupport.hash(symvers));
            var hostPins =
                    Json.obj(
                            Json.obj(Json.obj(component, "toolchain"), "hosts"),
                            KernelHostTools.host());
            receipt.put(
                    "compiler_manifest_sha256",
                    FilesSupport.hash(
                            Storage.confined(recipe, Json.str(hostPins, "clang_manifest"))));
        } else {
            provenance.put(
                    "configuration_deviations",
                    List.of(
                            "CONFIG_CRYPTO_USER=n for development CAN netlink protocol21; unchanged"
                                    + " from previously accepted custom A9 kernel"));
            receipt.put("relocation_types", List.of("R_AARCH64_RELATIVE"));
            receipt.put("relocation_count", relocations);
            receipt.put(
                    "limitations",
                    List.of(
                            "development ECU only; not physical CAN",
                            "unchanged linker -z notext; ELF layout is not proof of runtime page"
                                    + " protections",
                            "machine boot/native RX/cold-boundary acceptance still required"));
        }

        String relative = project.relativize(publishedImage).toString();
        var descriptor = new LinkedHashMap<String, Object>();
        descriptor.put("url", releaseUrl(project, imageSha));
        descriptor.put("sha256", imageSha);
        descriptor.put("abi", "arm64-v8a");
        descriptor.put("provenance", provenance);
        receipt.put("descriptor", descriptor);
        write(work.resolve("development-kernel.json"), descriptor);
        write(destination.resolve("build.json"), receipt);
        System.out.println(
                "Built development kernel: " + relative + "; runtime acceptance still required");
    }

    /** Profiles download the CI-built release asset; see the kernels workflow. */
    private String releaseUrl(Path project, String imageSha) throws Exception {
        var kernelBuild =
                Json.obj(
                        Json.obj(KernelHostTools.toolchain(project), "optionalTools"),
                        "kernelBuild");

        return Json.str(kernelBuild, "releaseBase")
                + "kernel-"
                + releaseName()
                + "-"
                + imageSha.substring(0, 8)
                + "/Image";
    }

    private static String sha256(String value) throws Exception {
        var digest = MessageDigest.getInstance("SHA-256");

        return HexFormat.of().formatHex(digest.digest(value.getBytes(StandardCharsets.UTF_8)));
    }
}
