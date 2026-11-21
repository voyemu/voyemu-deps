# Compiles the host preparation sources and their host tests, guest Java and the host tests that run
# guest sources on JVM Android doubles on the pinned JDK of this host with the pinned Error Prone;
# every Error Prone and javac warning fails the check.
# Usage: cmake [-DVOYEMU_ERRORPRONE_OUTPUT=<directory>] -P src/app/errorprone.cmake
cmake_minimum_required(VERSION 3.22)
get_filename_component(voyemu_root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(voyemu_errorprone_roots
    src/app/core/generator
    src/app/graphics/generator
    src/dev/checks
    src/dev/format
    src/prepare/JavaHelperBuild.java
    src/prepare/JavaRuntimeBuild.java
    src/prepare/java
    src/prepare/kernel
    src/prepare/test/java/com/voyemu/prepare
)
# Host tests checked with their guest sources in voyemu_errorprone_test_units instead.
set(voyemu_errorprone_excluded_files
    src/prepare/test/java/com/voyemu/prepare/HomePreferenceTest.java
    src/prepare/test/java/com/voyemu/prepare/PassionAmbientLightTest.java
    src/prepare/test/java/com/voyemu/prepare/PassionBluetoothVolumeTest.java
    src/prepare/test/java/com/voyemu/prepare/PassionOneKeyMuteTest.java
    src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerCallbacksTest.java
    src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerTest.java
    src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerJournalConcurrencyTest.java
)
# Every component and patch directory under profiles and every src/guest/android package is one
# guest unit: its Java outside test and host-tests directories compiles alone at the guest
# level --release 8 against the pinned Android platform. These units compile only against classes
# of imported OEM originals, so only the format check covers them.
set(voyemu_errorprone_oem_units
    profiles/common/components/audiopolicy-disabled
    profiles/common/components/canbus
    profiles/free-09-chinese-20221230-nxp-rc45/components/canbus
    profiles/free-09-chinese-20221230-nxp-rc45/components/wheel
    profiles/common-qinggan-09/components/audiopolicy
    profiles/free-09-chinese-20260106-6.0.3/components/media
    profiles/common-qinggan-09/components/oem-services
    profiles/free-09-chinese-20260106-6.0.3/patches/api28-usb-player
    profiles/free-11-chinese-20250710-6.11.1/components/bluetooth-611
    profiles/free-11-chinese-20250710-6.11.1/components/oem-power
    profiles/free-11-chinese-20250710-6.11.1/patches/api30-usb-player
    profiles/free-11-chinese-20260731-7.3.1/components/oem-power
    profiles/free-11-chinese-20260731-7.3.1/patches/api30-usb-player
    profiles/free-11-russian-20260318-rc1/components/oem-power
    profiles/passion-11-russian-20260403-2.1.4.80/patches/passion-api30-mcu-property
)
# Host test units at the host level against JVM Android doubles, the pinned Android platform and
# the host sources, as in their CTest runners; "|" joins the files and directories of one unit.
set(voyemu_errorprone_test_units
    "src/prepare/test/java/com/voyemu/prepare/PassionAmbientLightTest.java|src/prepare/test/fixtures/passion-ambient-light|profiles/passion-11-russian-20260403-2.1.4.80/components/window-compat/src/com/voyemu/window/PassionAmbientLight.java"
    "src/prepare/test/java/com/voyemu/prepare/PassionBluetoothVolumeTest.java|src/prepare/test/fixtures/passion-bluetooth-volume|profiles/passion-11-russian-20260403-2.1.4.80/components/window-compat/src/com/voyemu/window/PassionBluetoothVolume.java"
    "src/prepare/test/java/com/voyemu/prepare/PassionOneKeyMuteTest.java|src/prepare/test/fixtures/passion-onekey-mute|profiles/passion-11-russian-20260403-2.1.4.80/components/window-compat/src/com/voyemu/window/PassionOneKeyMute.java"
    "src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerTest.java|profiles/common/components/bluetooth/src/SyntheticPhonePeer.java"
    "src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerJournalConcurrencyTest.java|profiles/common/components/bluetooth/src/SyntheticPhonePeer.java"
    "src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerJournalConcurrencyTest.java|profiles/free-09-chinese-20221230-nxp-rc45/components/phone-bluetooth/src/SyntheticPhonePeer.java"
    "src/prepare/test/java/com/voyemu/prepare/HomePreferenceTest.java|src/prepare/test/fixtures/qinggan09-home-preferred|profiles/free-09-chinese-20260106-6.0.3/patches/usercenter-network/src/com/voyemu/oem/HomePreference.java"
    "src/prepare/test/java/com/voyemu/prepare/SyntheticPhonePeerCallbacksTest.java|src/prepare/test/fixtures/phone-peer-process/android|src/prepare/test/fixtures/phone-peer-process/com|src/prepare/test/fixtures/phone-peer-process/org|src/prepare/test/fixtures/phone-pbap-race/com|profiles/common/components/bluetooth/src"
    "src/prepare/test/java/com/qinggan/canbus/service/CanBusAccStatusTest.java|src/prepare/test/fixtures/canbus-model|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanModelBridge.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanModelClient.java"
    "src/prepare/test/java/com/qinggan/canbus/service/CanBusComponentTest.java|src/prepare/test/fixtures/canbus-component|src/prepare/test/fixtures/canbus-model/android/net|src/prepare/test/canbus-observation/com/voyemu/prepare/PhonePeerJsonBridge.java|profiles/common/components/canbus/src/qinggan09/com/qinggan/canbus/service/ClusterTheme.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/BaseCanBusComponent.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanBusState.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanModelBridge.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanModelClient.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanObservationJournal.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/ClusterThemeStore.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/ICanBusOperator.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IIpkSyncComponentBase.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkFrames.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkInfoSync.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkLink.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/PhoneDeliveryDiagnostics.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/VehicleInputSnapshot.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/VehicleInputStore.java|profiles/common/components/canbus/src/qinggan09/com/qinggan/canbus/service/VehicleSignalCatalog.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/protocol/dongfeng/DongfengCanBusComponentImpl.java"
    "src/prepare/test/java/com/qinggan/canbus/service/IpkFramesTest.java|src/prepare/test/java/com/qinggan/canbus/service/IpkLinkTest.java|src/prepare/test/java/com/qinggan/canbus/service/IpkInfoSyncTest.java|src/prepare/test/fixtures/canbus-hfp/android/os/Bundle.java|src/prepare/test/fixtures/canbus-hfp/android/system/ErrnoException.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/CanObservationJournal.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkFrames.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkInfoSync.java|profiles/common/components/canbus/src/com/qinggan/canbus/service/IpkLink.java"
    "src/prepare/test/java/com/voyemu/oem/CarSignalStubTest.java|src/prepare/test/fixtures/carsignal|profiles/common/components/carsignal/src"
    "src/prepare/test/java/com/qinggan/audiopolicy/AudioPolicyStubServiceTest.java|src/prepare/test/fixtures/audiopolicy-stub|profiles/common-qinggan-09/components/audiopolicy/src/compile-only|profiles/common-qinggan-09/components/audiopolicy/src/com/qinggan/audiopolicy/AudioPolicyStubService.java"
    "src/prepare/test/java/com/voyemu/oem/PowerManagerStubTest.java|src/prepare/test/fixtures/oem-power|profiles/common/components/oem-power/src|profiles/common-qinggan-09/components/oem-services/src/com/voyemu/oem/PowerManagerStub.java|profiles/common-qinggan-09/components/oem-services/src/compile-only/com/qinggan/power/IPowerManagerListener.java|profiles/common-qinggan-09/components/oem-services/src/compile-only/com/qinggan/system/IPowerManagerService.java"
    "profiles/common/components/view-observation/host-tests|profiles/common/components/view-observation/src"
    "profiles/free-09-chinese-20260106-6.0.3/patches/camera-capabilities/test|profiles/free-09-chinese-20260106-6.0.3/patches/camera-capabilities/src"
    src/prepare/test/phone-producer
    src/prepare/test/free-wheel-ingress
    src/prepare/test/nxp-phone
    src/prepare/test/phone-peer-provider
    src/prepare/test/phone-provider
    src/prepare/test/phone-provider-binding
    src/prepare/test/fixtures/canbus-hfp
    src/prepare/test/fixtures/com
    src/prepare/test/fixtures/phone-provider
    src/prepare/test/fixtures/hiboard-top-gesture
    src/prepare/test/fixtures/passion-mcu-property
    src/prepare/test/fixtures/phone-peer
)
if(NOT VOYEMU_ERRORPRONE_OUTPUT)
    set(VOYEMU_ERRORPRONE_OUTPUT "${voyemu_root}/.local/build/errorprone")
endif()
file(READ "${voyemu_root}/src/app/toolchain.json" voyemu_toolchain_json)
file(READ "${voyemu_root}/src/prepare/toolchain.json" voyemu_prepare_toolchain_json)
file(READ "${voyemu_root}/src/prepare/libraries.json" voyemu_libraries_json)

# Stops unless file exists with the pinned SHA-256; hint names the pin to install.
function(voyemu_pinned_file file sha256 hint)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} not found; install ${hint}")
    endif()
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL sha256)
        message(FATAL_ERROR "${file} differs from ${hint}")
    endif()
