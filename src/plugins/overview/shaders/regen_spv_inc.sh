#!/usr/bin/env bash
# Regenerate overview_quad_spv.inc from the .frag and .vert sources by
# running glslc -O and reformatting the resulting SPIR-V into the
# six-uint32-per-line layout the .inc uses. Idempotent — run after any
# overview_quad.{frag,vert} edit.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

glslc -O overview_quad.vert -o overview_quad.vert.spv
glslc -O overview_quad.frag -o overview_quad.frag.spv
glslc -O wallpaper_blur.vert -o wallpaper_blur.vert.spv
glslc -O wallpaper_blur_ds.frag -o wallpaper_blur_ds.frag.spv
glslc -O wallpaper_blur_us.frag -o wallpaper_blur_us.frag.spv

python3 - <<'PY' > overview_quad_spv.inc
import struct

HEADER = '''/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Pre-compiled SPIR-V for OverviewEffectV2's textured-quad shaders.
// Compiled with `glslc -O` from overview_quad.{vert,frag} in this
// directory. The .inc extension is intentional: the project's
// pre-commit clang-format hook only touches cpp/h/hpp/c files, so the
// packed uint32 tables here stay six-per-line for readability.
//
// Regenerate via ./regen_spv_inc.sh in this directory.

#include <cstdint>

namespace KWin::OverviewEffectV2Shaders
{

'''

FOOTER = '\n} // namespace KWin::OverviewEffectV2Shaders\n'

def fmt(path, name):
    data = open(path, 'rb').read()
    n = len(data) // 4
    words = struct.unpack(f'<{n}I', data)
    out = [f'static const uint32_t {name}[] = {{']
    for i in range(0, n, 6):
        chunk = words[i:i+6]
        out.append('    ' + ', '.join(f'0x{w:08x}' for w in chunk) + ',')
    out.append('};')
    return '\n'.join(out)

print(HEADER + fmt('overview_quad.vert.spv', 'kVertSpv') + '\n\n'
      + fmt('overview_quad.frag.spv', 'kFragSpv') + FOOTER, end='')
PY

python3 - <<'PY' > wallpaper_blur_spv.inc
import struct

HEADER = '''/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Pre-compiled SPIR-V for OverviewEffectV2's dual-kawase wallpaper
// blur. Vertex shader is a vertex-buffer-less full-screen triangle;
// the two fragment shaders are the canonical kawase downsample and
// upsample (same pattern as src/plugins/blur). Compiled with
// `glslc -O` from wallpaper_blur.{vert,frag} in this directory.
// Regenerate via ./regen_spv_inc.sh.

#include <cstdint>

namespace KWin::OverviewEffectV2Shaders
{

'''

FOOTER = '\n} // namespace KWin::OverviewEffectV2Shaders\n'

def fmt(path, name):
    data = open(path, 'rb').read()
    n = len(data) // 4
    words = struct.unpack(f'<{n}I', data)
    out = [f'static const uint32_t {name}[] = {{']
    for i in range(0, n, 6):
        chunk = words[i:i+6]
        out.append('    ' + ', '.join(f'0x{w:08x}' for w in chunk) + ',')
    out.append('};')
    return '\n'.join(out)

print(HEADER + fmt('wallpaper_blur.vert.spv', 'kBlurVertSpv') + '\n\n'
      + fmt('wallpaper_blur_ds.frag.spv', 'kBlurDsFragSpv') + '\n\n'
      + fmt('wallpaper_blur_us.frag.spv', 'kBlurUsFragSpv') + FOOTER, end='')
PY

echo "regenerated overview_quad_spv.inc and wallpaper_blur_spv.inc"
