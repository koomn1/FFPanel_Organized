// Offset workflow for FFPanel.
//
// sdk/Offsets.h no longer hardcodes the per-build values: the four build
// offsets are read at runtime from offsets.json (schema in
// sdk/offsets.example.json). This file is the recipe for filling that in.
//
//   1. Extract libil2cpp.so + global-metadata.dat from the APK.
//   2. Il2CppDumper.exe libil2cpp.so global-metadata.dat output/
//   3. output/dump.cs     -> class fields and their offsets (static slots)
//      output/script.json -> method RVAs (the shot-direction function)
//   4. copy sdk/offsets.example.json offsets.json and fill in:
//        OFF_PLAYERMANAGER_INSTANCE  RVA of the static slot holding PlayerManager.Instance
//        OFF_CAMERA_INSTANCE         RVA of the static slot holding the main Camera
//        OFF_LOCALPLAYER_INSTANCE    RVA of the static slot holding LocalPlayer.Instance (optional)
//        OFF_WEAPON_DIRECTION_FN     RVA of the function that writes the shot direction
//   5. Drop offsets.json next to HD-Player.exe (panel) or external.exe, run it,
//      and read the log: "Offsets unresolved: ..." names every key still at 0.
//
// Values are RVAs inside libil2cpp.so, i.e. module_base + value. If your dumper
// reports file offsets instead, convert them (offset = RVA - segment vaddr + file off
// in the other direction) before putting them here.