endfunction()

if(CMAKE_HOST_WIN32)
    set(voyemu_jdk_host windowsBuild)
    set(voyemu_path_separator ";")
elseif(CMAKE_HOST_APPLE)
    set(voyemu_jdk_host macosBuild)
    set(voyemu_path_separator ":")
else()
    set(voyemu_jdk_host linuxBuild)
    set(voyemu_path_separator ":")
endif()
string(JSON voyemu_jdk GET "${voyemu_toolchain_json}" optionalTools ${voyemu_jdk_host} jdk directory)
set(voyemu_javac "${voyemu_root}/${voyemu_jdk}")
if(CMAKE_HOST_APPLE)
    # The macOS archive wraps the JDK home in Contents/Home.
    string(APPEND voyemu_javac "/Contents/Home")
endif()
string(APPEND voyemu_javac "/bin/javac")
if(CMAKE_HOST_WIN32)
    string(APPEND voyemu_javac ".exe")
endif()
if(NOT EXISTS "${voyemu_javac}")
    message(FATAL_ERROR "${voyemu_javac} not found; install the optionalTools.${voyemu_jdk_host}.jdk pin from src/app/toolchain.json")
endif()

string(JSON voyemu_errorprone_directory GET "${voyemu_toolchain_json}" optionalTools errorProne directory)
set(voyemu_processor_path)
foreach(voyemu_jar IN ITEMS core dataflow)
    string(JSON voyemu_name GET "${voyemu_toolchain_json}" optionalTools errorProne jars ${voyemu_jar} jar)
    string(JSON voyemu_sha256 GET "${voyemu_toolchain_json}" optionalTools errorProne jars ${voyemu_jar} sha256)
    set(voyemu_file "${voyemu_root}/${voyemu_errorprone_directory}/${voyemu_name}")
    voyemu_pinned_file("${voyemu_file}" "${voyemu_sha256}" "the optionalTools.errorProne pin from src/app/toolchain.json")
    list(APPEND voyemu_processor_path "${voyemu_file}")
