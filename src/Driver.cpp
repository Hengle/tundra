#include "Driver.hpp"
#include "BinaryWriter.hpp"
#include "Buffer.hpp"
#include "BuildQueue.hpp"
#include "Common.hpp"
#include "DagData.hpp"
#include "DagGenerator.hpp"
#include "FileInfo.hpp"
#include "MemAllocLinear.hpp"
#include "MemoryMappedFile.hpp"
#include "RuntimeNode.hpp"
#include "ScanData.hpp"
#include "Scanner.hpp"
#include "SortedArrayUtil.hpp"
#include "AllBuiltNodes.hpp"
#include "Stats.hpp"
#include "HashTable.hpp"
#include "Hash.hpp"
#include "Profiler.hpp"
#include "NodeResultPrinting.hpp"
#include "FileSign.hpp"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <sstream>

#if ENABLED(TUNDRA_CASE_INSENSITIVE_FILESYSTEM)
#if defined(_MSC_VER) || defined(TUNDRA_WIN32_MINGW)
#define PathCompareN _strnicmp
#define PathCompare _stricmp
#else
#define PathCompareN strncasecmp
#define PathCompare strcasecmp
#endif
#else
#define PathCompareN strncmp
#define PathCompare strcmp
#endif

#if defined(TUNDRA_WIN32)
#define strncasecmp _strnicmp
#endif



TundraStats g_Stats;

static const char *s_BuildFile;
static const char *s_DagFileName;

static bool DriverPrepareDag(Driver *self, const char *dag_fn);
static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength);

void DriverInitializeTundraFilePaths(DriverOptions *driverOptions)
{
    s_BuildFile = "tundra.lua";
    s_DagFileName = driverOptions->m_DAGFileName;
}

// Set default options.
void DriverOptionsInit(DriverOptions *self)
{
    self->m_ShowHelp = false;
    self->m_ForceDagRegen = false;
    self->m_ShowTargets = false;
    self->m_DebugMessages = false;
    self->m_Verbose = false;
    self->m_SpammyVerbose = false;
    self->m_DisplayStats = false;
    self->m_GenDagOnly = false;
    self->m_Quiet = false;
    self->m_SilenceIfPossible = false;
    self->m_Clean = false;
    self->m_Rebuild = false;
    self->m_IdeGen = false;
    self->m_DebugSigning = false;
    self->m_ContinueOnError = false;
    self->m_ThrottleOnHumanActivity = false;
    self->m_ThrottleInactivityPeriod = 30;
    self->m_ThrottledThreadsAmount = 0;
    self->m_ThreadCount = GetCpuCount();
    self->m_WorkingDir = nullptr;
    self->m_DAGFileName = ".tundra2.dag";
    self->m_ProfileOutput = nullptr;
    self->m_IncludesOutput = nullptr;
#if defined(TUNDRA_WIN32)
    self->m_RunUnprotected = false;
#endif
}

// Helper routine to load frozen data into RAM via memory mapping
template <typename FrozenType>
static bool LoadFrozenData(const char *fn, MemoryMappedFile *result, const FrozenType **ptr)
{
    MemoryMappedFile mapping;

    MmapFileInit(&mapping);

    MmapFileMap(&mapping, fn);

    if (MmapFileValid(&mapping))
    {
        char *mmap_buffer = static_cast<char *>(mapping.m_Address);
        const FrozenType *data = reinterpret_cast<const FrozenType *>(mmap_buffer);

        Log(kDebug, "%s: successfully mapped at %p (%d bytes)", fn, data, (int)mapping.m_Size);

        // Check size
        if (mapping.m_Size < sizeof(FrozenType))
        {
            Log(kWarning, "%s: Bad mmap size %d - need at least %d bytes",
                fn, (int)mapping.m_Size, (int)sizeof(FrozenType));
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumber != FrozenType::MagicNumber)
        {
            Log(kDebug, "%s: Bad magic number %08x - current is %08x",
                fn, data->m_MagicNumber, FrozenType::MagicNumber);
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumberEnd != FrozenType::MagicNumber)
        {
            Log(kError, "Did not find expected magic number marker at the end of %s. This most likely means data writing code for that file is writing too much or too little data", fn);
            goto error;
        }

        // Move ownership of memory mapping to member variable.
        *result = mapping;

        *ptr = data;

        return true;
    }

    Log(kDebug, "%s: mmap failed", fn);

error:
    MmapFileDestroy(&mapping);
    return false;
}

void DriverShowTargets(Driver *self)
{
    const Frozen::Dag *dag = self->m_DagData;

    printf("\nNamed nodes and aliases:\n");
    printf("----------------------------------------------------------------\n");

    int32_t count = dag->m_NamedNodes.GetCount();
    const char **temp = (const char **)alloca(sizeof(const char *) * count);
    for (int i = 0; i < count; ++i)
    {
        temp[i] = dag->m_NamedNodes[i].m_Name.Get();
    }
    std::sort(temp, temp + count, [](const char *a, const char *b) { return strcmp(a, b) < 0; });

    for (int i = 0; i < count; ++i)
    {
        printf(" - %s\n", temp[i]);
    }
}

static void GetIncludesRecursive(const HashDigest &scannerGuid, const char *fn, uint32_t fnHash, const Frozen::ScanData *scan_data, int depth, HashTable<HashDigest, kFlagPathStrings> &seen, HashSet<kFlagPathStrings> &direct)
{
    if (depth == 0 && !HashSetLookup(&direct, fnHash, fn))
        HashSetInsert(&direct, fnHash, fn);

    if (HashTableLookup(&seen, fnHash, fn))
        return;
    HashTableInsert(&seen, fnHash, fn, scannerGuid);

    HashDigest scan_key;
    ComputeScanCacheKey(&scan_key, fn, scannerGuid);

    const int32_t count = scan_data->m_EntryCount;
    if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
    {
        int index = int(ptr - scan_data->m_Keys.Get());
        const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
        int file_count = entry->m_IncludedFiles.GetCount();
        for (int i = 0; i < file_count; ++i)
        {
            GetIncludesRecursive(scannerGuid, entry->m_IncludedFiles[i].m_Filename.Get(), entry->m_IncludedFiles[i].m_FilenameHash, scan_data, depth + 1, seen, direct);
        }
    }
}

