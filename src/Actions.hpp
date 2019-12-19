#ifndef ACTIONS_HPP
#define ACTIONS_HPP

#include "Common.hpp"
#include "Exec.hpp"

// Windows.h defines CopyFile as a macro
#if defined(TUNDRA_WIN32) && defined(CopyFile)
#undef CopyFile
#endif

    struct StatCache;

    ExecResult WriteTextFile(const char* payload, const char* target_file, MemAllocHeap* heap);
    ExecResult CopyFile(const char* src_file, const char* target_file, StatCache* stat_cache, MemAllocHeap* heap);

#endif
