
#ifdef _WIN32
#include <Windows.h>

//TODO: let claude opus create a CABI bridge so I can finally release the source and others can make modules
//Let it handle the abstraction and force it to use stable CABI interfaces only which dont expose my internal data structures

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif