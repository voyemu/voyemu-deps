package com.voyemu.prepare;

import org.jf.dexlib2.AccessFlags;
import org.jf.dexlib2.Opcode;
import org.jf.dexlib2.Opcodes;
import org.jf.dexlib2.builder.MutableMethodImplementation;
import org.jf.dexlib2.builder.instruction.BuilderInstruction3rc;
import org.jf.dexlib2.iface.ClassDef;
import org.jf.dexlib2.iface.Method;
import org.jf.dexlib2.iface.MethodParameter;
import org.jf.dexlib2.iface.debug.DebugItem;
import org.jf.dexlib2.iface.debug.EndLocal;
import org.jf.dexlib2.iface.debug.RestartLocal;
import org.jf.dexlib2.iface.instruction.OneRegisterInstruction;
import org.jf.dexlib2.iface.instruction.ReferenceInstruction;
import org.jf.dexlib2.iface.instruction.RegisterRangeInstruction;
import org.jf.dexlib2.immutable.ImmutableClassDef;
import org.jf.dexlib2.immutable.ImmutableDexFile;
import org.jf.dexlib2.immutable.ImmutableMethod;
import org.jf.dexlib2.immutable.ImmutableMethodImplementation;
import org.jf.dexlib2.immutable.debug.ImmutableEndLocal;
import org.jf.dexlib2.immutable.debug.ImmutableRestartLocal;
import org.jf.dexlib2.immutable.reference.ImmutableMethodReference;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/** Inserts the fixed guest Ethernet setup on the successful framework readiness edge. */
final class InterguestStartupGuard {
    static final String NAME = "api30-interguest-startup";
    private static final String OWNER = "Lcom/android/server/SystemServer;";
    private static final String SERVICE = "Lcom/android/server/NetworkManagementService;";
    private static final String CALLBACK = "Lcom/android/server/VoyemuInterguestStartup;";
    private static final String READY = SERVICE + "->systemReady()V";
    private static final ImmutableMethodReference SCHEDULE =
            new ImmutableMethodReference(CALLBACK, "schedule", List.of(SERVICE), "V");

    private InterguestStartupGuard() {}

    private static void require(boolean condition) {
        Failure.require(
                condition,
                "ANDROID_INTERGUEST_STARTUP",
                "Exact pinned API30 Ethernet setup and readiness edge required",
                3);
    }

    static void validate(String platform, int api, Path input, Path source, byte[] primary)
            throws Exception {
        require(
                "qinggan11".equals(platform)
                        && api == 30
                        && FilesSupport.hash(input).equals(HiboardTopGestureGuard.PIN));
        require(
                FilesSupport.tree(source)
                        .keySet()
                        .equals(
                                Set.of(
                                        "com/android/server/VoyemuInterguestStartup.java",
                                        "compile-only/com/android/server/NetworkManagementService.java")));
        var type = HiboardTopGestureGuard.classes(primary).get(OWNER);
        require(type != null);
        int matched = 0;

        for (Method method : type.getMethods()) {
            if (readies(method)) {
                verifyReverse(method, rewrite(method, true));
                ++matched;
            }
        }

        require(matched == 1);
    }

    static byte[] rewritePrimary(byte[] primary) throws Exception {
        var classes = HiboardTopGestureGuard.classes(primary);
        var systemServer = classes.get(OWNER);

        require(systemServer != null);
        var direct = new ArrayList<Method>();
        var virtual = new ArrayList<Method>();
        int changed =
                patch(systemServer.getDirectMethods(), direct)
                        + patch(systemServer.getVirtualMethods(), virtual);

        require(changed == 1);
        classes.put(
                OWNER,
                new ImmutableClassDef(
                        systemServer.getType(),
                        systemServer.getAccessFlags(),
                        systemServer.getSuperclass(),
                        systemServer.getInterfaces(),
                        systemServer.getSourceFile(),
                        systemServer.getAnnotations(),
                        systemServer.getStaticFields(),
                        systemServer.getInstanceFields(),
                        direct,
                        virtual));
        byte[] result =
                FrameworkDexMaterializer.serialize(
                        new ImmutableDexFile(Opcodes.forApi(30), classes.values()));
        var reread = HiboardTopGestureGuard.classes(result);
        require(reread.keySet().equals(classes.keySet()));

        for (ClassDef reloaded : reread.values()) {
            require(
                    FrameworkDexMaterializer.semantic(reloaded)
                            .equals(
                                    FrameworkDexMaterializer.semantic(
                                            classes.get(reloaded.getType()))));
        }

        return result;
    }

    /** The caller is the one SystemServer method that makes NetworkManagementService ready. */
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

    private static int patch(Iterable<? extends Method> methods, List<Method> out)
            throws Exception {
        int changed = 0;

        for (Method method : methods) {
            if (readies(method)) {
                Method patched = rewrite(method, true);
                verifyReverse(method, patched);
                out.add(patched);
                ++changed;
            } else {
                out.add(method);
            }
        }

        return changed;
    }

    private static int width(MethodParameter parameter) {
        return parameter.getType().equals("J") || parameter.getType().equals("D") ? 2 : 1;
    }

