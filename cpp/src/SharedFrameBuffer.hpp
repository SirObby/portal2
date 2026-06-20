// SharedFrameBuffer: cross-platform shared memory segment for writing the
// rendered RGB24 frame each tick, read by the Python side via
// multiprocessing.shared_memory.
//
// Linux: shm_open + mmap (POSIX shared memory, name "/portal2_rl_frame_0")
// Windows: CreateFileMapping + MapViewOfFile (name "portal2_rl_frame_0",
//          mapped into the "Local\" namespace so it matches what
//          multiprocessing.shared_memory expects on Windows)
//
// See docs/protocol.md for the negotiated name/size handshake.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RLTransport {

class SharedFrameBuffer {
public:
    SharedFrameBuffer() = default;
    ~SharedFrameBuffer() { Close(); }

    bool Create(const std::string &name, size_t size) {
        m_name = name;
        m_size = size;

#ifdef _WIN32
        m_mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(size), ("Local\\" + name).c_str());
        if (!m_mapping) return false;

        m_data = static_cast<uint8_t *>(
            MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
        if (!m_data) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }
#else
        std::string shm_name = "/" + name;
        shm_unlink(shm_name.c_str());  // remove stale segment from a previous run

        m_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
        if (m_fd < 0) return false;

        if (ftruncate(m_fd, static_cast<off_t>(size)) != 0) {
            close(m_fd);
            shm_unlink(shm_name.c_str());
            m_fd = -1;
            return false;
        }

        m_data = static_cast<uint8_t *>(
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
        if (m_data == MAP_FAILED) {
            m_data = nullptr;
            close(m_fd);
            shm_unlink(shm_name.c_str());
            m_fd = -1;
            return false;
        }
#endif
        return true;
    }

    void Write(const uint8_t *pixels, size_t size) {
        if (!m_data || size > m_size) return;
        memcpy(m_data, pixels, size);
    }

    // Name as exposed to Python over the control socket (matches
    // docs/protocol.md HELLO_ACK.shm_name format).
    std::string ExportName() const {
#ifdef _WIN32
        return m_name;
#else
        return "/" + m_name;
#endif
    }

    void Close() {
#ifdef _WIN32
        if (m_data) {
            UnmapViewOfFile(m_data);
            m_data = nullptr;
        }
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
#else
        if (m_data) {
            munmap(m_data, m_size);
            m_data = nullptr;
        }
        if (m_fd >= 0) {
            close(m_fd);
            shm_unlink(("/" + m_name).c_str());
            m_fd = -1;
        }
#endif
    }

private:
    std::string m_name;
    size_t m_size = 0;
    uint8_t *m_data = nullptr;

#ifdef _WIN32
    HANDLE m_mapping = nullptr;
#else
    int m_fd = -1;
#endif
};

}  // namespace RLTransport
