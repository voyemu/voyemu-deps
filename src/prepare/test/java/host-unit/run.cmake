cmake_minimum_required(VERSION 3.29)
# Source-only preparation checks: each class compiles from src/prepare and runs without OEM inputs.
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
include("${ROOT}/src/prepare/test/jdk.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../java_source_dependencies.cmake")
# Classes that take the repository root as their only argument.
set(ROOT_TESTS
    Api28WrapperCodecTest Api30RecipesTest ArchivesTest ArgumentsTest CarAvailabilityTest
    FontCyrillicFixTest FrameworkBridgeVerifierTest FrameworkDexMaterializerTest BluetoothComponentProducerTest
    OwnAudioComponentsProducerTest PhonePackageProducerTest HiboardNormalizationTest
    NativeFilesTest SettingsVoiceMuteRecipeTest KeyManagerIngressRecipeTest
    KeyManagerRecipeTest OriginalNaviIdentityRecipesTest SettingsVoiceCloneRecipeTest
    StockStatusBarRecipeTest InputsTest ManifestLibrariesTest NxpMediaLibraryTest
    NxpSettingsLibraryTest PassionBluetoothRoleRecipeTest PassionDiagnosticsRecipeTest
    PassionMusicDayDrawableRecipeTest PassionMusicForegroundRecipeTest PassionSidebarRecipeTest
    PassionStatusBarRecipeTest PassionSystemExtReceiptTest PassionVehicleAnimationRecipeTest
    PreparationMainTest QnxFileClientTest QnxOriginalsTest SigningIntermediateTest SourceEditsTest
    VehicleCaptureTest Api30BootstrapTest ActivationPublisherTest RecipeCoverageTest SecondDonorApkTest)
# Classes that take no arguments and read the repository from the working directory.
set(PLAIN_TESTS
    SdkJavaCliClosureTest Api30OriginalNaviNativeSelectionTest Api30ReceiptTest CameraRecipeTest DexEditsTest
    HiboardRecipeTest OriginalNaviRecipesTest MultiDisplayStartupTest
    PassionMusicInsetsRecipeTest ProfilePropertiesTest RecipesTest StorageJsonTest VdexQuickeningTest
    ViewDumpPatchTest)
# Classes that take a fresh writable directory as their only argument.
set(WORK_TESTS ComponentSourcesTest KernelSourceTest ProfileRegistryIsolationTest RecipeCompletenessTest)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef TOKEN)
set(WORK "${ROOT}/.local/build/jobs/prepare-java-host-unit-${TOKEN}")
# Classes with their own arguments; JDK is the home that contains the javac used here.
get_filename_component(JDK "${JAVAC}" REALPATH)
get_filename_component(JDK "${JDK}/../.." ABSOLUTE)
set(PLATFORM "${ROOT}/.local/toolchains/java-sdk/platforms/android-34/android.jar")
set(AAPT2 "${ROOT}/.local/toolchains/java-sdk/build-tools/36.1.0/aapt2")
if(WIN32)
    string(APPEND AAPT2 ".exe")
endif()
if(NOT EXISTS "${PLATFORM}" OR NOT EXISTS "${AAPT2}")
    message(STATUS "Prepared SDK required: ${PLATFORM} and ${AAPT2}")
    cmake_language(EXIT 77)
endif()
set(A9 "${ROOT}/profiles/free-09-chinese-20260106-6.0.3")
set(QINGGAN11_731 "${ROOT}/profiles/free-11-chinese-20260731-7.3.1")
set(PASSION "${ROOT}/profiles/passion-11-russian-20260403-2.1.4.80")
set(ARGS_NetworkSetupReadinessTest --root "${ROOT}")
set(ARGS_FreeStartupAudioHostTest "${ROOT}" "${WORK}/FreeStartupAudioHostTest" "${JDK}")
set(ARGS_HiboardTopGestureHostTest --root "${ROOT}" --work "${WORK}/HiboardTopGestureHostTest" --jdk "${JDK}")
set(ARGS_PassionPowerHelperHostTest --root "${ROOT}" --work "${WORK}/PassionPowerHelperHostTest" --jdk "${JDK}" --platform "${PLATFORM}")
set(ARGS_PassionPowerHelperContractTest "${ROOT}" "${PLATFORM}" "${WORK}/PassionPowerHelperContractTest")
set(ARGS_Api30StatusBarAndroidDexTest "${ROOT}" "${WORK}/Api30StatusBarAndroidDexTest" "${PLATFORM}")
set(ARGS_Api28ViewObservationTest "${ROOT}" "${PLATFORM}" "${WORK}/Api28ViewObservationTest")
set(ARGS_DonorJarTest "${ROOT}" "${PLATFORM}")
set(ARGS_DonorJarBranchTest "${ROOT}" "${PLATFORM}")
set(ARGS_PhoneDexTest "${A9}/patches/phone-provider/src" "${PLATFORM}" "${WORK}/PhoneDexTest")
set(ARGS_PhoneRecipeTest "${A9}/patches/phone-provider.json")
set(ARGS_NxpSettingsRecipeDispatchTest "${ROOT}/profiles/free-09-chinese-20221230-nxp-rc45/patches/nxp-systemui-settings-managers.json")
set(ARGS_OriginalNaviHelperCompileTest "${PLATFORM}" "${WORK}/OriginalNaviHelperCompileTest")
set(ARGS_StockStatusBarAndroidDexTest "${QINGGAN11_731}/patches/api30-stock-statusbar/src" "${PLATFORM}"
    "${WORK}/StockStatusBarAndroidDexTest")