bool DriverReportIncludes(Driver *self)
{
    MemAllocLinearScope allocScope(&self->m_Allocator);

    const Frozen::Dag *dag = self->m_DagData;
    if (dag == nullptr)
    {
        Log(kError, "No build DAG data");
        return false;
    }

    const Frozen::ScanData *scan_data = self->m_ScanData;
    if (scan_data == nullptr)
    {
        Log(kError, "No build file scan data (there was no previous build done?)");
        return false;
    }

    // For each file, we have to remember which include scanner hash digest was used.
    HashTable<HashDigest, kFlagPathStrings> seen;
    HashTableInit(&seen, &self->m_Heap);
    // Which files were directly compiled in DAG? all others are included indirectly.
    HashSet<kFlagPathStrings> direct;
    HashSetInit(&direct, &self->m_Heap);

    // Crawl the DAG and include scanner data to find all direct and indirect files.
    int node_count = dag->m_NodeCount;
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode &node = dag->m_NodeData[i];

        const Frozen::ScannerData *s = node.m_Scanner;
        if (s != nullptr && node.m_InputFiles.GetCount() > 0)
        {
            const char *fn = node.m_InputFiles[0].m_Filename.Get();
            uint32_t fnHash = node.m_InputFiles[0].m_FilenameHash;
            GetIncludesRecursive(s->m_ScannerGuid, fn, fnHash, scan_data, 0, seen, direct);
        }
    }

    // Create JSON structure of includes report.
    JsonWriter msg;
    JsonWriteInit(&msg, &self->m_Allocator);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "dagFile");
    JsonWriteValueString(&msg, self->m_Options.m_DAGFileName);

    JsonWriteKeyName(&msg, "files");
    JsonWriteStartArray(&msg);
    JsonWriteNewline(&msg);

    HashTableWalk(&seen, [&](uint32_t index, uint32_t hash, const char *filename, const HashDigest &scannerguid) {
        HashDigest scan_key;
        ComputeScanCacheKey(&scan_key, filename, scannerguid);
        const int32_t count = scan_data->m_EntryCount;
        if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
        {
            int index = int(ptr - scan_data->m_Keys.Get());
            const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
            int file_count = entry->m_IncludedFiles.GetCount();
            JsonWriteStartObject(&msg);
            JsonWriteKeyName(&msg, "file");
            JsonWriteValueString(&msg, filename);
            if (HashSetLookup(&direct, hash, filename))
            {
                JsonWriteKeyName(&msg, "direct");
                JsonWriteValueInteger(&msg, 1);
            }
            JsonWriteKeyName(&msg, "includes");
            JsonWriteStartArray(&msg);
            JsonWriteNewline(&msg);
            for (int i = 0; i < file_count; ++i)
            {
                const char *fn = entry->m_IncludedFiles[i].m_Filename.Get();
                JsonWriteValueString(&msg, fn);
                JsonWriteNewline(&msg);
            }
            JsonWriteEndArray(&msg);
            JsonWriteEndObject(&msg);
        }
    });

    JsonWriteEndArray(&msg);
    JsonWriteEndObject(&msg);

    // Write into file.
    FILE *f = fopen(self->m_Options.m_IncludesOutput, "w");
    if (!f)
    {
        Log(kError, "Failed to create includes report file '%s'", self->m_Options.m_IncludesOutput);
        return false;
    }
    JsonWriteToFile(&msg, f);
    fclose(f);

    HashTableDestroy(&seen);
    HashSetDestroy(&direct);

    return true;
}

void DriverReportStartup(Driver *self, const char **targets, int target_count)
{
    MemAllocLinearScope allocScope(&self->m_Allocator);

    JsonWriter msg;
    JsonWriteInit(&msg, &self->m_Allocator);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "init");

    JsonWriteKeyName(&msg, "dagFile");
    JsonWriteValueString(&msg, self->m_Options.m_DAGFileName);

    JsonWriteKeyName(&msg, "targets");
    JsonWriteStartArray(&msg);
    for (int i = 0; i < target_count; ++i)
        JsonWriteValueString(&msg, targets[i]);
    JsonWriteEndArray(&msg);

    JsonWriteEndObject(&msg);

    LogStructured(&msg);
}

bool DriverInitData(Driver *self)
{
    if (!DriverPrepareDag(self, s_DagFileName))
        return false;

    ProfilerScope prof_scope("DriverInitData", 0);
    // do not produce/overwrite structured log output file,
    // if we're only reporting something and not doing an actual build
    if (self->m_Options.m_IncludesOutput == nullptr && !self->m_Options.m_ShowHelp && !self->m_Options.m_ShowTargets)
        SetStructuredLogFileName(self->m_DagData->m_StructuredLogFileName);

    DigestCacheInit(&self->m_DigestCache, MB(128), self->m_DagData->m_DigestCacheFileName);

    LoadFrozenData<Frozen::AllBuiltNodes>(self->m_DagData->m_StateFileName, &self->m_StateFile, &self->m_StateData);

    LoadFrozenData<Frozen::ScanData>(self->m_DagData->m_ScanCacheFileName, &self->m_ScanFile, &self->m_ScanData);

    ScanCacheSetCache(&self->m_ScanCache, self->m_ScanData);

    return true;
}

static bool DriverPrepareDag(Driver *self, const char *dag_fn)
{
    const int out_of_date_reason_length = 500;
    char out_of_date_reason[out_of_date_reason_length + 1];

    snprintf(out_of_date_reason, out_of_date_reason_length, "Build frontend of %s ran (unknown reason)", dag_fn);

    if (self->m_Options.m_ForceDagRegen)
        snprintf(out_of_date_reason, out_of_date_reason_length, "Build frontend of %s ran (ForceDagRegen option used)", dag_fn);

    bool loadFrozenDataResult = LoadFrozenData<Frozen::Dag>(dag_fn, &self->m_DagFile, &self->m_DagData);

    if (!loadFrozenDataResult)
        snprintf(out_of_date_reason, out_of_date_reason_length, "Build frontend of %s ran (no suitable previous build dag file)", dag_fn);

    if (loadFrozenDataResult)
    {
        if (self->m_DagData->m_ForceDagRebuild == 1)
            snprintf(out_of_date_reason, out_of_date_reason_length, "Build frontend of %s ran (previous dag file indicated it should not be reused)", dag_fn);
    }

    if (loadFrozenDataResult && self->m_Options.m_IncludesOutput != nullptr)
    {
        Log(kDebug, "Only showing includes; using existing DAG without out-of-date checks");
        return true;
    }

    // Try to use an existing DAG
    if (!self->m_Options.m_ForceDagRegen && loadFrozenDataResult && self->m_DagData->m_ForceDagRebuild == 0)
    {
        uint64_t time_exec_started = TimerGet();
        bool checkResult;
        {
            ProfilerScope prof_scope("DriverCheckDagSignatures", 0);
            checkResult = DriverCheckDagSignatures(self, out_of_date_reason, out_of_date_reason_length);
        }
        uint64_t now = TimerGet();
        double duration = TimerDiffSeconds(time_exec_started, now);
        if (duration > 1)
            PrintNonNodeActionResult(duration, self->m_DagData->m_NodeCount, MessageStatusLevel::Warning, "Calculating file and glob signatures. (unusually slow)");

        if (checkResult)
        {
            Log(kDebug, "DAG signatures match - using existing data w/o build frontend invocation");
            return true;
        }
    }

    if (loadFrozenDataResult)
        MmapFileUnmap(&self->m_DagFile);

    uint64_t time_exec_started = TimerGet();
    // We need to generate the DAG data
    {
        ProfilerScope prof_scope("RunFrontend", 0);
        if (!GenerateDag(s_BuildFile, dag_fn))
            return false;
    }
    PrintNonNodeActionResult(TimerDiffSeconds(time_exec_started, TimerGet()), 1, MessageStatusLevel::Success, out_of_date_reason);

    // The DAG had better map in now, or we can give up.
    if (!LoadFrozenData<Frozen::Dag>(dag_fn, &self->m_DagFile, &self->m_DagData))
    {
        Log(kError, "panic: couldn't load in freshly generated DAG");
        return false;
    }

    // In checked builds, make sure signatures are valid.
    // For Unity, our frontend is so slow, that we'll happily take a tiny extra hit to ensure our signatures are correct.
    if (!DriverCheckDagSignatures(self, out_of_date_reason, out_of_date_reason_length))
    {
        printf("Abort: rerunning DriverCheckDagSignatures() in PrepareDag() caused it to fail because: %s", out_of_date_reason);
        exit(1);
    }

    return true;
}

static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength)
{
    const Frozen::Dag *dag_data = self->m_DagData;

#if ENABLED(CHECKED_BUILD)
    // Paranoia - make sure the data is sorted.
    for (int i = 1, count = dag_data->m_NodeCount; i < count; ++i)
    {
        if (dag_data->m_NodeGuids[i] < dag_data->m_NodeGuids[i - 1])
            Croak("DAG data is not sorted by guid");
    }
#endif

    Log(kDebug, "checking file signatures for DAG data");

    // Check timestamps of frontend files used to produce the DAG
    for (const Frozen::DagFileSignature &sig : dag_data->m_FileSignatures)
    {
        const char *path = sig.m_Path;

        uint64_t timestamp = sig.m_Timestamp;
        FileInfo info = GetFileInfo(path);

        if (info.m_Timestamp != timestamp)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "Build frontend of %s ran (build file timestamp changed: %s)", s_DagFileName, sig.m_Path.Get());
            Log(kInfo, "DAG out of date: timestamp change for %s. was: %lu now: %lu", path, timestamp, info.m_Timestamp);
            return false;
        }
    }

    // Check directory listing fingerprints
    // Note that the digest computation in here must match the one in LuaListDirectory
    // The digests computed there are stored in the signature block by frontend code.
    for (const Frozen::DagGlobSignature &sig : dag_data->m_GlobSignatures)
    {
        HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, &self->m_Heap, &self->m_Allocator);

        // Compare digest with the one stored in the signature block
        if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
        {
            char stored[kDigestStringSize], actual[kDigestStringSize];
            DigestToString(stored, sig.m_Digest);
            DigestToString(actual, digest);
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "Build frontend of %s ran (folder contents changed: %s)", s_DagFileName, sig.m_Path.Get());
            Log(kInfo, "DAG out of date: file glob change for %s (%s => %s)", sig.m_Path.Get(), stored, actual);
            return false;
        }
    }

    return true;
}

static int LevenshteinDistanceNoCase(const char *s, const char *t)
{
    int n = (int)strlen(s);
    int m = (int)strlen(t);

    if (n == 0)
        return m;
    if (m == 0)
        return n;

    int xSize = n + 1;
    int ySize = m + 1;
    int *d = (int *)alloca(xSize * ySize * sizeof(int));

    for (int x = 0; x <= n; x++)
        d[ySize * x] = x;
    for (int y = 0; y <= m; y++)
        d[y] = y;

    for (int y = 1; y <= m; y++)
    {
        for (int x = 1; x <= n; x++)
        {
            if (tolower(s[x - 1]) == tolower(t[y - 1]))        // Case insensitive
                d[ySize * x + y] = d[ySize * (x - 1) + y - 1]; // no operation
            else
                d[ySize * x + y] = std::min(std::min(
                                                d[ySize * (x - 1) + y] + 1, // a deletion
                                                d[ySize * x + y - 1] + 1),  // an insertion
                                            d[ySize * (x - 1) + y - 1] + 1  // a substitution
                );
        }
    }
    return d[ySize * n + m];
}

//searching in inputs prevents useful single object builds, as the requested object gets found as an input of the linker
#define SUPPORT_SEARCHING_IN_INPUTS 0

