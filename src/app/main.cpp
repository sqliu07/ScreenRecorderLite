#include <windows.h>

#include "app/application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    return srl::RunApplication(instance, showCommand);
}
