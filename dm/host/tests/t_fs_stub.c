// t_fs 的桩：前端存档失败通知的强实现（另一个编译单元，链接时替掉 hle_fs.c 的弱实现，同 iOS 前端的做法）
// Stub for t_fs: strong implementation of the front end's save-failure notification (a separate compilation unit that replaces the weak implementation in hle_fs.c at link time, the same way the iOS front end does it)
int g_nfail, g_lasterr;
void front_save_failed(int err) { g_nfail++; g_lasterr = err; }

int g_t_lang = -1;   // 语言开关（dm_custom.c 的 lang_en）在设置文件里的值，t_fs.c 的用例改它：-1 没点过 / 0 简中 / 1 英文 | Value of the language switch (lang_en in dm_custom.c) in the settings file, changed by the cases in t_fs.c: -1 never tapped / 0 Simplified Chinese / 1 English
int dm_custom_lang_explicit(void) { return g_t_lang; }

// 装载器的模块表：h_fs_MountSave 按 main 的构建号选存档目录（dm.h dm_main_build），单测里没有模块，表为空 | the loader's module table: h_fs_MountSave picks the save directory by main's build ID (dm.h dm_main_build); the unit test has no modules, so the table is empty
#include "../dm.h"
module_t g_mod[MAX_MOD];
int g_nmod;
