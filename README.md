# Roti3D
A fully custom DirectX9 graphics engine built to be optimized and realistic.

## Graphical features:
- Shadows
- Ambient occlusion
- Anti Aliasing (MSAA and FXAA)
- Reflections
- Light bounces
- Colored shadows / light sources
- Fully configurable graphics settings with a settings file

## How it works:
Roti3D is a mix of rasterisation and ray tracing: it bakes ray traced shadows one time and then displays it, using both your CPU and GPU for maximum speeds. For reflections, it displays a copy of the scene that is reflected to give the best looking image while still being quite cheap to do. It's insanely fast, and makes the scene looks very realistic.

Moving objects shadows and reflections are planned, but it will need custom support for best performance.




## Minimum requirements:
- Shader Model 2.0 (DirectX9) compatible GPU
- 1 GHz single core CPU
- 32 MB of available RAM
- Windows 2000 (NT 5). Support hasn't been tested on NT4/Win9x

## Recommended requirements:
- Shader Model 3.0 (DirectX9.0c) compatible GPU
- 3 GHz single core CPU / 2 GHz dual core CPU
- 96 MB of available RAM
- Windows Vista (NT 6).

The requirements aim for (native) 720p 60 FPS, and doesn't take in count the amount of time it takes to bake everything because you can pack baked lighting with your game.
