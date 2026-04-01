# Zenith

`Zenith` is a small Direct3D 12 model viewer and rendering sandbox built first and foremost as a **beginner D3D12 learning resource**.

The goal of the project is not just to show a final rendered model, but to keep the renderer understandable: camera setup, pipeline creation, constant buffers, model loading, lighting, shadow mapping, and a small amount of editor-style UI are all kept visible in the codebase so they can be studied and modified.

## What the app is for

- learning the structure of a small real-time `D3D12` renderer
- experimenting with directional lighting and point lighting
- studying directional and point-shadow workflows
- loading common 3D assets into a simple viewer
- rendering a viewport image to disk

## Screenshots

<img width="1920" height="1080" alt="render" src="https://github.com/user-attachments/assets/581d3678-7c96-4504-a3dd-af241e6c7c70" />


<img width="1918" height="1131" alt="Screenshot 2026-04-01 163836" src="https://github.com/user-attachments/assets/3e24b43f-eada-4d2e-9398-34494fe13685" />

## Prerequisites

Recommended environment:

- Windows 10/11
- Visual Studio with desktop C++ tooling
- MSVC toolset compatible with `v145`
- Windows 10 SDK
- NuGet restore enabled
- `vcpkg` available for manifest-based dependency restore

## Dependencies

This project currently depends on:

### NuGet packages

- `Microsoft.Direct3D.D3D12`
- `Microsoft.Direct3D.DXC`
- `directxtex_desktop_win10`

### vcpkg dependency

- `assimp`

`assimp` is used for importing model formats, while the Direct3D and shader packages provide the rendering and shader compilation support used by the viewer.

## Build notes

The project is configured to use:

- project include directories rooted at `$(ProjectDir)`
- precompiled headers through `Core\pch.h`
- shader compilation from `Core\Shaders\shaders.hlsl`

In a normal Visual Studio workflow you should:

1. restore NuGet packages
2. let `vcpkg` restore the manifest dependency
3. build the solution
4. run the application and load a model from the menu

## What is currently supported

Current app features include:

- loading a 3D model from disk
- orbit/pan/zoom camera controls
- directional light editing
- point light add/remove and editing
- viewport point-light gizmo interaction
- directional shadow mapping
- point-light shadow mapping
- optional solid ground plane for shadow inspection
- normal mapping support
- transparent and double-sided mesh handling
- image export from the render menu

## Supported asset formats

The file picker currently accepts:

- `obj`
- `fbx`
- `dae`
- `gltf`
- `glb`
- `3ds`
- `stl`
- `ply`

Support ultimately depends on the import pipeline and source assets, but these are the formats currently exposed by the app UI.

## Beginner D3D12 learning focus

This project intentionally emphasizes readability over engine-scale abstraction.

Examples of beginner-friendly topics present in the codebase:

- root signature creation
- graphics PSO setup
- upload heap vs default heap usage
- constant buffer alignment and updates
- shadow-map render passes
- light-space transform setup
- point-shadow face rendering
- basic editor-style Win32 tool windows for runtime tweaking

If you are learning `D3D12`, the project is meant to be read alongside experimentation: change a value, rebuild, and see what effect it has on the final render.
