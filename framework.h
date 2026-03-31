#pragma once

#include "targetver.h"
#include "Resource.h"
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
// Direct3D 12 Header Files
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXTex.h>

#include <d3dx12.h>

// Assimp Header Files
#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>     // Post processing flags
#include <assimp/GltfMaterial.h>

// C RunTime Header Files
#include <iostream>
#include <string>
#include <vector>
#include <wrl.h>
#include <shellapi.h>
#include <cmath>
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include <commdlg.h>
#include <algorithm>
#include <cfloat>