endforeach()

# The same classpath as the Java helper build: pinned Maven libraries and the pinned SDK D8 and apksigner.
set(voyemu_classpath)
string(JSON voyemu_library_count LENGTH "${voyemu_libraries_json}")
math(EXPR voyemu_library_last "${voyemu_library_count} - 1")
foreach(voyemu_index RANGE ${voyemu_library_last})
    string(JSON voyemu_name GET "${voyemu_libraries_json}" ${voyemu_index} filename)
    string(JSON voyemu_sha256 GET "${voyemu_libraries_json}" ${voyemu_index} sha256)
    set(voyemu_file "${voyemu_root}/.local/toolchains/java/libraries/${voyemu_name}")
    voyemu_pinned_file("${voyemu_file}" "${voyemu_sha256}" "the src/prepare/libraries.json pin")
    list(APPEND voyemu_classpath "${voyemu_file}")
endforeach()
string(JSON voyemu_build_tools GET "${voyemu_toolchain_json}" sdk buildTools)
foreach(voyemu_name IN ITEMS d8 apksigner)
    string(JSON voyemu_sha256 GET "${voyemu_prepare_toolchain_json}" libraries ${voyemu_name} sha256)
    set(voyemu_file "${voyemu_root}/.local/toolchains/java-sdk/build-tools/${voyemu_build_tools}/lib/${voyemu_name}.jar")
    voyemu_pinned_file("${voyemu_file}" "${voyemu_sha256}" "the src/prepare/toolchain.json ${voyemu_name} pin")
    list(APPEND voyemu_classpath "${voyemu_file}")
