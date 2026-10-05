cmake_minimum_required(VERSION 3.24)

project(BackgroundKernel VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "BackgroundKernel targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_executable(CommentGate policy/NoComments.cpp)
add_test(NAME Comments COMMAND CommentGate "${CMAKE_SOURCE_DIR}")
add_custom_target(CommentCheck ALL
    COMMAND $<TARGET_FILE:CommentGate> "${CMAKE_SOURCE_DIR}"
    DEPENDS CommentGate
    VERBATIM
)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(KERNEL_TICK_RATE "144" CACHE STRING "")
if(NOT KERNEL_TICK_RATE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "KERNEL_TICK_RATE must be a positive integer")
endif()
if(KERNEL_TICK_RATE GREATER 48000)
    message(FATAL_ERROR "KERNEL_TICK_RATE cannot exceed the 48000 Hz audio clock")
endif()
add_compile_definitions("BK_TICK_RATE=${KERNEL_TICK_RATE}")
message(STATUS
    "Tick rate: ${KERNEL_TICK_RATE} Hz")

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

add_executable(RoomPreview WIN32
    src/app/HdrSceneMain.cpp
    src/audio/AudioLoopback.cpp
    src/render/HdrRoomRenderer.cpp
    src/visualizer/AudioTrailMotion.cpp
)
set_target_properties(RoomPreview PROPERTIES OUTPUT_NAME "RoomPreview")
target_include_directories(RoomPreview PRIVATE src)
target_compile_definitions(RoomPreview PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(RoomPreview PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(RoomPreview PRIVATE
    d3d12
    dxgi
    d3dcompiler
    dxguid
    ole32
    uuid
    user32
)

add_executable(SceneKernel WIN32
    src/app/SceneKernelMain.cpp
)

add_executable(StrokeLab WIN32
    src/app/StrokeLabMain.cpp
    src/render/WallStrokeCanvas.cpp
    src/audio/AudioLoopback.cpp
)
target_include_directories(StrokeLab PRIVATE src)
target_compile_definitions(StrokeLab PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
)
target_compile_options(StrokeLab PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(StrokeLab PRIVATE
    d3d11 dxgi d3dcompiler dxguid user32 advapi32 gdi32 comctl32 ole32 uuid
)
add_dependencies(SceneKernel StrokeLab)

add_executable(BakedRoomLight
    src/app/BakeRoomLightMain.cpp
    src/scene/BakedRoomLight.cpp
)
target_include_directories(BakedRoomLight PRIVATE src)
target_compile_definitions(BakedRoomLight PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(BakedRoomLight PRIVATE /W4 /permissive- /EHsc)

add_executable(AudioScene WIN32
    src/app/HdrSceneMain.cpp
    src/audio/AudioLoopback.cpp
    src/render/HdrRoomRenderer.cpp
    src/visualizer/AudioTrailMotion.cpp
)
set_target_properties(AudioScene PROPERTIES OUTPUT_NAME "AudioScene")
target_include_directories(AudioScene PRIVATE src)
target_compile_definitions(AudioScene PRIVATE
    BK_DESKTOP
    BK_VISUALIZER
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(AudioScene PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(AudioScene PRIVATE
    d3d12
    dxgi
    d3dcompiler
    dxguid
    ole32
    uuid
    user32
)
add_dependencies(AudioScene BakedRoomLight)
add_custom_command(TARGET AudioScene POST_BUILD
    COMMAND $<TARGET_FILE:BakedRoomLight>
        "$<TARGET_FILE_DIR:AudioScene>/BakedRoomLight.bin"
    VERBATIM
)

add_executable(KeypadControl WIN32
    src/app/KeypadMain.cpp
)
set_target_properties(KeypadControl PROPERTIES OUTPUT_NAME "KeypadControl")
target_compile_definitions(KeypadControl PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(KeypadControl PRIVATE /W4 /permissive- /EHsc)
target_include_directories(KeypadControl PRIVATE src)
target_link_libraries(KeypadControl PRIVATE user32 advapi32 gdi32 comctl32)
foreach(app IN ITEMS RoomPreview AudioScene)
    target_link_libraries(${app} PRIVATE advapi32 ws2_32)
    target_sources(${app} PRIVATE src/render/BakedRoomScene.cpp src/render/HdrSceneOutput.cpp src/scene/BakedRoomLight.cpp src/tracking/OpenTrackPose.cpp)
endforeach()
set_target_properties(SceneKernel PROPERTIES OUTPUT_NAME "SceneKernel")
target_include_directories(SceneKernel PRIVATE src)
target_compile_definitions(SceneKernel PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(SceneKernel PRIVATE /W4 /permissive- /EHsc)
target_link_options(SceneKernel PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/SceneKernel.manifest"
)
target_link_libraries(SceneKernel PRIVATE
    user32
    advapi32
    gdi32
    comctl32
    ole32
    uuid
)

add_executable(DwmHookHost
    src/dwm_hook_host/DwmHookHostMain.cpp
    src/dwm_hook_host/RemoteDwmHook.cpp
)
set_target_properties(DwmHookHost PROPERTIES OUTPUT_NAME "DwmHookHost")
target_include_directories(DwmHookHost PRIVATE src)
target_compile_definitions(DwmHookHost PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(DwmHookHost PRIVATE /W4 /permissive- /EHsc)
target_link_options(DwmHookHost PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)
target_link_libraries(DwmHookHost PRIVATE advapi32 version)

add_library(DwmHookModule SHARED
    src/dwm_hook/DwmHookModule.cpp
    src/dwm_hook/OutputHook.cpp
    src/dwm_hook/HookRenderer.cpp
)
set_target_properties(DwmHookModule PROPERTIES PREFIX "" OUTPUT_NAME "DwmHookModule")
target_include_directories(DwmHookModule PRIVATE src)
target_compile_definitions(DwmHookModule PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(DwmHookModule PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(DwmHookModule PRIVATE
    minhook
    d3d11
    d3d12
    d3dcompiler
    dxgi
    dxguid
    user32
)

add_executable(FutureAudio
    src/app/FutureAudioMain.cpp
    src/audio/AudioLoopback.cpp
)
set_target_properties(FutureAudio PROPERTIES OUTPUT_NAME "FutureAudio")
target_include_directories(FutureAudio PRIVATE src)
target_compile_definitions(FutureAudio PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(FutureAudio PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(FutureAudio PRIVATE ole32 uuid)

add_executable(DelayRecorder
    src/app/DelayRecorderMain.cpp
    src/audio/PcmRecord.cpp
    src/audio/AudioLoopback.cpp
)
set_target_properties(DelayRecorder PROPERTIES OUTPUT_NAME "DelayRecorder")
target_include_directories(DelayRecorder PRIVATE src)
target_compile_definitions(DelayRecorder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(DelayRecorder PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(DelayRecorder PRIVATE ole32 uuid)

foreach(target IN ITEMS
    RoomPreview
    SceneKernel
    StrokeLab
    BakedRoomLight
    AudioScene
    KeypadControl
    DwmHookHost
    DwmHookModule
    FutureAudio
    DelayRecorder)
    add_dependencies(${target} CommentCheck)
endforeach()

foreach(target IN ITEMS KeypadControl SceneKernel StrokeLab AudioScene)
    target_sources(${target} PRIVATE src/app/AppVersion.rc)
endforeach()

install(TARGETS KeypadControl SceneKernel StrokeLab AudioScene
    RUNTIME DESTINATION .
    COMPONENT Runtime)
install(FILES
    "$<TARGET_FILE_DIR:AudioScene>/BakedRoomLight.bin"
    Install-Keypad.ps1
    Start-Keypad.ps1
    Start-Keypad.cmd
    Stop-Kernel.cmd
    Remove-Keypad.ps1
    Remove-Keypad.cmd
    DESTINATION .
    COMPONENT Runtime)
