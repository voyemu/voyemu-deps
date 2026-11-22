package com.voyemu.prepare;

import org.jf.dexlib2.Opcode;
import org.jf.dexlib2.Opcodes;
import org.jf.dexlib2.iface.ClassDef;
import org.jf.dexlib2.iface.Method;
import org.jf.dexlib2.iface.instruction.Instruction;
import org.jf.dexlib2.iface.instruction.ReferenceInstruction;
import org.jf.dexlib2.immutable.ImmutableClassDef;
import org.jf.dexlib2.immutable.ImmutableDexFile;
import org.jf.dexlib2.immutable.ImmutableMethod;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.zip.ZipFile;

/**
 * Actual pinned clean services DEX: the Ethernet hook follows the readiness edge wherever it is.
 */
public final class InterguestStartupGuardTest {
    private static final String OWNER = "Lcom/android/server/SystemServer;";
    private static final String READY =
            "Lcom/android/server/NetworkManagementService;->systemReady()V";
    private static final String SCHEDULE =
            "Lcom/android/server/VoyemuInterguestStartup;->schedule("
                    + "Lcom/android/server/NetworkManagementService;)V";

    private interface Action {
        void run() throws Exception;
    }

    private static void check(boolean value) {
        if (!value) {
            throw new AssertionError("interguest startup contract");
        }
    }

    private static void denied(Action action) throws Exception {
        try {
            action.run();
        } catch (Failure expected) {
            return;
        }

        throw new AssertionError("ambiguous readiness edge accepted");
    }

    private static boolean readies(Method method) {
        if (method.getImplementation() == null) {
            return false;
        }

        for (var instruction : method.getImplementation().getInstructions()) {
            if (instruction instanceof ReferenceInstruction reference
                    && reference.getReference().toString().equals(READY)) {
                return true;
            }
        }

        return false;
    }

    /** Counts readiness calls immediately followed by the hook, and those without it. */
    private static int[] edges(byte[] dex) {
        int hooked = 0, plain = 0;

        for (Method method : HiboardTopGestureGuard.classes(dex).get(OWNER).getMethods()) {
            if (method.getImplementation() == null) {
                continue;
            }

            var instructions = new ArrayList<Instruction>();
            method.getImplementation().getInstructions().forEach(instructions::add);

            for (int index = 0; index + 1 < instructions.size(); ++index) {
                if (instructions.get(index) instanceof ReferenceInstruction reference
                        && reference.getReference().toString().equals(READY)) {
                    var next = instructions.get(index + 1);

                    if (next.getOpcode() == Opcode.INVOKE_STATIC_RANGE
                            && ((ReferenceInstruction) next)
                                    .getReference()
                                    .toString()
                                    .equals(SCHEDULE)) {
                        ++hooked;
                    } else {
                        ++plain;
                    }
                }
            }
        }

        return new int[] {hooked, plain};
    }

    private static Method renamed(Method method, String name) {
        return new ImmutableMethod(
                method.getDefiningClass(),
                name,
                method.getParameters(),
                method.getReturnType(),
                method.getAccessFlags(),
                method.getAnnotations(),
                method.getHiddenApiRestrictions(),
                method.getImplementation());
    }

    /** Replaces the caller by renamed copies, as another lambda numbering of javac would. */
    private static byte[] withCallers(byte[] dex, String... names) throws Exception {
        var classes = HiboardTopGestureGuard.classes(dex);
        ClassDef type = classes.get(OWNER);
        var virtual = new ArrayList<Method>();

        for (Method method : type.getVirtualMethods()) {
            if (readies(method)) {
                for (String name : names) {
                    virtual.add(renamed(method, name));
                }
            } else {
                virtual.add(method);
            }
        }

        classes.put(
                OWNER,
                new ImmutableClassDef(
                        type.getType(),
                        type.getAccessFlags(),
                        type.getSuperclass(),
                        type.getInterfaces(),
                        type.getSourceFile(),
                        type.getAnnotations(),
                        type.getStaticFields(),
                        type.getInstanceFields(),
                        type.getDirectMethods(),
                        virtual));

        return FrameworkDexMaterializer.serialize(
                new ImmutableDexFile(Opcodes.forApi(30), classes.values()));
    }

    public static void main(String[] args) throws Exception {
        Path input = Path.of(args[0]);
        byte[] original;

        try (var jar = new ZipFile(input.toFile())) {
            original = jar.getInputStream(jar.getEntry("classes.dex")).readAllBytes();
        }

        check(edges(original)[0] == 0 && edges(original)[1] == 1);
        byte[] patched = InterguestStartupGuard.rewritePrimary(original);
        check(edges(patched)[0] == 1 && edges(patched)[1] == 0);
        denied(() -> InterguestStartupGuard.rewritePrimary(patched));

        byte[] renumbered = withCallers(original, "lambda$startOtherServices$9$SystemServer");
        byte[] repatched = InterguestStartupGuard.rewritePrimary(renumbered);
        check(edges(repatched)[0] == 1 && edges(repatched)[1] == 0);

        byte[] twice =
                withCallers(
                        original,
                        "lambda$startOtherServices$6$SystemServer",
                        "lambda$startOtherServices$7$SystemServer");
        denied(() -> InterguestStartupGuard.rewritePrimary(twice));
        System.out.println(
                "Pinned API30 readiness edge hooked by structure, renumbered caller hooked,"
                        + " repeated and duplicated edges denied PASS; runtime unverified");
    }
}
