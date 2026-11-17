// Resolves "module+0xRVA" addresses from RDR2FrameGen.log against a PDB.
// usage: symbolize <image.dll> <pdb folder> <rva hex> [<rva hex> ...]
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: symbolize <image.dll> <pdb folder> <rva hex>...\n");
        return 2;
    }
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING);
    if (!SymInitialize(process, argv[2], FALSE)) {
        printf("SymInitialize failed %lu\n", GetLastError());
        return 1;
    }
    const DWORD64 base = SymLoadModuleEx(process, nullptr, argv[1], nullptr, 0x10000000, 0, nullptr, 0);
    if (!base) {
        printf("SymLoadModuleEx failed %lu\n", GetLastError());
        return 1;
    }
    IMAGEHLP_MODULE64 module{sizeof(IMAGEHLP_MODULE64)};
    if (SymGetModuleInfo64(process, base, &module)) {
        printf("symbols: type=%d pdb=%s lines=%d\n", module.SymType, module.LoadedPdbName, module.LineNumbers);
    }
    for (int i = 3; i < argc; ++i) {
        const DWORD64 address = base + strtoull(argv[i], nullptr, 16);
        char buffer[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
        DWORD lineDisplacement = 0;
        const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol);
        const bool haveLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line);
        printf("%s: %s+0x%llX  %s:%lu\n", argv[i], haveSymbol ? symbol->Name : "?", displacement,
               haveLine ? line.FileName : "?", haveLine ? line.LineNumber : 0);
    }
    return 0;
}
