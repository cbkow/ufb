// Synchronous ps::File implementation for PSD reads on macOS.
//
// psd_sdk's vendored NativeFile_Mac.mm dispatches every read onto the
// global default-qos GCD queue and waits on a semaphore. The block it
// hands to dispatch_read captures the operation pointer, signals the
// semaphore from an inner block, then continues running on the GCD
// worker after the waiter has unblocked. Under thumbnail churn (user
// browses an asset folder of mixed media), the operation/block can be
// torn down while the outer block is still being scheduled, leaving
// the next default-qos worker to invoke a freed function pointer
// (EXC_BAD_ACCESS / SIGBUS).
//
// We only thumbnail one PSD at a time per pool worker; there's no
// benefit to async I/O. This subclass uses pread/fstat directly, no
// GCD, no blocks. On Windows we keep ps::NativeFile (different code
// path, uses overlapped I/O with explicit handles).

#pragma once

// Psd.h is the SDK's umbrella header (drags in PsdPch.h with the
// platform/compiler defines, fixed-width int typedefs, and the
// PSD_NAMESPACE / PSD_ABSTRACT macros). The two headers below depend
// on it.
#include "Psd.h"
#include "PsdAllocator.h"
#include "PsdFile.h"

namespace ufb {

class SyncPsdFile final : public PSD_NAMESPACE::File {
public:
    explicit SyncPsdFile(PSD_NAMESPACE::Allocator* allocator);
    ~SyncPsdFile() override;

private:
    bool DoOpenRead(const wchar_t* filename) override;
    bool DoOpenWrite(const wchar_t* filename) override;
    bool DoClose() override;

    ReadOperation DoRead(void* buffer, uint32_t count, uint64_t position) override;
    bool DoWaitForRead(ReadOperation& operation) override;

    WriteOperation DoWrite(const void* buffer, uint32_t count, uint64_t position) override;
    bool DoWaitForWrite(WriteOperation& operation) override;

    uint64_t DoGetSize() const override;

    int m_fd;
};

}  // namespace ufb
