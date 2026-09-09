#include "recording/CancellablePipeIo.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    std::wstring name;

    explicit Pipe(bool withReader) {
        static std::atomic<unsigned> sequence{0};
        name = L"\\\\.\\pipe\\malloy_io_test_" + std::to_wstring(GetCurrentProcessId())
               + L"_" + std::to_wstring(++sequence);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 0, 0, nullptr);
        if (server == INVALID_HANDLE_VALUE) fail("create server");
        if (withReader) {
            client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (client == INVALID_HANDLE_VALUE) fail("create reader");
        }
    }
    ~Pipe() {
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    }
    static void fail(const char* message) {
        std::cerr << message << " (Windows error " << GetLastError() << ")\n";
        std::exit(1);
    }
};

void require(bool condition, const char* message) {
    if (!condition) Pipe::fail(message);
}

bool finish(std::future<bool>& worker, CancellablePipeIo& io) {
    if (worker.wait_for(2s) != std::future_status::ready) {
        io.requestStop();
        if (worker.wait_for(2s) != std::future_status::ready)
            Pipe::fail("cancelled I/O did not finish");
        Pipe::fail("I/O exceeded its expected completion bound");
    }
    return worker.get();
}
}

int main() {
    {
        Pipe pipe(true);
        CancellablePipeIo io(pipe.server);
        require(io.valid() && io.connect(), "already connected pipe");
        const std::string payload = "complete synthetic media payload";
        auto writer = std::async(std::launch::async, [&] { return io.writeAll(payload.data(), payload.size()); });
        std::vector<char> received(payload.size());
        DWORD read = 0;
        require(ReadFile(pipe.client, received.data(), DWORD(received.size()), &read, nullptr), "read payload");
        require(finish(writer, io), "successful complete write");
        require(std::string(received.data(), read) == payload, "payload integrity");
    }
    {
        Pipe pipe(false);
        CancellablePipeIo io(pipe.server);
        auto acceptor = std::async(std::launch::async, [&] { return io.connect(); });
        require(acceptor.wait_for(20ms) == std::future_status::timeout, "accept must await reader");
        io.requestStop();
        require(!finish(acceptor, io), "cancel pending accept");
    }
    const std::vector<char> payload(8 * 1024 * 1024, 'x');
    // The standard library initializes process-wide thread support on first
    // use. Measure repeated worker retirement after that initialization.
    DWORD before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before), "read initial handle count");
    for (int iteration = 0; iteration < 100; ++iteration) {
        Pipe pipe(true);
        CancellablePipeIo io(pipe.server);
        require(io.connect(), "connect stress pipe");
        // Both orderings exercise stop before the worker enters I/O and stop
        // concurrent with the worker's canStart-to-WriteFile window.
        if (iteration % 2 == 0) io.requestStop();
        auto writer = std::async(std::launch::async, [&] { return io.writeAll(payload.data(), payload.size()); });
        io.requestStop();
        require(!finish(writer, io), "stop cannot be missed at worker startup");
    }
    {
        Pipe pipe(true);
        CancellablePipeIo io(pipe.server);
        require(io.connect(), "connect stalled pipe");
        auto writer = std::async(std::launch::async, [&] { return io.writeAll(payload.data(), payload.size()); });
        require(writer.wait_for(30ms) == std::future_status::timeout, "write must stall without reads");
        io.requestStop();
        require(!finish(writer, io), "cancel stalled write");
    }
    {
        Pipe pipe(false);
        CancellablePipeIo io(pipe.server);
        io.requestStop();
        auto acceptor = std::async(std::launch::async, [&] { return io.connect(); });
        require(!finish(acceptor, io), "stop before acceptor startup");
    }
    DWORD after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &after), "read final handle count");
    std::cout << "Process handles before/after repeated workers: " << before << "/" << after << '\n';
    require(after <= before + 2, "I/O worker handles leaked");
    std::cout << "Pipe integrity, pending/startup cancellation and handle lifecycle passed\n";
}