// Match their source files and output files against the names specified.
static void FindNodesByName(
    const Frozen::Dag *dag,
    Buffer<int32_t> *out_nodes,
    MemAllocHeap *heap,
    const char **names,
    size_t name_count,
    const FrozenArray<Frozen::NamedNodeData> &named_nodes)
{
    size_t node_bits_size = (dag->m_NodeCount + 31) / 32 * sizeof(uint32_t);
    uint32_t *node_bits = (uint32_t *)alloca(node_bits_size);

    memset(node_bits, 0, node_bits_size);

    for (size_t name_i = 0; name_i < name_count; ++name_i)
    {
        const char *name = names[name_i];

        bool found = false;

        // Try all named nodes first
        bool foundMatchingPrefix = false;
        bool prefixIsAmbigious = false;
        const Frozen::NamedNodeData *nodeDataForMatchingPrefix = nullptr;
        struct StringWithScore
        {
            StringWithScore(int _score, const char *_string) : score(_score), string(_string) {}
            int score;
            const char *string;
        };
        std::vector<StringWithScore> fuzzyMatches;
        fuzzyMatches.reserve(named_nodes.GetCount());
        for (const Frozen::NamedNodeData &named_node : named_nodes)
        {
            const int distance = LevenshteinDistanceNoCase(named_node.m_Name, name);
            const int fuzzyMatchLimit = std::max(0, std::min((int)strlen(name) - 2, 4));
            bool isFuzzyMatch = distance <= fuzzyMatchLimit;

            // Exact match?
            if (distance == 0)
            {
                if (strcmp(named_node.m_Name, name) != 0)
                    Log(kInfo, "found case insensitive match for %s, mapping to %s", name, named_node.m_Name.Get());

                BufferAppendOne(out_nodes, heap, named_node.m_NodeIndex);
                Log(kDebug, "mapped %s to node %d", name, named_node.m_NodeIndex);
                found = true;
                break;
            }
            // Fuzzy match?
            else if (isFuzzyMatch)
                fuzzyMatches.emplace_back(distance, named_node.m_Name.Get());
            // Prefix match?
            if (strncasecmp(named_node.m_Name, name, strlen(name)) == 0)
            {
                prefixIsAmbigious = foundMatchingPrefix;
                if (!foundMatchingPrefix)
                {
                    foundMatchingPrefix = true;
                    nodeDataForMatchingPrefix = &named_node;
                }
                if (!isFuzzyMatch)
                    fuzzyMatches.emplace_back(strlen(named_node.m_Name) - strlen(name), named_node.m_Name.Get());
            }
        }

        // If the given name is an unambigious prefix of one of our named nodes, we go with it, but warn the user.
        if (!found && foundMatchingPrefix && !prefixIsAmbigious)
        {
            Log(kWarning, "autocompleting %s to %s", name, nodeDataForMatchingPrefix->m_Name.Get());
            BufferAppendOne(out_nodes, heap, nodeDataForMatchingPrefix->m_NodeIndex);
            found = true;
        }

        if (found)
            continue;

        //since outputs in the dag are "cleaned paths", with forward slashes converted to backward ones,
        //make sure we convert our searchstring in the same way
        PathBuffer pathbuf;
        PathInit(&pathbuf, name);
        char cleaned_path[kMaxPathLength];
        PathFormat(cleaned_path, &pathbuf);

        const uint32_t filename_hash = Djb2HashPath(cleaned_path);
        for (int node_index = 0; node_index != dag->m_NodeCount; node_index++)
        {
            const Frozen::DagNode &node = dag->m_NodeData[node_index];
            for (const FrozenFileAndHash &output : node.m_OutputFiles)
            {
                if (filename_hash == output.m_FilenameHash && 0 == PathCompare(output.m_Filename, cleaned_path))
                {
                    BufferAppendOne(out_nodes, heap, node_index);
                    Log(kDebug, "mapped %s to node %d (based on output file)", name, node_index);
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            std::stringstream errorOutput;
            errorOutput << "unable to map " << name << " to any named node or input/output file";
            if (!fuzzyMatches.empty())
            {
                std::sort(fuzzyMatches.begin(), fuzzyMatches.end(), [](const StringWithScore &a, const StringWithScore &b) { return a.score < b.score; });
                errorOutput << "\nmaybe you meant:\n";
                for (int i = 0; i < fuzzyMatches.size() - 1; ++i)
                    errorOutput << "- " << fuzzyMatches[i].string << "\n";
                errorOutput << "- " << fuzzyMatches[fuzzyMatches.size() - 1].string;
            }
            Croak(errorOutput.str().c_str());
        }
    }
}

static void DriverSelectNodes(const Frozen::Dag *dag, const char **targets, int target_count, Buffer<int32_t> *out_nodes, MemAllocHeap *heap)
{
    if (target_count > 0)
    {
        FindNodesByName(
            dag,
            out_nodes, heap,
            targets, target_count, dag->m_NamedNodes);
    }
    else
    {
        BufferAppend(out_nodes, heap, dag->m_DefaultNodes.GetArray(), dag->m_DefaultNodes.GetCount());
    }

    std::sort(out_nodes->begin(), out_nodes->end());
    int32_t *new_end = std::unique(out_nodes->begin(), out_nodes->end());
    out_nodes->m_Size = new_end - out_nodes->begin();
    Log(kDebug, "Node selection finished with %d nodes to build", (int)out_nodes->m_Size);
}

bool DriverPrepareNodes(Driver *self, const char **targets, int target_count)
{
    ProfilerScope prof_scope("Tundra PrepareNodes", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::DagNode *src_nodes = dag->m_NodeData;
    const HashDigest *src_guids = dag->m_NodeGuids;
    MemAllocHeap *heap = &self->m_Heap;

    Buffer<int32_t> node_stack;
    BufferInitWithCapacity(&node_stack, heap, 1024);

    DriverSelectNodes(dag, targets, target_count, &node_stack, heap);

    const size_t node_word_count = (dag->m_NodeCount + 31) / 32;
    uint32_t *node_visited_bits = HeapAllocateArrayZeroed<uint32_t>(heap, node_word_count);

    int node_count = 0;

    Buffer<int32_t> node_indices;
    BufferInitWithCapacity(&node_indices, heap, 1024);

    while (node_stack.m_Size > 0)
    {
        int dag_index = BufferPopOne(&node_stack);
        const int dag_word = dag_index / 32;
        const int dag_bit = 1 << (dag_index & 31);

        if (0 == (node_visited_bits[dag_word] & dag_bit))
        {
            const Frozen::DagNode *node = src_nodes + dag_index;

            BufferAppendOne(&node_indices, &self->m_Heap, dag_index);

            node_visited_bits[dag_word] |= dag_bit;

            // Update counts
            ++node_count;

            // Stash node dependencies on the work queue to keep iterating
            BufferAppend(&node_stack, &self->m_Heap, node->m_Dependencies.GetArray(), node->m_Dependencies.GetCount());
        }
    }

    HeapFree(heap, node_visited_bits);
    node_visited_bits = nullptr;

    // Allocate space for nodes
    RuntimeNode *out_nodes = BufferAllocZero(&self->m_Nodes, &self->m_Heap, node_count);

    // Initialize node state
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode *src_node = src_nodes + node_indices[i];
        out_nodes[i].m_DagNode = src_node;
#if ENABLED(CHECKED_BUILD)
        out_nodes[i].m_DebugAnnotation = src_node->m_Annotation.Get();
#endif
    }

    // Find frozen node state from previous build, if present.
    if (const Frozen::AllBuiltNodes *state_data = self->m_StateData)
    {
        const Frozen::BuiltNode *frozen_states = state_data->m_NodeStates;
        const HashDigest *state_guids = state_data->m_NodeGuids;
        const int state_guid_count = state_data->m_NodeCount;

        for (int i = 0; i < node_count; ++i)
        {
            const HashDigest *src_guid = src_guids + node_indices[i];

            if (const HashDigest *old_guid = BinarySearch(state_guids, state_guid_count, *src_guid))
            {
                int state_index = int(old_guid - state_guids);
                out_nodes[i].m_MmapState = frozen_states + state_index;
            }
        }
    }

    // initialize a remapping table from global (dag) index to local (state)
    // index. This is so we can map any DAG node reference onto any local state.
    int32_t *node_remap = BufferAllocFill(&self->m_NodeRemap, heap, dag->m_NodeCount, -1);

    CHECK(node_remap == self->m_NodeRemap.m_Storage);

    for (int local_index = 0; local_index < node_count; ++local_index)
    {
        const Frozen::DagNode *global_node = out_nodes[local_index].m_DagNode;
        const int global_index = int(global_node - src_nodes);
        CHECK(node_remap[global_index] == -1);
        node_remap[global_index] = local_index;
    }

    Log(kDebug, "Node remap: %d src nodes, %d active nodes, using %d bytes of node state buffer space",
        dag->m_NodeCount, node_count, sizeof(RuntimeNode) * node_count);

    BufferDestroy(&node_stack, &self->m_Heap);
    BufferDestroy(&node_indices, &self->m_Heap);

    return true;
}

bool DriverInit(Driver *self, const DriverOptions *options)
{
    memset(self, 0, sizeof(Driver));
    HeapInit(&self->m_Heap);
    LinearAllocInit(&self->m_Allocator, &self->m_Heap, MB(64), "Driver Linear Allocator");

    LinearAllocSetOwner(&self->m_Allocator, ThreadCurrent());

    InitNodeResultPrinting();

    MmapFileInit(&self->m_DagFile);
    MmapFileInit(&self->m_StateFile);
    MmapFileInit(&self->m_ScanFile);

    self->m_DagData = nullptr;
    self->m_StateData = nullptr;
    self->m_ScanData = nullptr;

    BufferInit(&self->m_NodeRemap);
    BufferInit(&self->m_Nodes);

    self->m_Options = *options;

    // This linear allocator is only accessed when the state cache is locked.
    LinearAllocInit(&self->m_ScanCacheAllocator, &self->m_Heap, MB(64), "scan cache");
    ScanCacheInit(&self->m_ScanCache, &self->m_Heap, &self->m_ScanCacheAllocator);

    // This linear allocator is only accessed when the state cache is locked.
    LinearAllocInit(&self->m_StatCacheAllocator, &self->m_Heap, MB(64), "stat cache");
    StatCacheInit(&self->m_StatCache, &self->m_StatCacheAllocator, &self->m_Heap);

    return true;
}

void DriverDestroy(Driver *self)
{
    DigestCacheDestroy(&self->m_DigestCache);

    StatCacheDestroy(&self->m_StatCache);

    ScanCacheDestroy(&self->m_ScanCache);

    BufferDestroy(&self->m_Nodes, &self->m_Heap);
    BufferDestroy(&self->m_NodeRemap, &self->m_Heap);

    MmapFileDestroy(&self->m_ScanFile);
    MmapFileDestroy(&self->m_StateFile);
    MmapFileDestroy(&self->m_DagFile);

    LinearAllocDestroy(&self->m_ScanCacheAllocator);
    LinearAllocDestroy(&self->m_StatCacheAllocator);
    LinearAllocDestroy(&self->m_Allocator);
    HeapDestroy(&self->m_Heap);
}

bool DriverPrepareDag(Driver *self, const char *dag_fn);
bool DriverAllocNodes(Driver *self);

BuildResult::Enum DriverBuild(Driver *self)
{
    const Frozen::Dag *dag = self->m_DagData;

    // Initialize build queue
    Mutex debug_signing_mutex;

    BuildQueueConfig queue_config;
    queue_config.m_Flags = 0;
    queue_config.m_Heap = &self->m_Heap;
    queue_config.m_ThreadCount = (int)self->m_Options.m_ThreadCount;
    queue_config.m_NodeData = self->m_DagData->m_NodeData;
    queue_config.m_NodeState = self->m_Nodes.m_Storage;
    queue_config.m_MaxNodes = (int)self->m_Nodes.m_Size;
    queue_config.m_NodeRemappingTable = self->m_NodeRemap.m_Storage;
    queue_config.m_ScanCache = &self->m_ScanCache;
    queue_config.m_StatCache = &self->m_StatCache;
    queue_config.m_DigestCache = &self->m_DigestCache;
    queue_config.m_ShaDigestExtensionCount = dag->m_ShaExtensionHashes.GetCount();
    queue_config.m_ShaDigestExtensions = dag->m_ShaExtensionHashes.GetArray();
    queue_config.m_SharedResources = dag->m_SharedResources.GetArray();
    queue_config.m_SharedResourcesCount = dag->m_SharedResources.GetCount();
    queue_config.m_ThrottleInactivityPeriod = self->m_Options.m_ThrottleInactivityPeriod;
    queue_config.m_ThrottleOnHumanActivity = self->m_Options.m_ThrottleOnHumanActivity;
    queue_config.m_ThrottledThreadsAmount = self->m_Options.m_ThrottledThreadsAmount;

    if (self->m_Options.m_Verbose)
    {
        queue_config.m_Flags |= BuildQueueConfig::kFlagEchoAnnotations | BuildQueueConfig::kFlagEchoCommandLines;
    }
    if (!self->m_Options.m_Quiet)
    {
        queue_config.m_Flags |= BuildQueueConfig::kFlagEchoAnnotations;
    }
    if (self->m_Options.m_ContinueOnError)
    {
        queue_config.m_Flags |= BuildQueueConfig::kFlagContinueOnError;
    }

    if (self->m_Options.m_DebugSigning)
    {
        MutexInit(&debug_signing_mutex);
        queue_config.m_FileSigningLogMutex = &debug_signing_mutex;
        queue_config.m_FileSigningLog = fopen("signing-debug.txt", "w");
    }
    else
    {
        queue_config.m_FileSigningLogMutex = nullptr;
        queue_config.m_FileSigningLog = nullptr;
    }

#if ENABLED(CHECKED_BUILD)
    {
        ProfilerScope prof_scope("Tundra DebugCheckRemap", 0);
        // Paranoia - double check node remapping table
        for (size_t i = 0, count = self->m_Nodes.m_Size; i < count; ++i)
        {
            const RuntimeNode *state = self->m_Nodes.m_Storage + i;
            const Frozen::DagNode *src = state->m_DagNode;
            const int src_index = int(src - self->m_DagData->m_NodeData);
            int remapped_index = self->m_NodeRemap[src_index];
            CHECK(size_t(remapped_index) == i);
        }
    }
#endif

    BuildQueue build_queue;
    BuildQueueInit(&build_queue, &queue_config);

    BuildResult::Enum build_result = BuildResult::kOk;

    build_result = BuildQueueBuildNodeRange(&build_queue, 0, self->m_Nodes.m_Size);

    if (self->m_Options.m_DebugSigning)
    {
        fclose((FILE *)queue_config.m_FileSigningLog);
        MutexDestroy(&debug_signing_mutex);
    }

    // Shut down build queue
    BuildQueueDestroy(&build_queue);

    return build_result;
}

// Save scan cache
bool DriverSaveScanCache(Driver *self)
{
    ScanCache *scan_cache = &self->m_ScanCache;

    if (!ScanCacheDirty(scan_cache))
        return true;

    // This will be invalidated.
    self->m_ScanData = nullptr;

    bool success = ScanCacheSave(scan_cache, self->m_DagData->m_ScanCacheFileNameTmp, &self->m_Heap);

    // Unmap the file so we can overwrite it (on Windows.)
    MmapFileDestroy(&self->m_ScanFile);

    if (success)
    {
        success = RenameFile(self->m_DagData->m_ScanCacheFileNameTmp, self->m_DagData->m_ScanCacheFileName);
    }
    else
    {
        remove(self->m_DagData->m_ScanCacheFileNameTmp);
    }

    return success;
}

// Save digest cache
bool DriverSaveDigestCache(Driver *self)
{
    // This will be invalidated.
    return DigestCacheSave(&self->m_DigestCache, &self->m_Heap, self->m_DagData->m_DigestCacheFileName, self->m_DagData->m_DigestCacheFileNameTmp);
}

struct StateSavingSegments
{
    BinarySegment *main;
    BinarySegment *guid;
    BinarySegment *state;
    BinarySegment *array;
    BinarySegment *string;
};

static const char *GetFileNameFrom(const FrozenFileAndHash &container)
{
    return container.m_Filename;
}

static const char *GetFileNameFrom(const FrozenString &container)
{
    return container;
}

template <class TNodeType>
static void save_node_sharedcode(bool nodeWasBuiltSuccesfully, const HashDigest *input_signature, const TNodeType *src_node, const HashDigest *guid, const StateSavingSegments &segments)
{
    BinarySegmentWriteInt32(segments.state, nodeWasBuiltSuccesfully ? 1 : 0);

    BinarySegmentWrite(segments.guid, (const char *)guid, sizeof(HashDigest));

    BinarySegmentWrite(segments.state, (const char *)input_signature, sizeof(HashDigest));

    int32_t file_count = src_node->m_OutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.state, file_count);
    BinarySegmentWritePointer(segments.state, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
    {
        BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
        BinarySegmentWriteStringData(segments.string, GetFileNameFrom(src_node->m_OutputFiles[i]));
    }

    file_count = src_node->m_AuxOutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.state, file_count);
    BinarySegmentWritePointer(segments.state, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
    {
        BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
        BinarySegmentWriteStringData(segments.string, GetFileNameFrom(src_node->m_AuxOutputFiles[i]));
    }

    BinarySegmentWritePointer(segments.state, BinarySegmentPosition(segments.string));
    BinarySegmentWriteStringData(segments.string, src_node->m_Action);
}

static bool node_was_used_by_this_dag_previously(const Frozen::BuiltNode *node_state_data, uint32_t current_dag_identifier)
{
    auto &previous_dags = node_state_data->m_DagsWeHaveSeenThisNodeInPreviously;
    return std::find(previous_dags.begin(), previous_dags.end(), current_dag_identifier) != previous_dags.end();
}

bool DriverSaveBuildState(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StateSaveTimeCycles);
    ProfilerScope prof_scope("Tundra SaveState", 0);

    MemAllocLinearScope alloc_scope(&self->m_Allocator);

    BinaryWriter writer;
    BinaryWriterInit(&writer, &self->m_Heap);

    StateSavingSegments segments;
    BinarySegment *main_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *guid_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *state_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *array_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *string_seg = BinaryWriterAddSegment(&writer);

    segments.main = main_seg;
    segments.guid = guid_seg;
    segments.state = state_seg;
    segments.array = array_seg;
    segments.string = string_seg;

    BinaryLocator guid_ptr = BinarySegmentPosition(guid_seg);
    BinaryLocator state_ptr = BinarySegmentPosition(state_seg);

    uint32_t src_count = self->m_DagData->m_NodeCount;
    const HashDigest *src_guids = self->m_DagData->m_NodeGuids;
    const Frozen::DagNode *src_data = self->m_DagData->m_NodeData;
    RuntimeNode *new_state = self->m_Nodes.m_Storage;
    const size_t new_state_count = self->m_Nodes.m_Size;

    std::sort(new_state, new_state + new_state_count, [=](const RuntimeNode &l, const RuntimeNode &r) {
        // We know guids are sorted, so all we need to do is compare pointers into that table.
        return l.m_DagNode < r.m_DagNode;
    });

    const HashDigest *old_guids = nullptr;
    const Frozen::BuiltNode *old_state = nullptr;
    uint32_t old_count = 0;

    if (const Frozen::AllBuiltNodes *state_data = self->m_StateData)
    {
        old_guids = state_data->m_NodeGuids;
        old_state = state_data->m_NodeStates;
        old_count = state_data->m_NodeCount;
    }

    int entry_count = 0;
    uint32_t this_dag_hashed_identifier = self->m_DagData->m_HashedIdentifier;

    auto save_node_state = [=](bool nodeWasBuiltSuccessfully, const HashDigest *input_signature, const Frozen::DagNode *src_node, const Frozen::BuiltNode *node_data_state, const HashDigest *guid) -> void {
        MemAllocLinear *scratch = &self->m_Allocator;

        save_node_sharedcode(nodeWasBuiltSuccessfully, input_signature, src_node, guid, segments);

        HashSet<kFlagPathStrings> implicitDependencies;
        if (src_node->m_Scanner)
            HashSetInit(&implicitDependencies, &self->m_Heap);

        int32_t file_count = src_node->m_InputFiles.GetCount();
        BinarySegmentWriteInt32(state_seg, file_count);
        BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            uint64_t timestamp = 0;
            FileInfo fileInfo = StatCacheStat(&self->m_StatCache, src_node->m_InputFiles[i].m_Filename, src_node->m_InputFiles[i].m_FilenameHash);
            if (fileInfo.Exists())
                timestamp = fileInfo.m_Timestamp;

            BinarySegmentWriteUint64(array_seg, timestamp);

            BinarySegmentWritePointer(array_seg, BinarySegmentPosition(string_seg));
            BinarySegmentWriteStringData(string_seg, src_node->m_InputFiles[i].m_Filename);

            if (src_node->m_Scanner)
            {
                MemAllocLinearScope alloc_scope(scratch);

                ScanInput scan_input;
                scan_input.m_ScannerConfig = src_node->m_Scanner;
                scan_input.m_ScratchAlloc = scratch;
                scan_input.m_ScratchHeap = &self->m_Heap;
                scan_input.m_FileName = src_node->m_InputFiles[i].m_Filename;
                scan_input.m_ScanCache = &self->m_ScanCache;

                ScanOutput scan_output;

                // It looks like we're re-running the scanner here, but the scan results should all be cached already, so it
                // should be fast.
                if (ScanImplicitDeps(&self->m_StatCache, &scan_input, &scan_output))
                {
                    for (int i = 0, count = scan_output.m_IncludedFileCount; i < count; ++i)
                    {
                        const FileAndHash &path = scan_output.m_IncludedFiles[i];
                        if (!HashSetLookup(&implicitDependencies, path.m_FilenameHash, path.m_Filename))
                            HashSetInsert(&implicitDependencies, path.m_FilenameHash, path.m_Filename);
                    }
                }
            }
        }

        if (src_node->m_Scanner)
        {
            BinarySegmentWriteInt32(state_seg, implicitDependencies.m_RecordCount);
            BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));

            HashSetWalk(&implicitDependencies, [=](uint32_t index, uint32_t hash, const char *filename) {
                uint64_t timestamp = 0;
                FileInfo fileInfo = StatCacheStat(&self->m_StatCache, filename, hash);
                if (fileInfo.Exists())
                    timestamp = fileInfo.m_Timestamp;

                BinarySegmentWriteUint64(array_seg, timestamp);

                BinarySegmentWritePointer(array_seg, BinarySegmentPosition(string_seg));
                BinarySegmentWriteStringData(string_seg, filename);
            });

            HashSetDestroy(&implicitDependencies);
        }
        else
        {
            BinarySegmentWriteInt32(state_seg, 0);
            BinarySegmentWriteNullPointer(state_seg);
        }

        //we cast the empty_frozen_array below here to a FrozenArray<uint32_t> that is empty, so the code below gets a lot simpler.
        const FrozenArray<uint32_t> &previous_dags = (node_data_state == nullptr) ? FrozenArray<uint32_t>::empty() : node_data_state->m_DagsWeHaveSeenThisNodeInPreviously;

        bool haveToAddOurselves = std::find(previous_dags.begin(), previous_dags.end(), this_dag_hashed_identifier) == previous_dags.end();

        BinarySegmentWriteUint32(state_seg, previous_dags.GetCount() + (haveToAddOurselves ? 1 : 0));
        BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));
        for (auto &identifier : previous_dags)
            BinarySegmentWriteUint32(array_seg, identifier);

        if (haveToAddOurselves)
            BinarySegmentWriteUint32(array_seg, this_dag_hashed_identifier);
    };

    auto save_node_state_old = [=](bool nodeWasBuiltSuccesfully, const HashDigest *input_signature, const Frozen::BuiltNode *src_node, const HashDigest *guid) -> void {
        save_node_sharedcode(nodeWasBuiltSuccesfully, input_signature, src_node, guid, segments);

        int32_t file_count = src_node->m_InputFiles.GetCount();
        BinarySegmentWriteInt32(state_seg, file_count);
        BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, src_node->m_InputFiles[i].m_Timestamp);
            BinarySegmentWritePointer(array_seg, BinarySegmentPosition(string_seg));
            BinarySegmentWriteStringData(string_seg, src_node->m_InputFiles[i].m_Filename);
        }

        file_count = src_node->m_ImplicitInputFiles.GetCount();
        BinarySegmentWriteInt32(state_seg, file_count);
        BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, src_node->m_ImplicitInputFiles[i].m_Timestamp);
            BinarySegmentWritePointer(array_seg, BinarySegmentPosition(string_seg));
            BinarySegmentWriteStringData(string_seg, src_node->m_ImplicitInputFiles[i].m_Filename);
        }

        int32_t dag_count = src_node->m_DagsWeHaveSeenThisNodeInPreviously.GetCount();
        BinarySegmentWriteInt32(state_seg, dag_count);
        BinarySegmentWritePointer(state_seg, BinarySegmentPosition(array_seg));
        BinarySegmentWrite(array_seg, src_node->m_DagsWeHaveSeenThisNodeInPreviously.GetArray(), dag_count * sizeof(uint32_t));
    };

    auto save_new = [=, &entry_count](size_t index) {
        const RuntimeNode *elem = new_state + index;
        const Frozen::DagNode *src_elem = elem->m_DagNode;
        const int src_index = int(src_elem - src_data);
        const HashDigest *guid = src_guids + src_index;

        // If this node never computed an input signature (due to an error, or build cancellation), copy the old build progress over to retain the history.
        // Only do this if the output files and aux output files agree with the previously stored build state.
        if (elem->m_BuildResult == NodeBuildResult::kDidNotRun)
        {
            if (const HashDigest *old_guid = BinarySearch(old_guids, old_count, *guid))
            {
                size_t old_index = old_guid - old_guids;
                const Frozen::BuiltNode *old_state_data = old_state + old_index;
                save_node_state_old(old_state_data->m_WasBuiltSuccessfully > 0, &old_state_data->m_InputSignature, old_state_data, guid);
                ++entry_count;
                ++g_Stats.m_StateSaveNew;
            }
        }
        else
        {
            bool nodeWasBuiltSuccesfully = elem->m_BuildResult == NodeBuildResult::kRanSuccesfully || elem->m_BuildResult == NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun;
            save_node_state(nodeWasBuiltSuccesfully, &elem->m_InputSignature, src_elem, elem->m_MmapState, guid);
            ++entry_count;
            ++g_Stats.m_StateSaveNew;
        }
    };

    auto save_old = [=, &entry_count](size_t index) {
        const HashDigest *guid = old_guids + index;
        const Frozen::BuiltNode *data = old_state + index;

        // Make sure this node is still relevant before saving.
        bool node_is_in_dag = BinarySearch(src_guids, src_count, *guid) != nullptr;

        if (node_is_in_dag || !node_was_used_by_this_dag_previously(data, this_dag_hashed_identifier))
        {
            save_node_state_old(data->m_WasBuiltSuccessfully, &data->m_InputSignature, data, guid);
            ++entry_count;
            ++g_Stats.m_StateSaveOld;
        }
        else
        {
            // Drop this node.
            ++g_Stats.m_StateSaveDropped;
        }
    };

    auto key_new = [=](size_t index) -> const HashDigest * {
        int dag_index = int(new_state[index].m_DagNode - src_data);
        return src_guids + dag_index;
    };

    auto key_old = [=](size_t index) {
        return old_guids + index;
    };

    TraverseSortedArrays(
        new_state_count, save_new, key_new,
        old_count, save_old, key_old);

    // Complete main data structure.
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);
    BinarySegmentWriteInt32(main_seg, entry_count);
    BinarySegmentWritePointer(main_seg, guid_ptr);
    BinarySegmentWritePointer(main_seg, state_ptr);
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);

    // Unmap old state data.
    MmapFileUnmap(&self->m_StateFile);
    self->m_StateData = nullptr;

    bool success = BinaryWriterFlush(&writer, self->m_DagData->m_StateFileNameTmp);

    if (success)
    {
        // Commit atomically with a file rename.
        success = RenameFile(self->m_DagData->m_StateFileNameTmp, self->m_DagData->m_StateFileName);
    }
    else
    {
        remove(self->m_DagData->m_StateFileNameTmp);
    }

    BinaryWriterDestroy(&writer);

    return success;
}

