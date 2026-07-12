#include "SyncPsdFile.h"

#include <QString>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ufb {

namespace {

namespace ps = PSD_NAMESPACE;

// Box returned from DoRead and consumed by DoWaitForRead so we can
// hand WaitForRead a verdict without the SDK caring how we got it.
// Allocated through the SDK's own allocator so the lifetime model
// matches the rest of psd_sdk.
struct SyncReadOp {
    size_t bytesRead;
    size_t expected;
};

}  // namespace

SyncPsdFile::SyncPsdFile(ps::Allocator* allocator)
    : ps::File(allocator), m_fd(-1) {}

SyncPsdFile::~SyncPsdFile() {
    if (m_fd >= 0) ::close(m_fd);
}

bool SyncPsdFile::DoOpenRead(const wchar_t* filename) {
    const QByteArray utf8 = QString::fromWCharArray(filename).toUtf8();
    m_fd = ::open(utf8.constData(), O_RDONLY);
    return m_fd >= 0;
}

bool SyncPsdFile::DoOpenWrite(const wchar_t*) {
    // Thumbnailing never writes; psd_sdk only calls DoOpenWrite if
    // OpenWrite is invoked, which we don't do.
    return false;
}

bool SyncPsdFile::DoClose() {
    if (m_fd < 0) return true;
    const int rc = ::close(m_fd);
    m_fd = -1;
    return rc == 0;
}

ps::File::ReadOperation SyncPsdFile::DoRead(void* buffer, uint32_t count, uint64_t position) {
    ssize_t got = 0;
    if (m_fd >= 0) {
        // pread doesn't move the file position, which lets the SDK
        // issue overlapping reads without an external lock.
        ssize_t r = ::pread(m_fd, buffer, count, off_t(position));
        if (r > 0) got = r;
    }
    auto* op = static_cast<SyncReadOp*>(m_allocator->Allocate(sizeof(SyncReadOp), alignof(SyncReadOp)));
    op->bytesRead = (got < 0) ? 0 : size_t(got);
    op->expected = count;
    return op;
}

bool SyncPsdFile::DoWaitForRead(ReadOperation& operation) {
    auto* op = static_cast<SyncReadOp*>(operation);
    const bool ok = (op->bytesRead == op->expected);
    m_allocator->Free(op);
    operation = nullptr;
    return ok;
}

ps::File::WriteOperation SyncPsdFile::DoWrite(const void*, uint32_t, uint64_t) {
    return nullptr;
}

bool SyncPsdFile::DoWaitForWrite(WriteOperation&) {
    return false;
}

uint64_t SyncPsdFile::DoGetSize() const {
    if (m_fd < 0) return 0;
    struct stat st;
    if (::fstat(m_fd, &st) != 0) return 0;
    return uint64_t(st.st_size);
}

}  // namespace ufb
