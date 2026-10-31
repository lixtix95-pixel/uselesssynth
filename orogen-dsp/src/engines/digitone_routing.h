// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ChoralRoot FM-1 contributors (a fork of Felucca)
// Adapted routing table from ChoralRootFM1 firmware/src/eng_quad.c, QUAD_ALGO:
// https://github.com/Quixotic7/ChoralRootFM1, commit ec9c54fdd7a0a544e8ec82bec196db745868f057.
// Only the routing graph and carrier masks are adapted; this is not Elektron
// firmware and OROGEN does not claim to reproduce Digitone's sound/parameters.
#pragma once
#include <cstdint>

namespace orogen {

enum DigitoneOperator : std::uint8_t { OpC, OpA, OpB1, OpB2 };
struct DigitoneRouting
{
    std::uint8_t mod[4], feedback, x, y, enveloped;
};
inline constexpr DigitoneRouting kDigitoneRouting[8] = {
    {{0x6, 0, 0x8, 0}, OpA,  0x1, 0x4, 0},
    {{0x2, 0, 0x8, 0}, OpB2, 0x1, 0x4, 0},
    {{0x2, 0, 0x2, 0x2}, OpA, 0x9, 0x4, 0},
    {{0x2, 0x4, 0x8, 0}, OpB2, 0x1, 0x4, 0},
    {{0x2, 0xC, 0, 0}, OpB1, 0x1, 0x2, 0},
    {{0xA, 0, 0xA, 0}, OpA,  0x1, 0x4, 0},
    {{0x2, 0, 0x8, 0}, OpA,  0x3, 0xC, 0xE},
    {{0x2, 0, 0, 0}, OpB1,   0x9, 0x4, 0xC},
};
// Topological evaluation order of each directed acyclic modulation graph.
inline constexpr std::uint8_t kDigitoneOrder[8][4] = {
    {OpA, OpB2, OpB1, OpC}, {OpA, OpC, OpB2, OpB1},
    {OpA, OpC, OpB2, OpB1}, {OpB2, OpB1, OpA, OpC},
    {OpB1, OpB2, OpA, OpC}, {OpA, OpB2, OpC, OpB1},
    {OpA, OpC, OpB2, OpB1}, {OpA, OpC, OpB1, OpB2},
};

} // namespace orogen
