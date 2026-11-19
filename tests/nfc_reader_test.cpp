// Standalone harness for NfcReader.hpp. Build and run with tests/run_tests.py.
//
// The unit is the frontend half of RETRO_ENVIRONMENT_GET_NFC_INTERFACE: what a
// core polling the reader sees as a figure goes on, is swapped and comes off,
// and that a save the core writes reaches the dump on disk.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "NfcReader.hpp"

using Xenu::NfcReader;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static std::string Dump(const char* name, size_t size, uint8_t seed)
{
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(seed + i);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return p.string();
}

static std::vector<uint8_t> Load(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void TestStates()
{
    std::printf("what a polling core sees\n");
    NfcReader r;
    const std::string a = Dump("nfc_reader_test_a.bin", 540, 1);
    Check(r.Status() == RETRO_NFC_STATUS_IDLE, "IDLE before the core scans");
    Check(r.SetTag(a), "a 540-byte dump goes on");
    Check(r.Status() == RETRO_NFC_STATUS_IDLE, "still IDLE with a tag until the core scans");
    r.StartScan();
    Check(r.Status() == RETRO_NFC_STATUS_TAG_PRESENT, "TAG_PRESENT once scanning");
    r.ClearTag();
    Check(r.Status() == RETRO_NFC_STATUS_SCANNING, "SCANNING with the tag off");
    r.SetTag(a);
    r.StopScan();
    Check(r.Status() == RETRO_NFC_STATUS_IDLE, "IDLE after stop_scan");
    r.StartScan();
    Check(r.Status() == RETRO_NFC_STATUS_TAG_PRESENT, "the tag is found again by the next scan");
}

static void TestSwap()
{
    std::printf("a figure swapped for another\n");
    NfcReader r;
    r.StartScan();
    r.SetTag(Dump("nfc_reader_test_a.bin", 540, 1));
    Check(r.Status() == RETRO_NFC_STATUS_TAG_PRESENT, "the first figure");
    r.SetTag(Dump("nfc_reader_test_b.bin", 540, 9));
    Check(r.Status() == RETRO_NFC_STATUS_SCANNING, "one SCANNING between the two");
    Check(r.Status() == RETRO_NFC_STATUS_TAG_PRESENT, "then the second, present");
    Check(r.Status() == RETRO_NFC_STATUS_TAG_PRESENT, "and only once");
}

static void TestInfoAndRead()
{
    std::printf("tag info and read\n");
    NfcReader r;
    retro_nfc_tag_info info{};
    uint8_t buf[600]{};
    const std::string big = Dump("nfc_reader_test_572.bin", 572, 3);
    r.SetTag(big);
    Check(!r.TagInfo(&info, 1), "no tag info while not scanning");
    Check(r.Read(0, buf, 10) == -1, "no read while not scanning");
    r.StartScan();
    Check(r.TagInfo(&info, 1), "tag info while present");
    Check(info.type == RETRO_NFC_TAG_TYPE_NTAG215, "an NTAG215");
    Check(info.mem_size == 540, "540 bytes even from a 572-byte dump");
    Check(info.uid_len == 7 && info.uid[0] == 3 && info.uid[2] == 5 && info.uid[3] == 7 && info.uid[6] == 10,
        "UID from bytes 0-2 and 4-7");
    info.caps = 0xdead;
    r.TagInfo(&info, 1);
    Check(info.caps == 0xdead, "version 1 leaves caps alone (the core's struct may not have it)");
    r.TagInfo(&info, 5);
    Check(info.caps == RETRO_NFC_CAP_NTAG, "version 5 says linear read/write");
    Check(r.Read(0, buf, 540) == 540 && buf[0] == 3 && buf[539] == static_cast<uint8_t>(3 + 539), "read all 540");
    Check(r.Read(560, buf, 100) == 12, "a read past the end is cut short");
    Check(r.Read(573, buf, 1) == -1, "a read starting past the end fails");
    Check(!r.SetTag(Dump("nfc_reader_test_small.bin", 100, 0)), "a 100-byte file is refused");
    Check(r.TagPath() == big, "and the reader keeps its tag");
}

static void TestWrite()
{
    std::printf("a save written back\n");
    NfcReader r;
    const std::string a = Dump("nfc_reader_test_w.bin", 540, 0);
    r.SetTag(a);
    const uint8_t save[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    Check(r.Write(16, save, 4) == -1, "no write while not scanning");
    r.StartScan();
    std::string written;
    Check(r.Write(16, save, 4, &written) == 4, "a write while present");
    Check(written == a, "names the file it wrote");
    const auto disk = Load(a);
    Check(disk.size() == 540 && disk[16] == 0xAA && disk[19] == 0xDD && disk[15] == 15 && disk[20] == 20,
        "the dump on disk has the save and nothing else changed");
    uint8_t back[4]{};
    r.Read(16, back, 4);
    Check(back[0] == 0xAA && back[3] == 0xDD, "and the reader reads it back");
    r.ClearTag();
    Check(r.Write(16, save, 4) == -1, "no write with the tag off");
}

int main()
{
    TestStates();
    TestSwap();
    TestInfoAndRead();
    TestWrite();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