// Returns true if the path was actually cleaned up.
// Does NOT delete symlinks.
static bool CleanupPath(const char *path)
{
    FileInfo info = GetFileInfo(path);
    if (!info.Exists())
        return false;
    if (info.IsSymlink())
        return false;
#if defined(TUNDRA_UNIX)
    return 0 == remove(path);
#else
    else if (info.IsDirectory())
        return TRUE == RemoveDirectoryA(path);
    else
        return TRUE == DeleteFileA(path);
#endif
}

void DriverRemoveStaleOutputs(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StaleCheckTimeCycles);
    ProfilerScope prof_scope("Tundra RemoveStaleOutputs", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::AllBuiltNodes *state = self->m_StateData;
    MemAllocLinear *scratch = &self->m_Allocator;

    MemAllocLinearScope scratch_scope(scratch);

    if (!state)
    {
        Log(kDebug, "unable to clean up stale output files - no previous build state");
        return;
    }

    HashSet<kFlagPathStrings> file_table;
    HashSetInit(&file_table, &self->m_Heap);

    // Insert all current regular and aux output files into the hash table.
    auto add_file = [&file_table](const FrozenFileAndHash &p) -> void {
        const uint32_t hash = p.m_FilenameHash;

        if (!HashSetLookup(&file_table, hash, p.m_Filename))
        {
            HashSetInsert(&file_table, hash, p.m_Filename);
        }
    };

    for (int i = 0, node_count = dag->m_NodeCount; i < node_count; ++i)
    {
        const Frozen::DagNode *node = dag->m_NodeData + i;

        for (const FrozenFileAndHash &p : node->m_OutputFiles)
        {
            add_file(p);
        }

        for (const FrozenFileAndHash &p : node->m_AuxOutputFiles)
        {
            add_file(p);
        }
    }

    HashSet<kFlagPathStrings> nuke_table;
    HashSetInit(&nuke_table, &self->m_Heap);

    // Check all output files in the state if they're still around.
    // Otherwise schedule them (and all their parent dirs) for nuking.
    // We will rely on the fact that we can't rmdir() non-empty directories.
    auto check_file = [&file_table, &nuke_table, scratch](const char *path) {
        uint32_t path_hash = Djb2HashPath(path);

        if (!HashSetLookup(&file_table, path_hash, path))
        {
            if (!HashSetLookup(&nuke_table, path_hash, path))
            {
                HashSetInsert(&nuke_table, path_hash, path);
            }

            PathBuffer buffer;
            PathInit(&buffer, path);

            while (PathStripLast(&buffer))
            {
                if (buffer.m_SegCount == 0)
                    break;

                char dir[kMaxPathLength];
                PathFormat(dir, &buffer);
                uint32_t dir_hash = Djb2HashPath(dir);

                if (!HashSetLookup(&nuke_table, dir_hash, dir))
                {
                    HashSetInsert(&nuke_table, dir_hash, StrDup(scratch, dir));
                }
            }
        }
    };

    for (int i = 0, state_count = state->m_NodeCount; i < state_count; ++i)
    {
        const Frozen::BuiltNode *node = state->m_NodeStates + i;

        if (!node_was_used_by_this_dag_previously(node, dag->m_HashedIdentifier))
            continue;

        for (const char *path : node->m_OutputFiles)
        {
            check_file(path);
        }

        for (const char *path : node->m_AuxOutputFiles)
        {
            check_file(path);
        }
    }

    // Create list of files and dirs, sort descending by path length. This sorts
    // files and subdirectories before their parent directories.
    const char **paths = LinearAllocateArray<const char *>(scratch, nuke_table.m_RecordCount);
    HashSetWalk(&nuke_table, [paths](uint32_t index, uint32_t hash, const char *str) {
        paths[index] = str;
    });

    std::sort(paths, paths + nuke_table.m_RecordCount, [](const char *l, const char *r) {
        return strlen(r) < strlen(l);
    });

    uint32_t nuke_count = nuke_table.m_RecordCount;
    uint64_t time_exec_started = TimerGet();
    for (uint32_t i = 0; i < nuke_count; ++i)
    {
        if (CleanupPath(paths[i]))
        {
            Log(kDebug, "cleaned up %s", paths[i]);
        }
    }

    if (nuke_count > 0)
    {
        char buffer[2000];
        snprintf(buffer, sizeof(buffer), "Delete %d artifact files that are no longer in use. (like %s)", nuke_count, paths[0]);
        PrintNonNodeActionResult(TimerDiffSeconds(time_exec_started, TimerGet()), (int)self->m_Nodes.m_Size, MessageStatusLevel::Success, buffer);
    }

    HashSetDestroy(&nuke_table);
    HashSetDestroy(&file_table);
}

void DriverCleanOutputs(Driver *self)
{
    ProfilerScope prof_scope("Tundra Clean", 0);
    int count = 0;
    for (RuntimeNode &state : self->m_Nodes)
    {
        for (const FrozenFileAndHash &fh : state.m_DagNode->m_OutputFiles)
        {
            if (0 == RemoveFileOrDir(fh.m_Filename))
                ++count;
        }
    }
    Log(kInfo, "Removed %d output files\n", count);
}


