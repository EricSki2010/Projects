cmake_minimum_required(VERSION 3.20)
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
project(PerlinNoiseVisualizer)

set(CMAKE_CXX_STANDARD 17)
set_property(GLOBAL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PerlinNoiseVisualizer
    main.cpp
    perlinNoiseManagement.cpp
    scene.cpp
    ../VisualEngine/VisualEngine.cpp
    ../VisualEngine/renderingManagement/Mesh.cpp
    ../VisualEngine/renderingManagement/Scene.cpp
    ../VisualEngine/renderingManagement/Shader.cpp
    ../VisualEngine/renderingManagement/Texture.cpp
    ../VisualEngine/renderingManagement/RenderLoop.cpp
    ../VisualEngine/renderingManagement/meshing/ChunkMesh.cpp
    ../VisualEngine/renderingManagement/meshing/Overlay.cpp
    ../VisualEngine/renderingManagement/primitives/LineRenderer.cpp
    ../VisualEngine/renderingManagement/primitives/DotRenderer.cpp
    ../VisualEngine/renderingManagement/effects/GradientBackground.cpp
    ../VisualEngine/renderingManagement/effects/RenderToTexture.cpp
    ../VisualEngine/sceneManagement/SceneManager.cpp
    ../VisualEngine/memoryManagement/memory.cpp
    ../VisualEngine/uiManagement/UIRenderer.cpp
    ../VisualEngine/uiManagement/UIManager.cpp
    ../VisualEngine/uiManagement/UIPrefabs.cpp
    ../VisualEngine/uiManagement/TextRenderer.cpp
    ../VisualEngine/inputManagement/Camera.cpp
    ../VisualEngine/inputManagement/Collision.cpp
    ../VisualEngine/inputManagement/Raycasting.cpp
    E:/glad/src/glad.c
)

target_include_directories(PerlinNoiseVisualizer PRIVATE
    ../VisualEngine
    ../thirdparty
    E:/glad/include
    C:/vcpkg/installed/x64-windows-static/include
)

set_property(TARGET PerlinNoiseVisualizer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(glfw3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)

target_link_libraries(PerlinNoiseVisualizer PRIVATE
    glfw
    opengl32
    gdi32
    user32
    shell32
    Freetype::Freetype
)
