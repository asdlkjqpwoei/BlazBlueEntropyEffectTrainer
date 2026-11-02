cmake_minimum_required(VERSION 4.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
    if(DEFINED VCPKG_ROOT AND NOT VCPKG_ROOT STREQUAL "")
        file(TO_CMAKE_PATH "${VCPKG_ROOT}" VcpkgRoot)
    elseif(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VcpkgRoot)
    elseif(NOT CMAKE_PREFIX_PATH AND "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "" AND
           NOT yaml-cpp_DIR AND NOT lz4_DIR)
        find_program(VcpkgExecutable NAMES vcpkg NO_CACHE)
        if(VcpkgExecutable)
            file(REAL_PATH "${VcpkgExecutable}" VcpkgExecutablePath)
            get_filename_component(VcpkgRoot "${VcpkgExecutablePath}" DIRECTORY)
        endif()
    endif()
    if(VcpkgRoot)
        if(NOT EXISTS "${VcpkgRoot}/scripts/buildsystems/vcpkg.cmake")
            message(FATAL_ERROR
                "The selected vcpkg root does not contain scripts/buildsystems/vcpkg.cmake. "
                "Set -DVCPKG_ROOT=<vcpkg installation root> or CMAKE_TOOLCHAIN_FILE explicitly.")
        endif()
        set(CMAKE_TOOLCHAIN_FILE "${VcpkgRoot}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "Detected or user-configured vcpkg toolchain")
        message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

project(BlazblueTrainer VERSION 0.2.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "Visual Studio" OR
    NOT CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
    message(FATAL_ERROR "This WinUI application requires a Visual Studio MSVC x64 generator.")
endif()
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported build configurations" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

find_package(yaml-cpp CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
option(BUILD_TESTING "Build the BlazblueTests module" ON)
if(BUILD_TESTING)
    enable_testing()
endif()

set(TrainerDirectory "${CMAKE_CURRENT_SOURCE_DIR}/Source/BlazblueTrainer")
set(TestDirectory "${CMAKE_CURRENT_SOURCE_DIR}/Source/BlazblueTests")
set(ConfigFile "${CMAKE_CURRENT_SOURCE_DIR}/Config/Defaults.yaml")
set(BinaryDirectory "${CMAKE_CURRENT_SOURCE_DIR}/Binaries")
set(NativeProjectDirectory "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(NativeIntermediateDirectory "${CMAKE_CURRENT_BINARY_DIR}/Intermediate")
file(MAKE_DIRECTORY "${NativeProjectDirectory}")

set(TrainerSources
    "${TrainerDirectory}/Core/AudioFeedback.cpp"
    "${TrainerDirectory}/Core/Config.cpp"
    "${TrainerDirectory}/Core/DataBodyCatalog.cpp"
    "${TrainerDirectory}/Core/DataBodySave.cpp"
    "${TrainerDirectory}/Core/GameData.cpp"
    "${TrainerDirectory}/Core/GamePresenceMonitor.cpp"
    "${TrainerDirectory}/Core/Hotkeys.cpp"
    "${TrainerDirectory}/Core/Il2CppScanner.cpp"
    "${TrainerDirectory}/Core/Localization.cpp"
    "${TrainerDirectory}/Core/Operations.cpp"
    "${TrainerDirectory}/Core/Paths.cpp"
    "${TrainerDirectory}/Core/PuertsRuntime.cpp"
    "${TrainerDirectory}/Core/RemoteProcess.cpp"
    "${TrainerDirectory}/Core/SaveDocument.cpp"
    "${TrainerDirectory}/Core/TrainerApi.cpp"
    "${TrainerDirectory}/Core/TrainerEngine.cpp"
    "${TrainerDirectory}/Core/WindowBehavior.cpp"
)
file(GLOB TrainerHeaders CONFIGURE_DEPENDS
    "${TrainerDirectory}/Core/*.h" "${TrainerDirectory}/Public/*.h")
function(EscapeXml Input Output)
    string(REPLACE "&" "&amp;" Value "${Input}")
    string(REPLACE "<" "&lt;" Value "${Value}")
    string(REPLACE ">" "&gt;" Value "${Value}")
    set(${Output} "${Value}" PARENT_SCOPE)
endfunction()

file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" NativeSourceRoot)
file(TO_NATIVE_PATH "${NativeIntermediateDirectory}" NativeIntermediateRoot)
file(TO_NATIVE_PATH "${BinaryDirectory}" NativeBinaryRoot)
EscapeXml("${NativeSourceRoot}" XmlSourceRoot)
EscapeXml("${NativeIntermediateRoot}" XmlIntermediateRoot)
EscapeXml("${NativeBinaryRoot}" XmlBinaryRoot)
set(CoreCompileItems "")
set(HeaderItems "")
set(FilterItems "")
foreach(Source IN LISTS TrainerSources)
    file(TO_NATIVE_PATH "${Source}" NativeSource)
    EscapeXml("${NativeSource}" XmlSource)
    string(APPEND CoreCompileItems
        "    <ClCompile Include=\"${XmlSource}\"><PrecompiledHeader>NotUsing</PrecompiledHeader><ObjectFileName>$(IntDir)Core\\%(Filename).obj</ObjectFileName></ClCompile>\n")
    string(APPEND FilterItems
        "    <ClCompile Include=\"${XmlSource}\"><Filter>Core</Filter></ClCompile>\n")
endforeach()
foreach(Header IN LISTS TrainerHeaders)
    file(TO_NATIVE_PATH "${Header}" NativeHeader)
    EscapeXml("${NativeHeader}" XmlHeader)
    string(APPEND HeaderItems "    <ClInclude Include=\"${XmlHeader}\" />\n")
    string(APPEND FilterItems
        "    <ClInclude Include=\"${XmlHeader}\"><Filter>Core</Filter></ClInclude>\n")
endforeach()

configure_file(CMake/BlazblueTrainer.vcxproj.in
    "${NativeProjectDirectory}/BlazblueTrainer.vcxproj" @ONLY)
configure_file(CMake/BlazblueTrainer.vcxproj.filters.in
    "${NativeProjectDirectory}/BlazblueTrainer.vcxproj.filters" @ONLY)
include(CMake/Dependencies.cmake)
foreach(Configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
    WriteNativeDependencies("${Configuration}")
    configure_file("${ConfigFile}" "${BinaryDirectory}/${Configuration}/Config/Defaults.yaml" COPYONLY)
endforeach()

if(NOT CMAKE_VS_MSBUILD_COMMAND OR NOT EXISTS "${CMAKE_VS_MSBUILD_COMMAND}")
    message(FATAL_ERROR "The selected Visual Studio instance did not provide MSBuild.")
endif()
execute_process(
    COMMAND "${CMAKE_VS_MSBUILD_COMMAND}" "${NativeProjectDirectory}/BlazblueTrainer.vcxproj"
        /t:Restore /nologo /v:q /p:Configuration=Debug /p:Platform=x64
    RESULT_VARIABLE RestoreResult
)
if(NOT RestoreResult EQUAL 0)
    message(FATAL_ERROR "The WinUI package restore failed.")
endif()

include_external_msproject(BlazblueTrainer
    "${NativeProjectDirectory}/BlazblueTrainer.vcxproj"
    GUID "73FB3A47-66BD-4FE9-AB93-6B1AC9ED82E8" PLATFORM x64)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT BlazblueTrainer)

if(BUILD_TESTING)
    file(GLOB TestSources CONFIGURE_DEPENDS "${TestDirectory}/*.cpp" "${TestDirectory}/Diagnostics/*.cpp")
    add_executable(BlazblueTests ${TestSources} ${TrainerSources} ${TrainerHeaders})
    target_include_directories(BlazblueTests PRIVATE
        "${TrainerDirectory}/Core" "${TrainerDirectory}/Public" "${TestDirectory}")
    target_link_libraries(BlazblueTests PRIVATE yaml-cpp::yaml-cpp lz4::lz4 comctl32 winmm shell32 ole32 advapi32)
    target_compile_definitions(BlazblueTests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(BlazblueTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    set_target_properties(BlazblueTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BinaryDirectory}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${BinaryDirectory}/$<CONFIG>"
    )
    source_group(TREE "${TestDirectory}" PREFIX "Tests" FILES ${TestSources})
    source_group(TREE "${TrainerDirectory}" PREFIX "Product" FILES ${TrainerSources} ${TrainerHeaders})
    add_test(NAME BlazblueTests COMMAND BlazblueTests)
    get_target_property(YamlType yaml-cpp::yaml-cpp TYPE)
    if(YamlType STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET BlazblueTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:yaml-cpp::yaml-cpp>" "$<TARGET_FILE_DIR:BlazblueTests>"
            VERBATIM)
    endif()
    ResolveImportedLibrary(lz4::lz4 Lz4Target)
    get_target_property(Lz4Type "${Lz4Target}" TYPE)
    if(Lz4Type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET BlazblueTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${Lz4Target}>" "$<TARGET_FILE_DIR:BlazblueTests>"
            VERBATIM)
    endif()
endif()

file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/BlazBlueEntropyEffectTrainer.sln")
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/BlazBlueEntropyEffectTrainer.slnx")
