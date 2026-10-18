// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Cemu cc6fea125a5d4f559c662d13c5f4af0b6dd0876f (LatteAddrLib.cpp).
// Standalone integration modifications: Splatoon native port contributors.
// Only the helpers LatteAddrLib_Coord.cpp needs for detiling.
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include <algorithm>

using namespace Latte;

namespace LatteAddrLib
{
	uint32 ComputeSurfaceRotationFromTileMode(E_HWTILEMODE tileMode)
	{
		switch (tileMode)
		{
		case E_HWTILEMODE::TM_2D_TILED_THIN1:
		case E_HWTILEMODE::TM_2D_TILED_THIN2:
		case E_HWTILEMODE::TM_2D_TILED_THIN4:
		case E_HWTILEMODE::TM_2D_TILED_THICK:
		case E_HWTILEMODE::TM_2B_TILED_THIN1:
		case E_HWTILEMODE::TM_2B_TILED_THIN2:
		case E_HWTILEMODE::TM_2B_TILED_THIN4:
		case E_HWTILEMODE::TM_2B_TILED_THICK:
			return m_pipes * ((m_banks >> 1) - 1);
		case E_HWTILEMODE::TM_3D_TILED_THIN1:
		case E_HWTILEMODE::TM_3D_TILED_THICK:
		case E_HWTILEMODE::TM_3B_TILED_THIN1:
		case E_HWTILEMODE::TM_3B_TILED_THICK:
			if (m_pipes >= 4)
				return (m_pipes >> 1) - 1;
			return 1;
		default:
			break;
		}
		return 0;
	}

	uint32 _ComputeMacroTileAspectRatio(E_HWTILEMODE tileMode)
	{
		switch (tileMode)
		{
		case E_HWTILEMODE::TM_2B_TILED_THIN1:
		case E_HWTILEMODE::TM_3D_TILED_THIN1:
		case E_HWTILEMODE::TM_3B_TILED_THIN1:
			return 1;
		case E_HWTILEMODE::TM_2D_TILED_THIN2:
		case E_HWTILEMODE::TM_2B_TILED_THIN2:
			return 2;
		case E_HWTILEMODE::TM_2D_TILED_THIN4:
		case E_HWTILEMODE::TM_2B_TILED_THIN4:
			return 4;
		default:
			break;
		}
		return 1;
	}

	uint32 ComputeSurfaceBankSwappedWidth(E_HWTILEMODE tileMode, uint32 bpp, uint32 numSamples, uint32 pitch)
	{
		uint32 bankSwapWidth = 0;
		uint32 slicesPerTile = 1;
		uint32 bytesPerSample = 8 * bpp;
		uint32 samplesPerTile = m_splitSize / bytesPerSample;
		if (m_splitSize / bytesPerSample)
		{
			slicesPerTile = numSamples / samplesPerTile;
			if (!(numSamples / samplesPerTile))
				slicesPerTile = 1;
		}
		if (TM_IsThickAndMacroTiled(tileMode) == 1)
			numSamples = 4;
		if (TM_IsBankSwapped(tileMode))
		{
			uint32 bytesPerTileSlice = numSamples * bytesPerSample / slicesPerTile;
			uint32 aspectRatioFactor = _ComputeMacroTileAspectRatio(tileMode);
			uint32 swapTiles = std::max<uint32>((m_swapSize >> 1) / bpp, 1);
			uint32 swapWidth = swapTiles * 8 * m_banks;
			uint32 heightBytes = numSamples * aspectRatioFactor * m_pipes * bpp / slicesPerTile;
			uint32 swapMax = m_pipes * m_banks * m_rowSize / heightBytes;
			uint32 swapMin = m_pipeInterleaveBytes * 8 * m_banks / bytesPerTileSlice;
			uint32 swapVal;
			if (swapMax >= swapWidth)
				swapVal = std::max(swapWidth, swapMin);
			else
				swapVal = swapMax;
			for (bankSwapWidth = swapVal; bankSwapWidth >= 2 * pitch; bankSwapWidth >>= 1);
		}
		return bankSwapWidth;
	}
}
