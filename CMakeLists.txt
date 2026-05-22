cmake_minimum_required(VERSION 3.21)
project(MalloyStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT DEFINED Qt6_DIR AND NOT DEFINED CMAKE_PREFIX_PATH)
    if(WIN32 AND EXISTS "C:/msys64/mingw64/lib/cmake/Qt6")
        list(APPEND CMAKE_PREFIX_PATH "C:/msys64/mingw64")
    endif()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Test Svg)

qt_standard_project_setup()

set(MALLOY_CORE_SOURCES
    src/capture/DxgiCapture.cpp
    src/capture/DxgiCapture.h
    src/capture/CaptureController.cpp
    src/capture/CaptureController.h
    src/capture/MonitorInfo.h
    src/capture/WindowCapture.cpp
    src/capture/WindowCapture.h
    src/capture/WindowCaptureSession.cpp
    src/capture/WindowCaptureSession.h
    src/capture/WasapiCapture.cpp
    src/capture/WasapiCapture.h
    src/audio/AudioController.cpp
    src/audio/AudioController.h
    src/audio/AudioInput.h
    src/media/TimedSource.h
    src/recording/OutputSettings.h
    src/recording/EncoderPipeline.cpp
    src/recording/EncoderPipeline.h
    src/recording/StreamingPipeline.cpp
    src/recording/StreamingPipeline.h
    src/recording/StreamSettings.cpp
    src/recording/StreamSettings.h
    src/recording/MediaController.cpp
    src/recording/MediaController.h
    src/recording/RingTimedFrameSource.cpp
    src/recording/RingTimedFrameSource.h
    src/recording/RingTimedPcmSource.cpp
    src/recording/RingTimedPcmSource.h
    src/recording/EncoderRegistry.cpp
    src/recording/EncoderRegistry.h
    src/input/HotkeyManager.cpp
    src/input/HotkeyManager.h
    src/project/ProjectDocument.cpp
    src/project/ProjectDocument.h
    src/project/ClipsRegistry.cpp
    src/project/ClipsRegistry.h
    src/project/ProjectRegistry.cpp
    src/project/ProjectRegistry.h
    src/project/MediaRegistry.cpp
    src/project/MediaRegistry.h
    src/recording/RenderQueue.cpp
    src/recording/RenderQueue.h
    src/model/Canvas.h
    src/model/FilterEffect.cpp
    src/model/FilterEffect.h
    src/model/Scene.cpp
    src/model/Scene.h
    src/model/SceneItem.cpp
    src/model/SceneItem.h
    src/model/Source.cpp
    src/model/Source.h
    src/model/SceneCollection.cpp
    src/model/SceneCollection.h
)

set(MALLOY_UI_SOURCES
    src/MainWindow.cpp
    src/MainWindow.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/IconFactory.cpp
    src/ui/IconFactory.h
    src/ui/CommandPalette.cpp
    src/ui/CommandPalette.h
    src/ui/OnboardingOverlay.cpp
    src/ui/OnboardingOverlay.h
    src/ui/shell/NavModel.h
    src/ui/shell/IconRail.cpp
    src/ui/shell/IconRail.h
    src/ui/shell/WorkspaceHeader.cpp
    src/ui/shell/WorkspaceHeader.h
    src/ui/shell/StudioStatusBar.cpp
    src/ui/shell/StudioStatusBar.h
    src/ui/shell/AppShell.cpp
    src/ui/shell/AppShell.h
    src/ui/components/PanelFrame.cpp
    src/ui/components/PanelFrame.h
    src/ui/components/Placeholder.cpp
    src/ui/components/Placeholder.h
    src/ui/components/MeterBar.cpp
    src/ui/components/MeterBar.h
    src/ui/workspaces/RecordingWorkspace.cpp
    src/ui/workspaces/RecordingWorkspace.h
    src/ui/workspaces/StreamingWorkspace.cpp
    src/ui/workspaces/StreamingWorkspace.h
    src/ui/workspaces/SettingsWorkspace.cpp
    src/ui/workspaces/SettingsWorkspace.h
    src/ui/workspaces/AILabWorkspace.cpp
    src/ui/workspaces/AILabWorkspace.h
    src/ui/workspaces/EditorWorkspace.cpp
    src/ui/workspaces/EditorWorkspace.h
    src/ui/workspaces/LibraryWorkspaces.cpp
    src/ui/workspaces/LibraryWorkspaces.h
    src/ui/dashboard/Dashboard.cpp
    src/ui/dashboard/Dashboard.h
    src/ui/ControlsBar.cpp
    src/ui/ControlsBar.h
    src/ui/AudioMixerPanel.cpp
    src/ui/AudioMixerPanel.h
    src/ui/VuMeter.cpp
    src/ui/VuMeter.h
    src/ui/OutputSettingsDialog.cpp
    src/ui/OutputSettingsDialog.h
    src/ui/StreamSettingsDialog.cpp
    src/ui/StreamSettingsDialog.h
    src/ui/HotkeysDialog.cpp
    src/ui/HotkeysDialog.h
    src/ui/PreviewWidget.cpp
    src/ui/PreviewWidget.h
    src/ui/ScenesPanel.cpp
    src/ui/ScenesPanel.h
    src/ui/SourcesPanel.cpp
    src/ui/SourcesPanel.h
    src/ui/InspectorPanel.cpp
    src/ui/InspectorPanel.h
    src/ui/MonitorPickerDialog.cpp
    src/ui/MonitorPickerDialog.h
    src/ui/WindowPickerDialog.cpp
    src/ui/WindowPickerDialog.h
    src/ui/MicrophonePickerDialog.cpp
    src/ui/MicrophonePickerDialog.h
)

# v8: bundled SVG icon set + dark.qss stylesheet. CMAKE_AUTORCC is ON (line 10)
# so listing the .qrc is enough — the auto-generated qrc_malloy.cpp gets compiled
# into the executable automatically.
set(MALLOY_RESOURCES
    src/resources/malloy.qrc
)

qt_add_executable(MalloyStudio WIN32
    src/main.cpp
    ${MALLOY_UI_SOURCES}
    ${MALLOY_CORE_SOURCES}
    ${MALLOY_RESOURCES}
)

target_include_directories(MalloyStudio PRIVATE src)

target_link_libraries(MalloyStudio PRIVATE
    Qt6::Widgets
    Qt6::Svg
    d3d11
    dxgi
    uuid
    ole32
    propsys
    mmdevapi
    avrt
    user32
    gdi32
)

enable_testing()

qt_add_executable(MalloyStudioTests
    tests/model_tests.cpp
    ${MALLOY_CORE_SOURCES}
)

# Force console subsystem so CTest captures stdout test output
set_target_properties(MalloyStudioTests PROPERTIES WIN32_EXECUTABLE FALSE)

target_include_directories(MalloyStudioTests PRIVATE src)

target_link_libraries(MalloyStudioTests PRIVATE
    Qt6::Widgets
    Qt6::Test
    d3d11
    dxgi
    uuid
    ole32
    propsys
    mmdevapi
    avrt
    user32
    gdi32
)

add_test(NAME MalloyStudioTests COMMAND MalloyStudioTests)
