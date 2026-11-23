// EliVier Explorer - FFmpeg's colour tags turned into the player's own ColourInfo.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/ffmpeg_colour.hpp"

namespace hui::video
{

ColourInfo colour_info(AVColorTransferCharacteristic transfer, AVColorSpace space,
                       AVColorPrimaries primaries, AVColorRange range, int height)
{
    ColourInfo colour;
    switch (transfer)
    {
    case AVCOL_TRC_SMPTE2084:
        colour.transfer = Transfer::pq;
        break;
    case AVCOL_TRC_ARIB_STD_B67:
        colour.transfer = Transfer::hlg;
        break;
    default:
        break;
    }
    switch (space)
    {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        colour.matrix = Matrix::bt2020;
        break;
    case AVCOL_SPC_BT709:
        colour.matrix = Matrix::bt709;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        colour.matrix = Matrix::bt601;
        break;
    default:
        if (primaries == AVCOL_PRI_BT2020 || colour.transfer != Transfer::sdr)
            colour.matrix = Matrix::bt2020;
        else
            colour.matrix = height < 720 ? Matrix::bt601 : Matrix::bt709;
        break;
    }
    colour.range = range == AVCOL_RANGE_JPEG ? Range::full : Range::limited;
    return colour;
}

} // namespace hui::video
