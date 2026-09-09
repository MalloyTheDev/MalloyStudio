#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>

// One worker issues I/O; its owner may request stop at any time. The pipe must
// use FILE_FLAG_OVERLAPPED and outlive this object and its joined worker.
class CancellablePipeIo {
public:
    explicit CancellablePipeIo(void* pipe) : m_pipe(static_cast<HANDLE>(pipe)) {
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_stop) { m_error = GetLastError(); return; }
        m_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_complete) m_error = GetLastError();
    }

    ~CancellablePipeIo() {
        if (m_complete) CloseHandle(m_complete);
        if (m_stop) CloseHandle(m_stop);
    }
    CancellablePipeIo(const CancellablePipeIo&) = delete;
    CancellablePipeIo& operator=(const CancellablePipeIo&) = delete;

    bool valid() const { return m_stop && m_complete; }
    void requestStop() { if (m_stop) SetEvent(m_stop); }
    bool stopped() const {
        return !valid() || WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0;
    }

    bool connect() {
        if (!canStart()) return false;
        OVERLAPPED operation{};
        operation.hEvent = m_complete;
        ResetEvent(m_complete);
        if (ConnectNamedPipe(m_pipe, &operation)) return true;
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return true;
        if (error != ERROR_IO_PENDING) return false;
        DWORD transferred = 0;
        return finish(operation, transferred);
    }

    bool writeAll(const char* data, std::int64_t bytes) {
        if (bytes < 0 || (!data && bytes)) return false;
        std::int64_t offset = 0;
        while (offset < bytes) {
            if (!canStart()) return false;
            OVERLAPPED operation{};
            operation.hEvent = m_complete;
            ResetEvent(m_complete);
            DWORD transferred = 0;
            const DWORD count = static_cast<DWORD>((std::min)(
                bytes - offset, std::int64_t((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(m_pipe, data + offset, count, &transferred, &operation)) {
                if (GetLastError() != ERROR_IO_PENDING || !finish(operation, transferred))
                    return false;
            }
            if (!transferred) return false;
            offset += transferred;
        }
        return true;
    }

private:
    bool canStart() const {
        if (!valid()) { SetLastError(m_error); return false; }
        if (stopped()) { SetLastError(ERROR_OPERATION_ABORTED); return false; }
        return true;
    }

    bool finish(OVERLAPPED& operation, DWORD& transferred) {
        // Stop remains signalled across the check-to-I/O and thread-start
        // windows. A one-shot CancelSynchronousIo call can miss both windows.
        const HANDLE events[] = {m_complete, m_stop};
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0) {
            CancelIoEx(m_pipe, &operation);
        }
        // Cancellation is only a request. The operation and payload must stay
        // alive until Windows confirms completion, even after cancellation.
        const BOOL completed = GetOverlappedResult(m_pipe, &operation, &transferred, TRUE);
        return completed && result != WAIT_FAILED;
    }

    HANDLE m_pipe;
    HANDLE m_stop = nullptr;
    HANDLE m_complete = nullptr;
    DWORD m_error = ERROR_INVALID_HANDLE;
};
