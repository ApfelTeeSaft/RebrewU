#pragma once
// GX2 exports not imported by Gambit's PLT. Kept callable for ABI regressions;
// imported exports continue to register through the generated address table.
struct CPUState;
void GX2SetColorMask(CPUState*);
void GX2SetPolygonOffset(CPUState*);
void GX2SetViewportReg(CPUState*);
void GX2SetScissorReg(CPUState*);
void GX2InitDepthStencilControlReg(CPUState*);
void GX2GetDepthStencilControlReg(CPUState*);
void GX2DrawEx2(CPUState*);
