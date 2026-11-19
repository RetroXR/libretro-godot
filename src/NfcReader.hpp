#pragma once

// One virtual NFC reader, served from a tag dump on disk: the frontend half of
// RETRO_ENVIRONMENT_GET_NFC_INTERFACE (NfcInterface.hpp) for one machine.
//
// Header-only and Godot-free, so tests/nfc_reader_test.cpp can hold it to the
// interface's rules without a core. It behaves as the amiibo backend of
// libretro/RetroArch#19754 (nfc/nfc_backend_amiibo.c) does, because that is
// the reader the cores using this interface were written against:
//
//   - A tag is reported only while the core scans: IDLE before start_scan,
//     SCANNING with nothing on the reader, TAG_PRESENT with a tag.
//   - A tag swapped for another while one is present reads SCANNING exactly
//     once, so a core that acts on the edge into TAG_PRESENT sees the new one.
//   - Every dump is an amiibo: an NTAG215 of 540 bytes, even when the file has
//     more (some dumps are 572), and the UID is its bytes 0-2 and 4-7.
//   - A write goes into the image and straight on to the file, and fails if
//     the file did not take it, so a save the core believes made is on disk.
//
// The frontend puts a tag on and takes it off from the main thread; the core
// calls from its own. One mutex covers both.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "NfcInterface.hpp"

namespace Xenu
{
class NfcReader
{
public:
    /// The smallest dump served: an NTAG215, as the PR's backend skips smaller.
    static constexpr size_t kMinBytes = 540;
    /// What a tag reports as its size: NTAG215's whole memory.
    static constexpr unsigned kNtag215Bytes = 540;

    // ── The frontend's side (main thread) ───────────────────────────────────

    /// Put the dump at `path` (UTF-8) on the reader, replacing any tag there.
    /// False, with the reader as it was, when it cannot be read or is too
    /// small to be a tag; `error` then says which.
    bool SetTag(const std::string& path, std::string* error = nullptr)
    {
        std::vector<uint8_t> bytes;
        if (!ReadFile(path, bytes))
        {
            if (error) *error = "cannot be read";
            return false;
        }
        if (bytes.size() < kMinBytes)
        {
            if (error) *error = std::to_string(bytes.size()) + " bytes, smaller than a tag";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        // Another tag where one was: the core gets one SCANNING between them.
        m_reswap = m_has_tag;
        m_path = path;
        m_bytes = std::move(bytes);
        m_has_tag = true;
        return true;
    }

    /// Take the tag off the reader.
    void ClearTag()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_has_tag = false;
        m_reswap = false;
        m_path.clear();
        m_bytes.clear();
    }

    std::string TagPath() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_path;
    }

    bool HasTag() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_has_tag;
    }

    /// Whether the core has the reader scanning: what a reader lamp shows.
    bool Scanning() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_scanning;
    }

    // ── The core's side (its own thread) ────────────────────────────────────

    bool StartScan()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_scanning = true;
        return true;
    }

    /// Stops scanning. The tag stays on the reader, as a figure stays on a
    /// pad nobody is reading; the next scan finds it again.
    void StopScan()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_scanning = false;
        m_reswap = false;
    }

    retro_nfc_status Status()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_scanning)
            return RETRO_NFC_STATUS_IDLE;
        if (!m_has_tag)
            return RETRO_NFC_STATUS_SCANNING;
        if (m_reswap)
        {
            m_reswap = false;
            return RETRO_NFC_STATUS_SCANNING;
        }
        return RETRO_NFC_STATUS_TAG_PRESENT;
    }

    /// Fills only the fields `version` has: `caps` is version 5's, and a core
    /// that asked for less passed a struct without it.
    bool TagInfo(retro_nfc_tag_info* info, unsigned version)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!info || !Present())
            return false;
        std::memset(info->uid, 0, sizeof(info->uid));
        info->uid[0] = m_bytes[0];
        info->uid[1] = m_bytes[1];
        info->uid[2] = m_bytes[2];
        info->uid[3] = m_bytes[4];
        info->uid[4] = m_bytes[5];
        info->uid[5] = m_bytes[6];
        info->uid[6] = m_bytes[7];
        info->uid_len = 7;
        info->type = RETRO_NFC_TAG_TYPE_NTAG215;
        info->mem_size = static_cast<unsigned>(std::min<size_t>(m_bytes.size(), kNtag215Bytes));
        if (version >= 5)
            info->caps = RETRO_NFC_CAP_NTAG;
        return true;
    }

    int Read(unsigned offset, uint8_t* buf, unsigned len)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!buf || !Present() || offset > m_bytes.size())
            return -1;
        len = static_cast<unsigned>(std::min<size_t>(len, m_bytes.size() - offset));
        std::memcpy(buf, m_bytes.data() + offset, len);
        return static_cast<int>(len);
    }

    /// Writes into the image and then the whole image to the file. -1 when no
    /// tag is present, the range is outside it, or the file did not take it
    /// (the image is then left as it was). `path_out` is the file written.
    int Write(unsigned offset, const uint8_t* buf, unsigned len, std::string* path_out = nullptr)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!buf || !Present() || offset > m_bytes.size())
            return -1;
        len = static_cast<unsigned>(std::min<size_t>(len, m_bytes.size() - offset));
        std::vector<uint8_t> next = m_bytes;
        std::memcpy(next.data() + offset, buf, len);
        if (!WriteFile(m_path, next))
            return -1;
        m_bytes = std::move(next);
        if (path_out) *path_out = m_path;
        return static_cast<int>(len);
    }

private:
    bool Present() const { return m_scanning && m_has_tag; }

    static std::filesystem::path Utf8Path(const std::string& path)
    {
        // u8path is deprecated from C++20, where char8_t says the same thing;
        // the tests build as C++17 and the extension newer.
#if defined(__cpp_lib_char8_t)
        return std::filesystem::path(std::u8string(path.begin(), path.end()));
#else
        return std::filesystem::u8path(path);
#endif
    }

    static bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        std::ifstream in(Utf8Path(path), std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !in.bad();
    }

    static bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream out(Utf8Path(path), std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return static_cast<bool>(out);
    }

    mutable std::mutex m_mutex;
    std::string m_path;
    std::vector<uint8_t> m_bytes;
    bool m_has_tag = false;
    bool m_scanning = false;
    bool m_reswap = false;
};
}
