# Roti3D
A fully custom DirectX9 graphics engine built to be optimized and realistic.

## Graphical features:
- Shadows
- Ambient occlusion
- Anti Aliasing (MSAA / FXAA)
- Reflections
- Path traced light bounces
- Colored shadows / light sources
- Fully configurable graphics settings with an advanced menu


<img width="246" height="309" alt="colored bouncing light2" src="https://github.com/user-attachments/assets/adcc710a-5f92-413f-b99e-9ff88680e947" /> <img width="434" height="156" alt="colored bouncing light_corrected" src="https://github.com/user-attachments/assets/4d0c604c-52b7-4c96-ba09-4f29fb3329fb" />

<img width="1263" height="481" alt="lotsofobjects" src="https://github.com/user-attachments/assets/2eff8c4c-ce99-4571-807a-feb4fd66d942" />
<img width="716" height="708" alt="PTLighting_1" src="https://github.com/user-attachments/assets/d3015369-12ff-42e4-bed4-f5eb0802e2bc" />


Screenshots made with stock settings, no color grading.

Tested on:

A GTX 970, Windows 7: 9500-11000 FPS (windows limited)

A GTX 750 Ti, Windows 10 21H2: 4600-5000 FPS (windows limited)

An FX 4800 [~GTX 260], Windows 10 21H2: 3600-4100 FPS

A GMA 945 iGPU + Atom N270: Windows XP SP3: 38-45 FPS


## How it works:
Roti3D is a mix of rasterisation and ray/path tracing: it bakes ray traced shadows one time and then displays it, using both your CPU and GPU for maximum speeds. For reflections, it displays a "copy" of the scene that is "reflected" to give the best looking image while still being quite cheap to do. It's insanely fast, and makes the scene looks very realistic.

Moving objects shadows and reflections are planned, but it will need custom support for best performance.




## Minimum requirements:
- Shader Model 2.0 (DirectX9) compatible GPU
- 1 GHz single core CPU
- 48 MB of available RAM*
- 32 MB of VRAM
- Windows 2000 (NT 5). Support hasn't been tested on NT4/Win9x

## Recommended requirements:
- Shader Model 3.0 (DirectX9.0c) compatible GPU
- 3 GHz single core CPU / 2 GHz dual core CPU
- 96 MB of available RAM*
- 128 MB of VRAM
- Windows Vista (NT 6).

The requirements aim for (native) 720p 60 FPS, and doesn't take in count the amount of time it takes to bake everything because you can pack baked lighting with your game.

*: Depends on the ram allocation policy of the OS. Windows 10 [NT10] allocates way more RAM than Windows 2000 [NT5]
