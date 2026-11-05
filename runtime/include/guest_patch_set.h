#pragma once

// Name of the static guest patch set the translator baked into this game build
// (Translator.Cli --guest-patches, see translator/src/Translator.Core/Loading/GuestPatchSet.cs).
// Empty for the unmodified game. Defined per game in runtime/game/runtime_config_values.cpp so
// the runtime itself stays game-independent.
namespace GuestPatchSet {
extern const char* const kName;
} // namespace GuestPatchSet
