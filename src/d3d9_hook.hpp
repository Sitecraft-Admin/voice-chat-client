#pragma once
#include <d3d9.h>

class D3D9Hook {
public:
    static bool install();
    static void uninstall();
    static bool is_installed();
};
