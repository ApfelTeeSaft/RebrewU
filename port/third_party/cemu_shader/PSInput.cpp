// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. See LICENSE.txt. Extracted from Cemu LatteShader.cpp.
#include "Standalone.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
void LatteShader_CreatePSInputTable(LatteShaderPSInputTable* psInputTable, uint32* contextRegisters)
{
    // PS control
	uint32 psControl0 = contextRegisters[mmSPI_PS_IN_CONTROL_0];
	uint32 spi0_positionEnable = (psControl0 >> 8) & 1;
	uint32 spi0_positionCentroid = (psControl0 >> 9) & 1;
	cemu_assert_debug(spi0_positionCentroid == 0); // controls gl_FragCoord
	uint32 spi0_positionAddr = spi0_positionEnable ? ((psControl0 >> 10) & 0x1F) : 0xFFFFFFFF; // controls gl_FragCoord
	uint32 spi0_paramGen = (psControl0 >> 15) & 0xF; // used for gl_PointCoords
	uint32 spi0_paramGenAddr = (psControl0 >> 19) & 0x7F;
	sint32 importIndex = 0;

	//cemu_assert_debug(((psControl0>>26)&3) == 1); // BARYC_SAMPLE_CNTL
	//cemu_assert_debug((psControl0&(1 << 28)) == 0); // PERSP_GRADIENT_ENA
	//cemu_assert_debug((psControl0&(1 << 29)) == 0); // LINEAR_GRADIENT_ENA
	// if LINEAR_GRADIENT_ENA_bit is enabled, the pixel shader accesses gl_ClipSize?

	// VS/GS parameters
	uint32 numPSInputs = contextRegisters[mmSPI_PS_IN_CONTROL_0] & 0x3F;
	uint64 key = 0;

	if (spi0_positionEnable)
	{
		key += (uint64)spi0_positionAddr + 1;
	}

	// parameter gen
	if (spi0_paramGen != 0)
	{
		key += std::rotr<uint64>(spi0_paramGen, 7);
		key += std::rotr<uint64>(spi0_paramGenAddr, 3);
		psInputTable->paramGen = spi0_paramGen;
		psInputTable->paramGenGPR = spi0_paramGenAddr;
	}
	else
	{
		psInputTable->paramGen = 0;
	}

	// semantic imports from vertex shader
#ifdef CEMU_DEBUG_ASSERT
	uint8 semanticMask[256 / 8] = { 0 };
#endif
	cemu_assert_debug(numPSInputs <= GPU7_PS_MAX_INPUTS);
	numPSInputs = std::min<uint32>(numPSInputs, GPU7_PS_MAX_INPUTS);

	for (uint32 f = 0; f < numPSInputs; f++)
	{
		uint32 psInputControl = contextRegisters[mmSPI_PS_INPUT_CNTL_0 + f];
		uint32 psSemanticId = (psInputControl & 0xFF);

		uint8 defaultValue = (psInputControl>>8)&3;
		// default:
		// 0 -> 0.0 0.0 0.0 0.0
		// 1 -> 0.0 0.0 0.0 1.0
		// 2 -> 1.0 1.0 1.0 0.0
		// 3 -> 1.0 1.0 1.0 1.0
		cemu_assert_debug(defaultValue <= 1);

		uint32 uknBits = psInputControl & ~((0xFF)|(0x3<<8) | (1 << 10) | (1 << 12));
		uknBits &= ~0x800; // FLAT_SHADE
		//cemu_assert_debug(uknBits == 0);
		//cemu_assert_debug(((psInputControl >> 11) & 1) == 0); // centroid
		//cemu_assert_debug(((psInputControl >> 17) & 1) == 0); // point sprite coord
		cemu_assert_debug(psSemanticId != 0xFF);

		key += (uint64)psInputControl;
		key = std::rotl<uint64>(key, 7);
		if (f == spi0_positionAddr)
		{
			psInputTable->import[f].semanticId = LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION;
			psInputTable->import[f].isFlat = false;
			psInputTable->import[f].isNoPerspective = false;
			key += (uint64)0x33;
		}
		else
		{
#ifdef CEMU_DEBUG_ASSERT
			if (semanticMask[psSemanticId >> 3] & (1 << (psSemanticId & 7)))
			{
				cemuLog_logDebug(LogType::Force, "SemanticId already used");
			}
			semanticMask[psSemanticId >> 3] |= (1 << (psSemanticId & 7));
#endif

			psInputTable->import[f].semanticId = psSemanticId;
			psInputTable->import[f].isFlat = (psInputControl&(1 << 10)) != 0;
			psInputTable->import[f].isNoPerspective = (psInputControl&(1 << 12)) != 0;
		}
	}
	psInputTable->key = key;
	psInputTable->count = numPSInputs;
}