endforeach()

string(JSON voyemu_compile_sdk GET "${voyemu_toolchain_json}" sdk compileSdk)
set(voyemu_platform "${voyemu_root}/.local/toolchains/java-sdk/platforms/android-${voyemu_compile_sdk}/android.jar")
if(NOT EXISTS "${voyemu_platform}")
    message(FATAL_ERROR "${voyemu_platform} not found; install the sdk.archives platform pin from src/app/toolchain.json")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/source_files.cmake")
voyemu_source_files(voyemu_tracked "${voyemu_root}" "*.java")
list(JOIN voyemu_processor_path "${voyemu_path_separator}" voyemu_processor_path)
list(JOIN voyemu_classpath "${voyemu_path_separator}" voyemu_classpath)
file(REMOVE_RECURSE "${VOYEMU_ERRORPRONE_OUTPUT}")

# Error Prone runs inside javac and needs the compiler internals exported to it.
set(voyemu_exports)
foreach(voyemu_package IN ITEMS api file main model parser processing tree util)
    list(APPEND voyemu_exports "-J--add-exports=jdk.compiler/com.sun.tools.javac.${voyemu_package}=ALL-UNNAMED")
endforeach()
foreach(voyemu_package IN ITEMS code comp)
    list(APPEND voyemu_exports "-J--add-opens=jdk.compiler/com.sun.tools.javac.${voyemu_package}=ALL-UNNAMED")
endforeach()

# Sets output to the selected Java files equal to or under paths, without excluded files.
function(voyemu_select output paths excluded)
    set(files)
    foreach(file IN LISTS voyemu_tracked)
        foreach(path IN LISTS paths)
            string(FIND "${file}" "${path}/" position)
            if((file STREQUAL path OR position EQUAL 0) AND NOT file IN_LIST excluded)
                list(APPEND files "${file}")
                break()
            endif()
        endforeach()
    endforeach()
    set(${output} "${files}" PARENT_SCOPE)
endfunction()