    /** Register of the only NetworkManagementService parameter of the caller. */
    private static int serviceRegister(Method method) {
        boolean instance = !AccessFlags.STATIC.isSet(method.getAccessFlags());
        int words = instance ? 1 : 0;

        for (MethodParameter parameter : method.getParameters()) {
            words += width(parameter);
        }

        int register = method.getImplementation().getRegisterCount() - words + (instance ? 1 : 0);
        int found = -1;

        for (MethodParameter parameter : method.getParameters()) {
            if (parameter.getType().equals(SERVICE)) {
                require(found < 0);
                found = register;
            }

            register += width(parameter);
        }

        require(found >= 0);

        return found;
    }

    private static boolean guarded(MutableMethodImplementation code, int start, int units) {
        return code.getTryBlocks().stream()
                .anyMatch(block ->
                        block.getStartCodeAddress() == start && block.getCodeUnitCount() == units);
    }

    private static void verifyReverse(Method original, Method patched) throws Exception {
        require(
                FrameworkDexMaterializer.semantic(original.getImplementation())
                        .equals(
                                FrameworkDexMaterializer.semantic(
                                        rewrite(patched, false).getImplementation())));
    }

    private static Method rewrite(Method method, boolean forward) {
        require(method.getImplementation() != null && method.getReturnType().equals("V"));
        var code = new MutableMethodImplementation(method.getImplementation());
        var instructions = code.getInstructions();
        int service = serviceRegister(method);
        int found = 0;

        for (int index = 1; index + 2 < instructions.size(); ++index) {
            var lookup = instructions.get(index);

            if (!(lookup instanceof ReferenceInstruction reference)
                    || !reference.getReference().toString().equals(READY)) {
                continue;
            }

            var nullable = instructions.get(index - 1);
            var success = instructions.get(index + 1);
            int start = lookup.getLocation().getCodeAddress();
            int units = lookup.getCodeUnits();
            // The service parameter is null-checked, then systemReady alone is guarded by a try.
            require(
                    lookup.getOpcode() == Opcode.INVOKE_VIRTUAL_RANGE
                            && lookup instanceof RegisterRangeInstruction registers
                            && registers.getStartRegister() == service
                            && registers.getRegisterCount() == 1
                            && nullable.getOpcode() == Opcode.IF_EQZ
                            && ((OneRegisterInstruction) nullable).getRegisterA() == service);
            require(guarded(code, start, units));

            if (forward) {
                require(
                        success.getOpcode() == Opcode.GOTO
                                && instructions.get(index + 2).getOpcode()
                                        == Opcode.MOVE_EXCEPTION);
                // Keep the original try-end label at the first instruction after systemReady.
                code.replaceInstruction(
                        index + 1,
                        new BuilderInstruction3rc(
                                Opcode.INVOKE_STATIC_RANGE, service, 1, SCHEDULE));
                code.addInstruction(index + 2, success);
            } else {
                require(
                        success.getOpcode() == Opcode.INVOKE_STATIC_RANGE
                                && success instanceof ReferenceInstruction hook
                                && hook.getReference().toString().equals(SCHEDULE.toString()));
                var originalGoto = instructions.get(index + 2);
                require(originalGoto.getOpcode() == Opcode.GOTO);
                code.removeInstruction(index + 2);
                code.replaceInstruction(index + 1, originalGoto);
            }

            require(guarded(code, start, units));
            ++found;
        }

        require(found == 1);

        return new ImmutableMethod(
                method.getDefiningClass(),
                method.getName(),
                method.getParameters(),
                method.getReturnType(),
                method.getAccessFlags(),
                method.getAnnotations(),
                method.getHiddenApiRestrictions(),
                new ImmutableMethodImplementation(
                        code.getRegisterCount(),
                        code.getInstructions(),
                        code.getTryBlocks(),
                        locals(method.getImplementation().getDebugItems(), code.getDebugItems())));
    }

    /** Restores the local names that the builder drops from end-local and restart-local items. */
    private static List<DebugItem> locals(
            Iterable<? extends DebugItem> original, Iterable<? extends DebugItem> rewritten) {
        var names = original.iterator();
        var out = new ArrayList<DebugItem>();

        for (DebugItem item : rewritten) {
            require(names.hasNext());
            DebugItem source = names.next();
            require(source.getDebugItemType() == item.getDebugItemType());

            if (source instanceof EndLocal local) {
                out.add(
                        new ImmutableEndLocal(
                                item.getCodeAddress(),
                                local.getRegister(),
                                local.getName(),
                                local.getType(),
                                local.getSignature()));
            } else if (source instanceof RestartLocal local) {
                out.add(
                        new ImmutableRestartLocal(
                                item.getCodeAddress(),
                                local.getRegister(),
                                local.getName(),
                                local.getType(),
                                local.getSignature()));
            } else {
                out.add(item);
            }
        }

        require(!names.hasNext());

        return out;
    }

    static byte[] addHelper(byte[] secondary, byte[] helper) throws Exception {
        var classes = HiboardTopGestureGuard.classes(secondary);
        var additions = HiboardTopGestureGuard.classes(helper);

        require(
                additions
                        .keySet()
                        .equals(
                                Set.of(
                                        CALLBACK,
                                        "Lcom/android/server/VoyemuInterguestStartup$Attempt;")));

        for (var addition : additions.entrySet()) {
            require(classes.putIfAbsent(addition.getKey(), addition.getValue()) == null);
        }

        return FrameworkDexMaterializer.serialize(
                new ImmutableDexFile(Opcodes.forApi(30), classes.values()));
    }
}
