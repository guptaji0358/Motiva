#pragma once
// Operation-scoped profile-generation evidence for wallpaper attach verification (Qt Core types only, so the
// rules are unit-tested without a desktop; WallpaperManager owns the wiring).
//
// Why: a verification proves "the wallpaper presented frames under the profile that was in force while it was
// being attached". The profile generation must therefore be captured when the attach OPERATION begins and carried
// unchanged through every job, retry and verification of that operation. Capturing it when the verification
// starts (or ends) would let an attach begun under profile N confirm a trial of profile N+1 installed meanwhile.
//
//  - beginOperation(g): a new operation (apply, Explorer-restart recovery, IPC recovery, battery resume) starts;
//    captures `g` (ProfileStore::generation()) NOW. Every operation re-captures; none reuses an older value.
//  - launchJob(): an attach job (first attempt, retry, re-attach after a stale discard) started inside the current
//    operation. It carries the operation's captured generation, never a fresh read.
//  - jobIsCurrent(job): false when a newer operation captured a different generation after the job launched; the
//    owner discards that job's result and launches a new one (same handling as a stale attach generation).
//  - The generation is compared with ProfileStore::generation() only at confirmation (confirmHealthy(gen)).
// Existing apply-token and rebuild-generation (m_attachGeneration) checks are separate and unchanged.
#include <QtGlobal>

class AttachProfileEvidence {
public:
    struct Job {
        quint64 profileGeneration = 0; // the operation's captured generation
    };

    void beginOperation(quint64 currentProfileGeneration) { m_operationGeneration = currentProfileGeneration; }
    quint64 operationGeneration() const { return m_operationGeneration; }

    Job launchJob() const { return Job{m_operationGeneration}; }
    bool jobIsCurrent(const Job& job) const { return job.profileGeneration == m_operationGeneration; }

private:
    quint64 m_operationGeneration = 0;
};
