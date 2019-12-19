#include "Actions.hpp"
#include <string.h>

ExecResult WriteTextFile(const char* payload, const char* target_file, MemAllocHeap* heap)
{
    ExecResult result;
    char tmpBuffer[1024];

    memset(&result, 0, sizeof(result));

    FILE* f = fopen(target_file, "wb");
    if (!f)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);

        snprintf(tmpBuffer, sizeof(tmpBuffer), "Error opening for writing the file: %s, error: %s", target_file, strerror( errno ));
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

        result.m_ReturnCode = 1;
        return result;
    }
    int length = strlen(payload);
    int written = fwrite(payload, sizeof(char), length, f);
    fclose(f);

    if (written == length)
    return result;

    InitOutputBuffer(&result.m_OutputBuffer, heap);

    snprintf(tmpBuffer, sizeof(tmpBuffer), "fwrite was supposed to write %d bytes to %s, but wrote %d bytes", length, target_file, written);
    EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

    result.m_ReturnCode = 1;
    return result;
}

#if !defined(TUNDRA_APPLE) && !defined(TUNDRA_WIN32)

ExecResult CopyFile(const char* src_file, const char* target_file, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));

    const char notImplMsg[] = "CopyFile is not implemented yet.";

    result.m_ReturnCode = -1;
    InitOutputBuffer(&result.m_OutputBuffer, heap);
    EmitOutputBytesToDestination(&result, notImplMsg, strlen(notImplMsg));

    return result;
}

#endif
