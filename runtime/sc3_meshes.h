#pragma once

class PS2Runtime;

namespace sc3
{
    // The 3D drawn natively from the offline mesh pack (scripts/sc3_mesh_pack.py) at the game's part builders
    // (SC3_NATIVE_MESHES=1, =check against the lifted VU1 programs): RECOMP_NOTES "Native meshes".
    void installNativeMeshes(PS2Runtime &runtime, bool nativeGs);
    bool nativeMeshesOn();
}
