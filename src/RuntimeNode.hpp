#pragma once

#include "Common.hpp"
#include "Hash.hpp"

namespace NodeBuildResult
{
    enum Enum
    {
        kDidNotRun = 0,
        kUpToDate,
        kRanSuccesfully,
        kRanFailed,
        kRanSuccessButDependeesRequireFrontendRerun
    };
}

namespace NodeStateFlags
{
    static const uint16_t kQueued = 1 << 0;
    static const uint16_t kActive = 1 << 1;
} // namespace NodeStateFlags

namespace Frozen
{
    struct DagNode;
    struct BuiltNode;
}

struct RuntimeNode
{
    uint16_t m_Flags;

#if ENABLED(CHECKED_BUILD)
    const char *m_DebugAnnotation;
#endif
    const Frozen::DagNode *m_DagNode;
    const Frozen::BuiltNode *m_BuiltNode;

    NodeBuildResult::Enum m_BuildResult;
    bool m_Finished;
    HashDigest m_InputSignature;
};

inline bool RuntimeNodeIsQueued(const RuntimeNode *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kQueued);
}

inline void RuntimeNodeFlagQueued(RuntimeNode *state)
{
    state->m_Flags |= NodeStateFlags::kQueued;
}

inline void RuntimeNodeFlagUnqueued(RuntimeNode *state)
{
    state->m_Flags &= ~NodeStateFlags::kQueued;
}

inline bool RuntimeNodeIsActive(const RuntimeNode *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kActive);
}

inline void RuntimeNodeFlagActive(RuntimeNode *state)
{
    state->m_Flags |= NodeStateFlags::kActive;
}

inline void RuntimeNodeFlagInactive(RuntimeNode *state)
{
    state->m_Flags &= ~NodeStateFlags::kActive;
}
