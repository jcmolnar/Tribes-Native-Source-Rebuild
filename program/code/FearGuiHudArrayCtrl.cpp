// WASM-PORT: superseded duplicate. This file and FearHudArrayCtrl.cpp are exact twins —
// both defined FGHudArrayCtrl::onAdd (declared in FearGuiHudArrayCtrl.h). Compiling both
// is a link-time ODR violation. FearHudArrayCtrl.cpp is kept as the single definition
// (it includes the canonical header correctly); this copy is emptied. The broken include
// here was "FearGuiArrayCtrl.h" (no FGHudArrayCtrl) — that's why it was the failing twin.