# Runs Error Prone with plugin options over files as unit at release with the extra argument-file lines.
set(voyemu_units 0)
set(voyemu_count 0)
set(voyemu_failed)
function(voyemu_errorprone unit release plugin extra files)
    if(NOT files)
        message(FATAL_ERROR "No Java sources selected for ${unit}")
    endif()
    math(EXPR index "${voyemu_units} + 1")
    set(voyemu_units ${index} PARENT_SCOPE)
    set(output "${VOYEMU_ERRORPRONE_OUTPUT}/${index}")
    file(MAKE_DIRECTORY "${output}/classes")
    # An argument file keeps the Windows command line short; javac reads quoted forward-slash paths.
    set(arguments "-processorpath \"${voyemu_processor_path}\"\n${extra}")
    foreach(file IN LISTS files)
        string(APPEND arguments "\"${voyemu_root}/${file}\"\n")
    endforeach()
    file(WRITE "${output}/arguments.txt" "${arguments}")
    list(LENGTH files count)
    math(EXPR count "${voyemu_count} + ${count}")
    set(voyemu_count ${count} PARENT_SCOPE)
    # -Xlint:-options keeps the obsolete --release 8 notice from failing -Werror.
    execute_process(COMMAND "${voyemu_javac}" -J-Xmx1500m ${voyemu_exports}
        -XDcompilePolicy=simple --should-stop=ifError=FLOW -XDaddTypeAnnotationsToSymbol=true
        "-Xplugin:ErrorProne${plugin}" -Werror -Xlint:-options -Xmaxwarns 10000 -Xmaxerrs 10000 --release ${release}
        -encoding UTF-8 -proc:none -implicit:none -d "${output}/classes" "@${output}/arguments.txt"
        WORKING_DIRECTORY "${voyemu_root}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        set(voyemu_failed ${voyemu_failed} "${unit}" PARENT_SCOPE)
    endif()
endfunction()

# Host code and host tests: the classpath of the Java helper build.
voyemu_select(voyemu_files "${voyemu_errorprone_roots}" "${voyemu_errorprone_excluded_files}")
voyemu_errorprone(host 25 "" "-classpath \"${voyemu_classpath}\"\n" "${voyemu_files}")

# Guest units: Java outside test and host-tests directories, against the Android platform.
set(voyemu_guest_units)
foreach(voyemu_file IN LISTS voyemu_tracked)
    if(voyemu_file MATCHES "^(profiles/[^/]+/(components|patches)/[^/]+|src/guest/android/[^/]+)/")
        list(APPEND voyemu_guest_units "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES voyemu_guest_units)
foreach(voyemu_unit IN LISTS voyemu_guest_units)
    if(voyemu_unit IN_LIST voyemu_errorprone_oem_units)
        continue()
    endif()
    voyemu_select(voyemu_files "${voyemu_unit}" "")
    list(FILTER voyemu_files EXCLUDE REGEX "/(test|host-tests)/")
    if(voyemu_files)
        voyemu_errorprone("${voyemu_unit}" 8 "" "-classpath \"${voyemu_platform}\"\n" "${voyemu_files}")
    endif()
endforeach()

# Host test units: JVM Android doubles in sources precede the Android platform on the classpath.
# Guest sources are checked in their guest units, and doubles and fixtures also compile with guest
# code at --release 8, so the instanceof pattern suggestion stays off here.
set(voyemu_test_plugin " -XepExcludedPaths:.*/(profiles/[^/]+/[^/]+/[^/]+/src|src/guest)/.* -Xep:PatternMatchingInstanceof:OFF")
foreach(voyemu_unit IN LISTS voyemu_errorprone_test_units)
    string(REPLACE "|" ";" voyemu_paths "${voyemu_unit}")
    voyemu_select(voyemu_files "${voyemu_paths}" "")
    voyemu_errorprone("${voyemu_unit}" 25 "${voyemu_test_plugin}"
        "-classpath \"${voyemu_platform}\"\n-sourcepath \"${voyemu_root}/src/prepare/java\"\n" "${voyemu_files}")
endforeach()

if(voyemu_failed)
    string(REPLACE ";" "\n  " voyemu_failed "${voyemu_failed}")
    message(FATAL_ERROR "Error Prone found problems in:\n  ${voyemu_failed}")
endif()
message(STATUS "Error Prone check passed for ${voyemu_count} Java files in ${voyemu_units} units")
