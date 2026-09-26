#include "App.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    ld::App app;
    return app.Run(instance);
}
