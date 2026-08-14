#pragma once
#include "Inc/Includes.h"

// Defined at namespace scope but NOT initialized from GetModuleHandle at DLL-load time.
// Suspended-start injection would run those calls when the game hasn't loaded its modules yet,
// dereferencing NULL for NtHeaders and hard-faulting inside DllMain. Instead we set these in
// InitGlobals(), called from InitThread() after the launcher has confirmed the modules are loaded.
uintptr_t GA = 0;
const IMAGE_NT_HEADERS* NtHeaders = nullptr;
uintptr_t Referee = 0;

uint64_t* RetAddr = nullptr;

// Called from the DLL's init thread once the game has loaded GameAssembly and Referee.
// Returns true only if both modules are present AND GameAssembly has a readable PE header.
static bool InitGlobals() {
    GA = (uintptr_t)GetModuleHandleA("GameAssembly.dll");
    Referee = (uintptr_t)GetModuleHandleA("Referee.dll");
    if (!GA) { printf("[Campus] init: GameAssembly.dll not loaded\n"); return false; }
    if (!Referee) { printf("[Campus] init: Referee.dll not loaded\n"); return false; }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(GA);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { printf("[Campus] init: GameAssembly has bad DOS header\n"); return false; }
    NtHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(GA + dos->e_lfanew);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE) { printf("[Campus] init: GameAssembly has bad NT header\n"); NtHeaders = nullptr; return false; }
    printf("[Campus] init: GameAssembly=%p Referee=%p SizeOfImage=0x%X\n",
        (void*)GA, (void*)Referee, NtHeaders->OptionalHeader.SizeOfImage);
    return true;
}

template <typename T>
T read(void* obj, std::ptrdiff_t offset) {
	return *(T*)((std::uint8_t*)obj + offset);
}

template <typename T>
void set(void* obj, std::ptrdiff_t offset, T val) {
	*(T*)((std::uint8_t*)obj + offset) = val;
}

// Byte-for-byte identical to the original working version. Struct-relative `il2cppString->Length`
// technically reads the klass pointer's low 32 bits instead of the real length, but wcstombs_s
// stops at the null terminator regardless of its `count` arg, so short strings (URIs, log lines)
// come through correctly and no allocations blow up in practice.
//
// A "corrected" version reading length from +16 (with a 32M safety guard) was tried and broke
// Photon connect on this build — the guard was rejecting URLs and SendRequest_H got empty
// strings back, killing the ns.rec.net→gameauth rewrite. Reverted here; long response bodies
// need to be captured by reading the raw byte[] instead (see the TODO in Patches.h Receive_H).
char* ReadIl2CppString(Il2cppString* il2cppString) {

    if (il2cppString == nullptr) {
        return nullptr;
    }

    size_t length = static_cast<size_t>(il2cppString->Length);
    const size_t bufferSize = (length + 1) * sizeof(wchar_t);
    char* internalString = new char[bufferSize];
    size_t convertedChars = 0;

    errno_t result = wcstombs_s(&convertedChars, internalString, bufferSize, reinterpret_cast<wchar_t*>(il2cppString) + 0xA, length);

    if (result != 0) {
        delete[] internalString;
        return nullptr;
    }

    return internalString;
}

// Owning wrapper: reads the IL2CPP string, copies into std::string, frees the intermediate
// buffer. Use this from hot paths (SendRequest_H, log lines) — the raw ReadIl2CppString
// leaks (length+1)*2 bytes per call if the caller doesn't delete[] the return.
#include <string>
inline std::string ReadIl2CppStringOwned(Il2cppString* il2cppString) {
    char* raw = ReadIl2CppString(il2cppString);
    if (!raw) return {};
    std::string out(raw);
    delete[] raw;
    return out;
}