set(ARGS_StockStatusBarWindowTest
    "${QINGGAN11_731}/patches/api30-stock-statusbar/src/com/voyemu/output/StockStatusBarWindow.java"
    "${WORK}/StockStatusBarWindowTest")
set(ARGS_JavaDexCompileOnlyTest "${ROOT}" "${PLATFORM}" "${WORK}/JavaDexCompileOnlyTest")
set(ARGS_JavaSourcesTest "${PLATFORM}")
set(ARGS_GuestJavaTest "${PLATFORM}" "${WORK}/GuestJavaTest")
set(ARGS_GuestJavaRecipeTest "${ROOT}/.local/toolchains/java-sdk" "${WORK}/GuestJavaRecipeTest")
set(ARGS_PassionSidebarWindowTest
    "${PASSION}/patches/passion-api30-sidebars/src/com/voyemu/output/PassionSidebarWindow.java"
    "${WORK}/PassionSidebarWindowTest")
set(ARGS_PassionStatusBarAndroidDexTest "${PASSION}/patches/passion-api30-statusbar/src" "${PLATFORM}"
    "${WORK}/PassionStatusBarAndroidDexTest")
set(ARGS_PassionStatusBarWindowTest
    "${PASSION}/patches/passion-api30-statusbar/src/com/voyemu/output/PassionStatusBarWindow.java"
    "${WORK}/PassionStatusBarWindowTest")
set(ARGS_ResourceLinkTest "${ROOT}" "${AAPT2}" "${PLATFORM}")
set(ARGS_SignaturesTest "${PLATFORM}")
set(ARGUMENT_TESTS Api28ViewObservationTest NxpSettingsRecipeDispatchTest NetworkSetupReadinessTest FreeStartupAudioHostTest
    HiboardTopGestureHostTest PassionPowerHelperHostTest PassionPowerHelperContractTest
    Api30StatusBarAndroidDexTest DonorJarTest DonorJarBranchTest
    PhoneDexTest PhoneRecipeTest OriginalNaviHelperCompileTest
    StockStatusBarAndroidDexTest StockStatusBarWindowTest JavaDexCompileOnlyTest
    JavaSourcesTest GuestJavaTest GuestJavaRecipeTest PassionSidebarWindowTest PassionStatusBarAndroidDexTest PassionStatusBarWindowTest
    ResourceLinkTest SignaturesTest)
file(MAKE_DIRECTORY "${WORK}/classes" "${WORK}/tmp")
set(SOURCES)
foreach(NAME IN LISTS ROOT_TESTS WORK_TESTS ARGUMENT_TESTS PLAIN_TESTS)
    list(APPEND SOURCES "${ROOT}/src/prepare/test/java/com/voyemu/prepare/${NAME}.java")
endforeach()
# Kernel build classes live outside the package tree and are listed explicitly.
file(GLOB KERNEL_SOURCES "${ROOT}/src/prepare/kernel/*.java")
list(APPEND SOURCES ${KERNEL_SOURCES})
execute_process(COMMAND "${JAVAC}" -J-Xmx512m -J-XX:ActiveProcessorCount=1 --release 25 -encoding UTF-8 -proc:none -nowarn
    -cp "${JAVA_SOURCE_CLASSPATH}" -sourcepath "${ROOT}/src/prepare/java${CP_SEPARATOR}${ROOT}/src/prepare/test/java"
    -d "${WORK}/classes" ${SOURCES}
    TIMEOUT 180 RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Preparation host unit compilation: ${RESULT}\n${OUTPUT}\n${ERROR}")
endif()
set(FAILED)
foreach(NAME IN LISTS ROOT_TESTS WORK_TESTS ARGUMENT_TESTS PLAIN_TESTS)
    if(DEFINED ARGS_${NAME})
        set(ARGUMENT ${ARGS_${NAME}})
    elseif(NAME IN_LIST PLAIN_TESTS)
        set(ARGUMENT)
    elseif(NAME IN_LIST WORK_TESTS)
        set(ARGUMENT "${WORK}/${NAME}")
        file(MAKE_DIRECTORY "${ARGUMENT}")
    else()
        set(ARGUMENT "${ROOT}")
    endif()
    execute_process(COMMAND "${JAVA}" -Xmx512m -XX:ActiveProcessorCount=1 "-Djava.io.tmpdir=${WORK}/tmp"
        -cp "${WORK}/classes${CP_SEPARATOR}${JAVA_SOURCE_CLASSPATH}" "com.voyemu.prepare.${NAME}" ${ARGUMENT}
        WORKING_DIRECTORY "${ROOT}" TIMEOUT 60 RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    file(WRITE "${WORK}/${NAME}.log" "${OUTPUT}${ERROR}")
    if(RESULT EQUAL 0)
        message(STATUS "${NAME}: passed")
    else()
        message(STATUS "${NAME}: failed ${RESULT}\n${OUTPUT}\n${ERROR}")
        list(APPEND FAILED "${NAME}")
    endif()
endforeach()
if(FAILED)
    message(FATAL_ERROR "Preparation host unit failures: ${FAILED}; diagnostics: ${WORK}")
endif()
file(REMOVE_RECURSE "${WORK}")
