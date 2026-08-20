// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/FileSystem.h"
#include "common/Pcsx2Defs.h"
#include "common/Path.h"
#include <gtest/gtest.h>

TEST(Path, ToNativePath)
{
	ASSERT_EQ(Path::ToNativePath(""), "");

#ifdef _WIN32
	ASSERT_EQ(Path::ToNativePath("foo"), "foo");
	ASSERT_EQ(Path::ToNativePath("foo\\"), "foo");
	ASSERT_EQ(Path::ToNativePath("foo\\\\bar"), "foo\\bar");
	ASSERT_EQ(Path::ToNativePath("foo\\bar"), "foo\\bar");
	ASSERT_EQ(Path::ToNativePath("foo\\bar\\baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::ToNativePath("foo\\bar/baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::ToNativePath("foo/bar/baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::ToNativePath("foo/🙃bar/b🙃az"), "foo\\🙃bar\\b🙃az");
	ASSERT_EQ(Path::ToNativePath("\\\\foo\\bar\\baz"), "\\\\foo\\bar\\baz");
#else
	ASSERT_EQ(Path::ToNativePath("foo"), "foo");
	ASSERT_EQ(Path::ToNativePath("foo/"), "foo");
	ASSERT_EQ(Path::ToNativePath("foo//bar"), "foo/bar");
	ASSERT_EQ(Path::ToNativePath("foo/bar"), "foo/bar");
	ASSERT_EQ(Path::ToNativePath("foo/bar/baz"), "foo/bar/baz");
	ASSERT_EQ(Path::ToNativePath("/foo/bar/baz"), "/foo/bar/baz");
#endif
}

TEST(Path, IsValidFileName)
{
#if defined(_WIN32) || defined(__APPLE__)
	ASSERT_FALSE(Path::IsValidFileName("foo:bar", false));
	ASSERT_FALSE(Path::IsValidFileName("baz\\foo:bar", false));
	ASSERT_FALSE(Path::IsValidFileName("baz/foo:bar", false));
	ASSERT_FALSE(Path::IsValidFileName("baz\\foo:bar", true));
	ASSERT_FALSE(Path::IsValidFileName("baz/foo:bar", true));
#endif
#ifdef _WIN32
	ASSERT_TRUE(Path::IsValidFileName("baz\\foo", true));
	ASSERT_FALSE(Path::IsValidFileName("baz\\foo", false));
	ASSERT_FALSE(Path::IsValidFileName("foo.", true));
	ASSERT_FALSE(Path::IsValidFileName("foo\\.", true));
#else
	ASSERT_FALSE(Path::IsValidFileName("foo\\*", true));
	ASSERT_FALSE(Path::IsValidFileName("foo*", true));
#endif

	ASSERT_TRUE(Path::IsValidFileName("baz/foo", true));
	ASSERT_FALSE(Path::IsValidFileName("baz/foo", false));
}

TEST(Path, IsAbsolute)
{
	ASSERT_FALSE(Path::IsAbsolute(""));
	ASSERT_FALSE(Path::IsAbsolute("foo"));
	ASSERT_FALSE(Path::IsAbsolute("foo/bar"));
	ASSERT_FALSE(Path::IsAbsolute("foo/b🙃ar"));
#ifdef _WIN32
	ASSERT_TRUE(Path::IsAbsolute("C:\\foo/bar"));
	ASSERT_TRUE(Path::IsAbsolute("C://foo\\bar"));
	ASSERT_FALSE(Path::IsAbsolute("\\foo/bar"));
	ASSERT_TRUE(Path::IsAbsolute("\\\\foo\\bar\\baz"));
#else
	ASSERT_TRUE(Path::IsAbsolute("/foo/bar"));
#endif
}

TEST(Path, Canonicalize)
{
	ASSERT_EQ(Path::Canonicalize(""), Path::ToNativePath(""));
	ASSERT_EQ(Path::Canonicalize("foo/bar/../baz"), Path::ToNativePath("foo/baz"));
	ASSERT_EQ(Path::Canonicalize("foo/bar/./baz"), Path::ToNativePath("foo/bar/baz"));
	ASSERT_EQ(Path::Canonicalize("foo/./bar/./baz"), Path::ToNativePath("foo/bar/baz"));
	ASSERT_EQ(Path::Canonicalize("foo/bar/../baz/../foo"), Path::ToNativePath("foo/foo"));
	ASSERT_EQ(Path::Canonicalize("foo/bar/../baz/./foo"), Path::ToNativePath("foo/baz/foo"));
	ASSERT_EQ(Path::Canonicalize("./foo"), Path::ToNativePath("foo"));
	ASSERT_EQ(Path::Canonicalize("../foo"), Path::ToNativePath("../foo"));
	ASSERT_EQ(Path::Canonicalize("foo/b🙃ar/../b🙃az/./foo"), Path::ToNativePath("foo/b🙃az/foo"));
	ASSERT_EQ(Path::Canonicalize("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃az/../foℹ︎o"), Path::ToNativePath("ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/foℹ︎o"));
#ifdef _WIN32
	ASSERT_EQ(Path::Canonicalize("C:\\foo\\bar\\..\\baz\\.\\foo"), "C:\\foo\\baz\\foo");
	ASSERT_EQ(Path::Canonicalize("C:/foo\\bar\\..\\baz\\.\\foo"), "C:\\foo\\baz\\foo");
	ASSERT_EQ(Path::Canonicalize("foo\\bar\\..\\baz\\.\\foo"), "foo\\baz\\foo");
	ASSERT_EQ(Path::Canonicalize("foo\\bar/..\\baz/.\\foo"), "foo\\baz\\foo");
	ASSERT_EQ(Path::Canonicalize("\\\\foo\\bar\\baz/..\\foo"), "\\\\foo\\bar\\foo");
#else
	ASSERT_EQ(Path::Canonicalize("/foo/bar/../baz/./foo"), "/foo/baz/foo");
#endif
}

TEST(Path, Combine)
{
	ASSERT_EQ(Path::Combine("", ""), Path::ToNativePath(""));
	ASSERT_EQ(Path::Combine("foo", "bar"), Path::ToNativePath("foo/bar"));
	ASSERT_EQ(Path::Combine("foo/bar", "baz"), Path::ToNativePath("foo/bar/baz"));
	ASSERT_EQ(Path::Combine("foo/bar", "../baz"), Path::ToNativePath("foo/bar/../baz"));
	ASSERT_EQ(Path::Combine("foo/bar/", "/baz/"), Path::ToNativePath("foo/bar/baz"));
	ASSERT_EQ(Path::Combine("foo//bar", "baz/"), Path::ToNativePath("foo/bar/baz"));
	ASSERT_EQ(Path::Combine("foo//ba🙃r", "b🙃az/"), Path::ToNativePath("foo/ba🙃r/b🙃az"));
#ifdef _WIN32
	ASSERT_EQ(Path::Combine("C:\\foo\\bar", "baz"), "C:\\foo\\bar\\baz");
	ASSERT_EQ(Path::Combine("\\\\server\\foo\\bar", "baz"), "\\\\server\\foo\\bar\\baz");
	ASSERT_EQ(Path::Combine("foo\\bar", "baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::Combine("foo\\bar\\", "baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::Combine("foo/bar\\", "\\baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::Combine("\\\\foo\\bar", "baz"), "\\\\foo\\bar\\baz");
#else
	ASSERT_EQ(Path::Combine("/foo/bar", "baz"), "/foo/bar/baz");
#endif
}

// The frontend VFS can hand the core a URI instead of a path: RetroArch spells
// storage access framework content as "saf://<tree>/<path>". The two slashes
// after the scheme have to survive, otherwise the frontend no longer
// recognises the path it gave us.
TEST(Path, URI)
{
	static constexpr const char* TREE =
		"saf://content%3A%2F%2Fcom.android.externalstorage.documents%2Ftree%2Fprimary%253ARoms";

	ASSERT_EQ(Path::Combine("saf://tree", "track01.bin"), Path::ToNativePath("saf://tree/track01.bin"));
	ASSERT_EQ(Path::Combine("saf://tree/ps2", "track01.bin"), Path::ToNativePath("saf://tree/ps2/track01.bin"));
	ASSERT_EQ(Path::Combine(TREE, "game.cue"), Path::ToNativePath(std::string(TREE) + "/game.cue"));

	// What resolving a cue sheet's track file does.
	ASSERT_EQ(Path::GetDirectory("saf://tree/ps2/game.cue"), "saf://tree/ps2");
	ASSERT_EQ(Path::Combine(Path::GetDirectory("saf://tree/ps2/game.cue"), "track01.bin"),
		Path::ToNativePath("saf://tree/ps2/track01.bin"));

	ASSERT_EQ(Path::Canonicalize("saf://tree/ps2/./game.cue"), Path::ToNativePath("saf://tree/ps2/game.cue"));
	ASSERT_EQ(Path::Canonicalize("saf://tree/ps2/sub/../game.cue"), Path::ToNativePath("saf://tree/ps2/game.cue"));

	// Any scheme is kept, not just the one RetroArch uses.
	ASSERT_EQ(Path::Combine("content://provider/doc", "x"), Path::ToNativePath("content://provider/doc/x"));

	// What is not a scheme keeps being treated as a path, so its doubled
	// separator is collapsed as before.
	ASSERT_EQ(Path::Combine("1saf://tree", "baz"), Path::ToNativePath("1saf:/tree/baz"));
	ASSERT_EQ(Path::Combine("foo/bar://baz", "x"), Path::ToNativePath("foo/bar:/baz/x"));
	ASSERT_EQ(Path::Combine("C://foo", "x"), Path::ToNativePath("C:/foo/x"));
#ifndef _WIN32
	ASSERT_EQ(Path::ToNativePath("saf://tree/ps2/game.cue"), "saf://tree/ps2/game.cue");
#endif
}

TEST(Path, AppendDirectory)
{
	ASSERT_EQ(Path::AppendDirectory("foo/bar", "baz"), Path::ToNativePath("foo/baz/bar"));
	ASSERT_EQ(Path::AppendDirectory("", "baz"), Path::ToNativePath("baz"));
	ASSERT_EQ(Path::AppendDirectory("", ""), Path::ToNativePath(""));
	ASSERT_EQ(Path::AppendDirectory("foo/bar", "🙃"), Path::ToNativePath("foo/🙃/bar"));
#ifdef _WIN32
	ASSERT_EQ(Path::AppendDirectory("foo\\bar", "baz"), "foo\\baz\\bar");
	ASSERT_EQ(Path::AppendDirectory("\\\\foo\\bar", "baz"), "\\\\foo\\baz\\bar");
#else
	ASSERT_EQ(Path::AppendDirectory("/foo/bar", "baz"), "/foo/baz/bar");
#endif
}

TEST(Path, MakeRelative)
{
	ASSERT_EQ(Path::MakeRelative("", ""), Path::ToNativePath(""));
	ASSERT_EQ(Path::MakeRelative("foo", ""), Path::ToNativePath("foo"));
	ASSERT_EQ(Path::MakeRelative("", "foo"), Path::ToNativePath(""));
	ASSERT_EQ(Path::MakeRelative("foo", "bar"), Path::ToNativePath("foo"));

#ifdef _WIN32
#define A "C:\\"
#else
#define A "/"
#endif

	ASSERT_EQ(Path::MakeRelative(A "foo", A "bar"), Path::ToNativePath("../foo"));
	ASSERT_EQ(Path::MakeRelative(A "foo/bar", A "foo"), Path::ToNativePath("bar"));
	ASSERT_EQ(Path::MakeRelative(A "foo/bar", A "foo/baz"), Path::ToNativePath("../bar"));
	ASSERT_EQ(Path::MakeRelative(A "foo/b🙃ar", A "foo/b🙃az"), Path::ToNativePath("../b🙃ar"));
	ASSERT_EQ(Path::MakeRelative(A "f🙃oo/b🙃ar", A "f🙃oo/b🙃az"), Path::ToNativePath("../b🙃ar"));
	ASSERT_EQ(Path::MakeRelative(A "ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃ar", A "ŻąłóРстуぬねのはen🍪⟑η∏☉ⴤℹ︎∩₲ ₱⟑♰⫳🐱/b🙃az"), Path::ToNativePath("../b🙃ar"));

#undef A

#ifdef _WIN32
	ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\baz\\foo", "\\\\foo\\bar\\baz"), "foo");
	ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\foo", "\\\\foo\\bar\\baz"), "..\\foo");
	ASSERT_EQ(Path::MakeRelative("\\\\foo\\bar\\foo", "\\\\other\\bar\\foo"), "\\\\foo\\bar\\foo");
#endif
}

TEST(Path, GetExtension)
{
	ASSERT_EQ(Path::GetExtension("foo"), "");
	ASSERT_EQ(Path::GetExtension("foo.txt"), "txt");
	ASSERT_EQ(Path::GetExtension("foo.t🙃t"), "t🙃t");
	ASSERT_EQ(Path::GetExtension("foo."), "");
	ASSERT_EQ(Path::GetExtension("a/b/foo.txt"), "txt");
	ASSERT_EQ(Path::GetExtension("a/b/foo"), "");
}

TEST(Path, GetFileName)
{
	ASSERT_EQ(Path::GetFileName(""), "");
	ASSERT_EQ(Path::GetFileName("foo"), "foo");
	ASSERT_EQ(Path::GetFileName("foo.txt"), "foo.txt");
	ASSERT_EQ(Path::GetFileName("foo"), "foo");
	ASSERT_EQ(Path::GetFileName("foo/bar/."), ".");
	ASSERT_EQ(Path::GetFileName("foo/bar/baz"), "baz");
	ASSERT_EQ(Path::GetFileName("foo/bar/baz.txt"), "baz.txt");
#ifdef _WIN32
	ASSERT_EQ(Path::GetFileName("foo/bar\\baz"), "baz");
	ASSERT_EQ(Path::GetFileName("foo\\bar\\baz.txt"), "baz.txt");
#endif
}

TEST(Path, GetFileTitle)
{
	ASSERT_EQ(Path::GetFileTitle(""), "");
	ASSERT_EQ(Path::GetFileTitle("foo"), "foo");
	ASSERT_EQ(Path::GetFileTitle("foo.txt"), "foo");
	ASSERT_EQ(Path::GetFileTitle("foo/bar/."), "");
	ASSERT_EQ(Path::GetFileTitle("foo/bar/baz"), "baz");
	ASSERT_EQ(Path::GetFileTitle("foo/bar/baz.txt"), "baz");
#ifdef _WIN32
	ASSERT_EQ(Path::GetFileTitle("foo/bar\\baz"), "baz");
	ASSERT_EQ(Path::GetFileTitle("foo\\bar\\baz.txt"), "baz");
#endif
}

TEST(Path, GetDirectory)
{
	ASSERT_EQ(Path::GetDirectory(""), "");
	ASSERT_EQ(Path::GetDirectory("foo"), "");
	ASSERT_EQ(Path::GetDirectory("foo.txt"), "");
	ASSERT_EQ(Path::GetDirectory("foo/bar/."), "foo/bar");
	ASSERT_EQ(Path::GetDirectory("foo/bar/baz"), "foo/bar");
	ASSERT_EQ(Path::GetDirectory("foo/bar/baz.txt"), "foo/bar");
#ifdef _WIN32
	ASSERT_EQ(Path::GetDirectory("foo\\bar\\baz"), "foo\\bar");
	ASSERT_EQ(Path::GetDirectory("foo\\bar/baz.txt"), "foo\\bar");
#endif
}

TEST(Path, ChangeFileName)
{
	ASSERT_EQ(Path::ChangeFileName("", ""), Path::ToNativePath(""));
	ASSERT_EQ(Path::ChangeFileName("", "bar"), Path::ToNativePath("bar"));
	ASSERT_EQ(Path::ChangeFileName("bar", ""), Path::ToNativePath(""));
	ASSERT_EQ(Path::ChangeFileName("foo/bar", ""), Path::ToNativePath("foo"));
	ASSERT_EQ(Path::ChangeFileName("foo/", "bar"), Path::ToNativePath("foo/bar"));
	ASSERT_EQ(Path::ChangeFileName("foo/bar", "baz"), Path::ToNativePath("foo/baz"));
	ASSERT_EQ(Path::ChangeFileName("foo//bar", "baz"), Path::ToNativePath("foo/baz"));
	ASSERT_EQ(Path::ChangeFileName("foo//bar.txt", "baz.txt"), Path::ToNativePath("foo/baz.txt"));
	ASSERT_EQ(Path::ChangeFileName("foo//ba🙃r.txt", "ba🙃z.txt"), Path::ToNativePath("foo/ba🙃z.txt"));
#ifdef _WIN32
	ASSERT_EQ(Path::ChangeFileName("foo/bar", "baz"), "foo\\baz");
	ASSERT_EQ(Path::ChangeFileName("foo//bar\\foo", "baz"), "foo\\bar\\baz");
	ASSERT_EQ(Path::ChangeFileName("\\\\foo\\bar\\foo", "baz"), "\\\\foo\\bar\\baz");
#else
	ASSERT_EQ(Path::ChangeFileName("/foo/bar", "baz"), "/foo/baz");
#endif
}

TEST(Path, CreateFileURL)
{
#ifdef _WIN32
	ASSERT_EQ(Path::CreateFileURL("C:\\foo\\bar"), "file:///C:/foo/bar");
	ASSERT_EQ(Path::CreateFileURL("\\\\server\\share\\file.txt"), "file://server/share/file.txt");
#else
	ASSERT_EQ(Path::CreateFileURL("/foo/bar"), "file:///foo/bar");
#endif
}

#if __linux__

static std::optional<std::string> create_test_directory()
{
	for (u16 i = 0; i < UINT16_MAX; i++)
	{
		std::string path = std::string("/tmp/pcsx2_path_test_") + std::to_string(i);
		if (!FileSystem::DirectoryExists(path.c_str()))
		{
			if (!FileSystem::CreateDirectoryPath(path.c_str(), false))
				break;

			return path;
		}
	}

	return std::nullopt;
}

TEST(Path, RealPathAbsoluteSymbolicLink)
{
	std::optional<std::string> test_dir = create_test_directory();
	ASSERT_TRUE(test_dir.has_value());

	// Create a file to point at.
	std::string file_path = Path::Combine(*test_dir, "file");
	ASSERT_TRUE(FileSystem::WriteStringToFile(file_path.c_str(), "Hello, world!"));

	// Create a symbolic link that points to said file.
	std::string link_path = Path::Combine(*test_dir, "link");
	ASSERT_TRUE(FileSystem::CreateSymLink(link_path.c_str(), file_path.c_str()));

	// Make sure the symbolic link is resolved correctly.
	ASSERT_EQ(Path::RealPath(link_path), file_path);

	// Clean up.
	ASSERT_TRUE(FileSystem::DeleteSymbolicLink(link_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteFilePath(file_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(test_dir->c_str()));
}

TEST(Path, RealPathRelativeSymbolicLink)
{
	std::optional<std::string> test_dir = create_test_directory();
	ASSERT_TRUE(test_dir.has_value());

	// Create a file to point at.
	std::string file_path = Path::Combine(*test_dir, "file");
	ASSERT_TRUE(FileSystem::WriteStringToFile(file_path.c_str(), "Hello, world!"));

	// Create a symbolic link that points to said file.
	std::string link_path = Path::Combine(*test_dir, "link");
	ASSERT_TRUE(FileSystem::CreateSymLink(link_path.c_str(), "file"));

	// Make sure the symbolic link is resolved correctly.
	ASSERT_EQ(Path::RealPath(link_path), file_path);

	// Clean up.
	ASSERT_TRUE(FileSystem::DeleteSymbolicLink(link_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteFilePath(file_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(test_dir->c_str()));
}

TEST(Path, RealPathDotDotSymbolicLink)
{
	std::optional<std::string> test_dir = create_test_directory();
	ASSERT_TRUE(test_dir.has_value());

	// Create a file to point at.
	std::string file_path = Path::Combine(*test_dir, "file");
	ASSERT_TRUE(FileSystem::WriteStringToFile(file_path.c_str(), "Hello, world!"));

	// Create a directory to put the link in.
	std::string link_dir = Path::Combine(*test_dir, "dir");
	ASSERT_TRUE(FileSystem::CreateDirectoryPath(link_dir.c_str(), false));

	// Create a symbolic link that points to said file.
	std::string link_path = Path::Combine(link_dir, "link");
	ASSERT_TRUE(FileSystem::CreateSymLink(link_path.c_str(), "../file"));

	// Make sure the symbolic link is resolved correctly.
	ASSERT_EQ(Path::RealPath(link_path), file_path);

	// Clean up.
	ASSERT_TRUE(FileSystem::DeleteSymbolicLink(link_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(link_dir.c_str()));
	ASSERT_TRUE(FileSystem::DeleteFilePath(file_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(test_dir->c_str()));
}

TEST(Path, RealPathCircularSymbolicLink)
{
	std::optional<std::string> test_dir = create_test_directory();
	ASSERT_TRUE(test_dir.has_value());

	// Create a circular symbolic link.
	std::string link_path = Path::Combine(*test_dir, "link");
	ASSERT_TRUE(FileSystem::CreateSymLink(link_path.c_str(), "."));

	// Make sure the link gets resolved correctly.
	ASSERT_EQ(Path::RealPath(link_path), *test_dir);
	ASSERT_EQ(Path::RealPath(Path::Combine(link_path, "link")), *test_dir);

	// Clean up.
	ASSERT_TRUE(FileSystem::DeleteSymbolicLink(link_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(test_dir->c_str()));
}

TEST(Path, RealPathLoopingSymbolicLink)
{
	std::optional<std::string> test_dir = create_test_directory();
	ASSERT_TRUE(test_dir.has_value());

	// Create a symbolic link that points to itself.
	std::string link_path = Path::Combine(*test_dir, "link");
	ASSERT_TRUE(FileSystem::CreateSymLink(link_path.c_str(), "link"));

	// Make sure this doesn't cause problems.
	ASSERT_EQ(Path::RealPath(link_path), link_path);

	// Clean up.
	ASSERT_TRUE(FileSystem::DeleteSymbolicLink(link_path.c_str()));
	ASSERT_TRUE(FileSystem::DeleteDirectory(test_dir->c_str()));
}

#endif
