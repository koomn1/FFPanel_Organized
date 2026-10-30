#pragma once
#include <cstdint>
#include "Structs.h"

// Eye height above the shooter's feet, used to build the shot direction.
constexpr float kSilentEyeHeight = 1.55f;

bool installSilentHook(uintptr_t moduleBase, uintptr_t shootDirectionRva);

// Restores the original bytes and drops the trampoline. Called by the panic key
// so nothing of ours stays patched in the game's code section.
void uninstallSilentHook();

// Aim point fed to the hook. The hook rewrites the outgoing shot direction to
// normalize(targetWorld - originWorld). `originWorld` is the shooter's eye
// position, `targetWorld` the aim point in world space. Pass valid=false to
// leave the game's own direction untouched.
void setSilentTarget(const Vec3& targetWorld, const Vec3& originWorld, bool valid);
