#pragma once
struct CPUState;
// nn_save path operations share coreinit's FS handle namespace. The game
// consumes SAVEOpenFile handles through FSReadFile/FSWriteFile/FSCloseFile.
void coreinit_save_init(CPUState*);
void coreinit_save_init_dir(CPUState*);
void coreinit_save_open_file(CPUState*);
void coreinit_save_flush_quota(CPUState*);
void coreinit_save_make_dir(CPUState*);
void coreinit_save_get_stat(CPUState*);
