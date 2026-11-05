#pragma once
// ---------------------------------------------------------------------------
// ayther_identity_contract.h - version of the element identity contract.
//
// The identity of sprites, plane tiles and FM events
// (docs/IDENTITY_SPECIFICATION.md) is shared by every emulation core: gpgx and
// Tessera must produce the same hashes from the same machine state, so a pack
// baked against one loads on the other. This number changes only when an
// identity function changes, which invalidates published packs. It is mirrored
// by `IDENTITY_CONTRACT` in core/src/identity_kat.rs and by the `[identity]
// contract` key of the generated vectors; ayther.unit.identity_contract_kat
// pins all three.
// ---------------------------------------------------------------------------
#define AYTHER_IDENTITY_CONTRACT 1
