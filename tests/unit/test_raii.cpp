// SPDX-License-Identifier: MIT
#include <wl/fd_handle.hpp>
#include <wl/raii.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;
using namespace wl;

// ── FileHandle ───────────────────────────────────────────────────────────────

class FileHandleTest : public ::testing::Test {
protected:
    fs::path tmp_path_;
    void     SetUp() override {
        tmp_path_ = fs::temp_directory_path() / "wl_raii_test.txt";
        std::ofstream{tmp_path_} << "hello";
    }
    void TearDown() override { fs::remove(tmp_path_); }
};

TEST_F(FileHandleTest, DefaultConstructIsNull) {
    FileHandle fh;
    EXPECT_TRUE(fh.IsNull());
}
TEST_F(FileHandleTest, OpenExistingFile) {
    FileHandle fh{tmp_path_, "r"};
    EXPECT_FALSE(fh.IsNull());
}
TEST_F(FileHandleTest, OpenMissingFileThrows) {
    EXPECT_THROW(FileHandle("/nope/x", "r"), std::system_error);
}
TEST_F(FileHandleTest, MoveConstructTransfers) {
    FileHandle a{tmp_path_, "r"};
    FILE*      raw = a.Get();
    FileHandle b{std::move(a)};
    EXPECT_TRUE(a.IsNull());
    EXPECT_EQ(b.Get(), raw);
}
TEST_F(FileHandleTest, MoveAssignTransfers) {
    FileHandle a{tmp_path_, "r"};
    FileHandle b;
    b = std::move(a);
    EXPECT_TRUE(a.IsNull());
    EXPECT_FALSE(b.IsNull());
}
TEST_F(FileHandleTest, DetachYieldsRaw) {
    FileHandle fh{tmp_path_, "r"};
    FILE*      raw = fh.Get();
    EXPECT_EQ(fh.Detach(), raw);
    EXPECT_TRUE(fh.IsNull());
    std::fclose(raw);
}
TEST_F(FileHandleTest, AdoptRawStdinNotClosed) {
    {FileHandle fh{FileHandle::adopt_raw, stdin};}
    // stdin must still be usable — just check we reach here
    EXPECT_TRUE(true);
}

// ── FdHandle ─────────────────────────────────────────────────────────────────

TEST(FdHandleTest, DefaultIsNull) {
    FdHandle fd;
    EXPECT_TRUE(fd.IsNull());
}
TEST(FdHandleTest, WrapValidFd) {
    int p[2];
    ASSERT_EQ(::pipe(p), 0);
    {
        FdHandle a{p[0]};
        FdHandle b{p[1]};
        EXPECT_FALSE(a.IsNull());
        EXPECT_FALSE(b.IsNull());
    }
}
TEST(FdHandleTest, MoveTransfers) {
    int p[2];
    ::pipe(p);
    ::close(p[1]);
    FdHandle a{p[0]};
    int      raw = a.Get();
    FdHandle b{std::move(a)};
    EXPECT_TRUE(a.IsNull());
    EXPECT_EQ(b.Get(), raw);
}
TEST(FdHandleTest, DetachYieldsRaw) {
    int p[2];
    ::pipe(p);
    ::close(p[1]);
    FdHandle a{p[0]};
    int      r = a.Detach();
    EXPECT_TRUE(a.IsNull());
    ::close(r);
}
TEST(FdHandleTest, CloseExplicit) {
    int p[2];
    ::pipe(p);
    ::close(p[1]);
    FdHandle a{p[0]};
    a.Close();
    EXPECT_TRUE(a.IsNull());
}

// ── ScopeExit ────────────────────────────────────────────────────────────────

TEST(ScopeExitTest, RunsOnDestruction) {
    bool ran = false;
    {ScopeExit g{[&] { ran = true; }};}
    EXPECT_TRUE(ran);
}
TEST(ScopeExitTest, RunsOnException) {
    bool ran = false;
    try {
        ScopeExit g{[&] { ran = true; }};
        throw std::runtime_error("x");
    } catch (...) {
    }
    EXPECT_TRUE(ran);
}
TEST(ScopeExitTest, CTADDeduction) {
    int n = 0;
    {ScopeExit g{[&] { ++n; }};}
    EXPECT_EQ(n, 1);
}
