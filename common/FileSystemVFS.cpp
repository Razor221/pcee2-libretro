// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "FileSystemVFS.h"
#include "Console.h"
#include "Error.h"
#include "Path.h"
#include "ProgressCallback.h"
#include "StringUtil.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

// fopencookie and funopen live outside the C++ standard headers.
#include <stdio.h>

// A frontend file handle has to end up behind a std::FILE*, because that is
// what the emulator passes around everywhere (FlatFileReader, libchdr, the
// settings interface, ...). The three stdio implementations that let a program
// supply its own read/write/seek/close are glibc's fopencookie and the BSD
// funopen family; where neither exists the layer compiles down to "no VFS" and
// FileSystem keeps doing everything natively.
#if defined(__GLIBC__)
#define VFS_STREAMS_FOPENCOOKIE 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define VFS_STREAMS_FUNOPEN64 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
// funopen's offsets are 64-bit on these; on 32-bit glibc-less platforms they
// would not be, which is why this is not a blanket "everything else" case.
#define VFS_STREAMS_FUNOPEN 1
#endif

#if defined(VFS_STREAMS_FOPENCOOKIE) || defined(VFS_STREAMS_FUNOPEN64) || defined(VFS_STREAMS_FUNOPEN)
#define VFS_STREAMS_SUPPORTED 1
#endif

// Mirrors of the retro_vfs constants. Kept here rather than pulled in from
// libretro.h so that common/ does not gain a dependency on the frontend's
// headers; the hosting layer is responsible for handing us hooks that speak
// this ABI (which it does by adapting, not by casting).
enum : u32
{
	VFS_ACCESS_READ = (1 << 0),
	VFS_ACCESS_WRITE = (1 << 1),
	VFS_ACCESS_READ_WRITE = VFS_ACCESS_READ | VFS_ACCESS_WRITE,
	VFS_ACCESS_UPDATE_EXISTING = (1 << 2),
};

enum : u32
{
	VFS_HINT_NONE = 0,
};

enum : int
{
	VFS_SEEK_SET = 0,
	VFS_SEEK_CUR = 1,
	VFS_SEEK_END = 2,
};

enum : int
{
	VFS_STAT_IS_VALID = (1 << 0),
	VFS_STAT_IS_DIRECTORY = (1 << 1),
	VFS_STAT_IS_CHARACTER_SPECIAL = (1 << 2),
};

namespace
{
	struct VFSStream
	{
		void* handle = nullptr;
		std::FILE* fp = nullptr;
		bool append = false;
	};
} // namespace

static const FileSystem::VFS::Hooks* s_hooks = nullptr;
static std::mutex s_streams_mutex;
static std::unordered_map<std::FILE*, VFSStream*> s_streams;

void FileSystem::VFS::SetHooks(const Hooks* hooks)
{
	s_hooks = hooks;
}

bool FileSystem::VFS::IsActive()
{
#ifdef VFS_STREAMS_SUPPORTED
	return (s_hooks && s_hooks->open && s_hooks->close && s_hooks->read && s_hooks->seek && s_hooks->tell);
#else
	return false;
#endif
}

bool FileSystem::VFS::HasPathOps()
{
	return (IsActive() && s_hooks->stat && s_hooks->opendir && s_hooks->readdir && s_hooks->dirent_get_name &&
			s_hooks->dirent_is_dir && s_hooks->closedir && s_hooks->mkdir);
}

#ifdef VFS_STREAMS_SUPPORTED

static VFSStream* LookupStream(std::FILE* fp)
{
	std::unique_lock lock(s_streams_mutex);
	const auto it = s_streams.find(fp);
	return (it != s_streams.end()) ? it->second : nullptr;
}

static s64 StreamRead(VFSStream* stream, void* buffer, size_t size)
{
	return s_hooks->read(stream->handle, buffer, static_cast<u64>(size));
}

static s64 StreamWrite(VFSStream* stream, const void* buffer, size_t size)
{
	if (!s_hooks->write)
		return -1;

	// The frontend interface has no append mode, so a stream opened for
	// appending has to be positioned at the end before every write - the
	// emulator's log files are opened that way and would otherwise overwrite
	// themselves from wherever the last read left the position.
	if (stream->append)
		s_hooks->seek(stream->handle, 0, VFS_SEEK_END);

	return s_hooks->write(stream->handle, buffer, static_cast<u64>(size));
}

static s64 StreamSeek(VFSStream* stream, s64 offset, int whence)
{
	int vfs_whence;
	switch (whence)
	{
		case SEEK_SET:
			vfs_whence = VFS_SEEK_SET;
			break;
		case SEEK_CUR:
			vfs_whence = VFS_SEEK_CUR;
			break;
		case SEEK_END:
			vfs_whence = VFS_SEEK_END;
			break;
		default:
			return -1;
	}

	const s64 pos = s_hooks->seek(stream->handle, offset, vfs_whence);
	if (pos < 0)
		return -1;

	// Not every frontend returns the resulting position (the interface has
	// said both "0 on success" and "the new position" over its lifetime), so
	// ask for it rather than trusting the return value.
	const s64 real_pos = s_hooks->tell(stream->handle);
	return (real_pos >= 0) ? real_pos : pos;
}

static int StreamClose(VFSStream* stream)
{
	{
		std::unique_lock lock(s_streams_mutex);
		s_streams.erase(stream->fp);
	}

	const int res = s_hooks->close(stream->handle);
	delete stream;
	return (res == 0) ? 0 : -1;
}

#if defined(VFS_STREAMS_FOPENCOOKIE)

static ssize_t CookieRead(void* cookie, char* buffer, size_t size)
{
	const s64 res = StreamRead(static_cast<VFSStream*>(cookie), buffer, size);
	return (res < 0) ? -1 : static_cast<ssize_t>(res);
}

static ssize_t CookieWrite(void* cookie, const char* buffer, size_t size)
{
	const s64 res = StreamWrite(static_cast<VFSStream*>(cookie), buffer, size);
	// glibc treats a zero return from a non-empty write as an error condition
	// anyway; report it as one so the caller's ferror() sees it.
	return (res <= 0) ? -1 : static_cast<ssize_t>(res);
}

static int CookieSeek(void* cookie, off64_t* offset, int whence)
{
	const s64 pos = StreamSeek(static_cast<VFSStream*>(cookie), static_cast<s64>(*offset), whence);
	if (pos < 0)
		return -1;

	*offset = static_cast<off64_t>(pos);
	return 0;
}

static int CookieClose(void* cookie)
{
	return StreamClose(static_cast<VFSStream*>(cookie));
}

static std::FILE* WrapStream(VFSStream* stream, const char* stdio_mode)
{
	const cookie_io_functions_t functions = {&CookieRead, &CookieWrite, &CookieSeek, &CookieClose};
	return fopencookie(stream, stdio_mode, functions);
}

#else // funopen / funopen64

static int FunopenRead(void* cookie, char* buffer, int size)
{
	const s64 res = StreamRead(static_cast<VFSStream*>(cookie), buffer, static_cast<size_t>(size));
	return (res < 0) ? -1 : static_cast<int>(res);
}

static int FunopenWrite(void* cookie, const char* buffer, int size)
{
	const s64 res = StreamWrite(static_cast<VFSStream*>(cookie), buffer, static_cast<size_t>(size));
	return (res <= 0) ? -1 : static_cast<int>(res);
}

#if defined(VFS_STREAMS_FUNOPEN64)
using FunopenOffset = off64_t;
#else
using FunopenOffset = fpos_t;
#endif

static FunopenOffset FunopenSeek(void* cookie, FunopenOffset offset, int whence)
{
	const s64 pos = StreamSeek(static_cast<VFSStream*>(cookie), static_cast<s64>(offset), whence);
	return (pos < 0) ? static_cast<FunopenOffset>(-1) : static_cast<FunopenOffset>(pos);
}

static int FunopenClose(void* cookie)
{
	return StreamClose(static_cast<VFSStream*>(cookie));
}

static std::FILE* WrapStream(VFSStream* stream, const char* stdio_mode)
{
	// funopen has no mode string; read/write permission comes from which
	// callbacks are supplied, and the frontend handle already enforces it.
	(void)stdio_mode;
#if defined(VFS_STREAMS_FUNOPEN64)
	return funopen64(stream, &FunopenRead, &FunopenWrite, &FunopenSeek, &FunopenClose);
#else
	return funopen(stream, &FunopenRead, &FunopenWrite, &FunopenSeek, &FunopenClose);
#endif
}

#endif

// Turns an fopen() mode string into the frontend's access flags. Returns false
// for anything we should not be handling.
static bool TranslateMode(const char* mode, u32* access, bool* append, std::string* stdio_mode)
{
	bool read = false, write = false, update = false, plus = false;
	*append = false;

	for (const char* ch = mode; *ch != '\0'; ch++)
	{
		switch (*ch)
		{
			case 'r':
				read = true;
				break;
			case 'w':
				write = true;
				break;
			case 'a':
				write = true;
				update = true;
				*append = true;
				break;
			case '+':
				plus = true;
				break;
			case 'b':
			case 't':
			case 'e':
			case 'c':
			case 'm':
			case 'x':
			case 'S':
			case 'R':
			case 'N':
			case 'D':
			case 'T':
				// buffering, sharing and O_CLOEXEC style hints - the frontend
				// decides these for us
				break;
			default:
				return false;
		}
	}

	if (!read && !write)
		return false;

	if (plus)
	{
		// "r+" updates in place, "w+" still truncates, "a+" still appends
		*access = VFS_ACCESS_READ_WRITE;
		if (read)
			update = true;
	}
	else
	{
		*access = read ? VFS_ACCESS_READ : VFS_ACCESS_WRITE;
	}

	if (update)
		*access |= VFS_ACCESS_UPDATE_EXISTING;

	*stdio_mode = plus ? (read ? "r+" : (*append ? "a+" : "w+")) : (read ? "r" : (*append ? "a" : "w"));
	return true;
}

std::FILE* FileSystem::VFS::OpenFile(const char* path, const char* mode, Error* error)
{
	if (!IsActive())
		return nullptr;

	u32 access;
	bool append;
	std::string stdio_mode;
	if (!TranslateMode(mode, &access, &append, &stdio_mode))
	{
		Console.WarningFmt("VFS: unsupported file mode '{}' for '{}'.", mode, path);
		return nullptr;
	}

	void* handle = s_hooks->open(path, access, VFS_HINT_NONE);
	if (!handle)
	{
		// The frontend does not tell us why; ENOENT is the overwhelmingly
		// common reason and callers only use this for their message.
		Error::SetErrno(error, ENOENT);
		return nullptr;
	}

	VFSStream* stream = new VFSStream();
	stream->handle = handle;
	stream->append = append;

	std::FILE* fp = WrapStream(stream, stdio_mode.c_str());
	if (!fp)
	{
		s_hooks->close(handle);
		delete stream;
		Error::SetErrno(error, ENOMEM);
		return nullptr;
	}

	stream->fp = fp;

	{
		std::unique_lock lock(s_streams_mutex);
		s_streams.emplace(fp, stream);
	}

	if (append)
		s_hooks->seek(handle, 0, VFS_SEEK_END);

	return fp;
}

bool FileSystem::VFS::IsVFSStream(std::FILE* fp)
{
	return (fp && LookupStream(fp) != nullptr);
}

std::optional<s64> FileSystem::VFS::GetStreamSize(std::FILE* fp)
{
	VFSStream* stream = fp ? LookupStream(fp) : nullptr;
	if (!stream || !s_hooks->size)
		return std::nullopt;

	// Anything buffered on the stdio side has to reach the frontend first, or
	// it will report a size that is short by however much is still buffered.
	std::fflush(fp);

	const s64 size = s_hooks->size(stream->handle);
	return (size >= 0) ? std::optional<s64>(size) : std::nullopt;
}

#else // !VFS_STREAMS_SUPPORTED

std::FILE* FileSystem::VFS::OpenFile(const char* /*path*/, const char* /*mode*/, Error* /*error*/)
{
	return nullptr;
}

bool FileSystem::VFS::IsVFSStream(std::FILE* /*fp*/)
{
	return false;
}

std::optional<s64> FileSystem::VFS::GetStreamSize(std::FILE* /*fp*/)
{
	return std::nullopt;
}

#endif

static std::mutex s_mapped_files_mutex;
static std::unordered_map<const u8*, std::vector<u8>> s_mapped_files;

std::span<const u8> FileSystem::VFS::MapWholeFile(const char* path)
{
	if (!IsActive())
		return {};

	ManagedCFilePtr fp(OpenFile(path, "rb", nullptr));
	if (!fp)
		return {};

	const s64 size = FileSystem::FSize64(fp.get());
	if (size <= 0)
		return {};

	std::vector<u8> data(static_cast<size_t>(size));
	if (std::fread(data.data(), 1, data.size(), fp.get()) != data.size())
		return {};

	const u8* data_ptr = data.data();
	const size_t data_size = data.size();

	std::unique_lock lock(s_mapped_files_mutex);
	s_mapped_files.emplace(data_ptr, std::move(data));
	return std::span<const u8>(data_ptr, data_size);
}

std::span<const u8> FileSystem::VFS::MapWholeStream(std::FILE* fp)
{
	if (!IsVFSStream(fp))
		return {};

	const s64 size = FileSystem::FSize64(fp);
	if (size <= 0 || FileSystem::FSeek64(fp, 0, SEEK_SET) != 0)
		return {};

	std::vector<u8> data(static_cast<size_t>(size));
	if (std::fread(data.data(), 1, data.size(), fp) != data.size())
		return {};

	const u8* data_ptr = data.data();
	const size_t data_size = data.size();

	std::unique_lock lock(s_mapped_files_mutex);
	s_mapped_files.emplace(data_ptr, std::move(data));
	return std::span<const u8>(data_ptr, data_size);
}

bool FileSystem::VFS::UnmapWholeFile(std::span<const u8> span)
{
	std::unique_lock lock(s_mapped_files_mutex);
	return (s_mapped_files.erase(span.data()) > 0);
}

// Whether the caller needs the byte count to be right, or only the existence
// and directory bits out of the stat. Getting the size right can cost an extra
// open, so it is asked for rather than assumed.
enum class SizeAccuracy
{
	AsReported,
	Exact,
};

// Fills in a stat block from what the frontend can tell us. It has no notion of
// timestamps, so those stay zero: callers use them for cache freshness at
// worst, never for correctness.
static std::optional<bool> StatPathInternal(const char* path, FILESYSTEM_STAT_DATA* sd, SizeAccuracy accuracy)
{
	s64 size = 0;
	int flags;
	if (s_hooks->stat_64)
	{
		flags = s_hooks->stat_64(path, &size);
	}
	else
	{
		s32 size32 = 0;
		flags = s_hooks->stat(path, &size32);
		size = size32;
	}

	if (!(flags & VFS_STAT_IS_VALID))
		return false;

	sd->CreationTime = 0;
	sd->ModificationTime = 0;
	sd->Attributes = (flags & VFS_STAT_IS_DIRECTORY) ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0;
	sd->Size = 0;

	if (!(flags & (VFS_STAT_IS_DIRECTORY | VFS_STAT_IS_CHARACTER_SPECIAL)))
	{
		// The v3 stat only carries a 32-bit size, and PS2 media routinely goes
		// past that. A truncated size cannot be recognised by looking at it -
		// a 5 GB image comes back as a perfectly ordinary-looking 705 MB - so
		// whenever the number has to be right, take it from a handle, where
		// the interface is 64-bit.
		if (accuracy == SizeAccuracy::Exact && !s_hooks->stat_64 && s_hooks->size)
		{
			if (void* handle = s_hooks->open(path, VFS_ACCESS_READ, VFS_HINT_NONE))
			{
				const s64 handle_size = s_hooks->size(handle);
				s_hooks->close(handle);
				if (handle_size >= 0)
					size = handle_size;
			}
		}

		sd->Size = std::max<s64>(size, 0);
	}

	return true;
}

std::optional<bool> FileSystem::VFS::StatPath(const char* path, FILESYSTEM_STAT_DATA* sd)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	return StatPathInternal(path, sd, SizeAccuracy::Exact);
}

std::optional<bool> FileSystem::VFS::FileExists(const char* path)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	// Only the directory bit is read below, so the size need not be paid for.
	FILESYSTEM_STAT_DATA sd;
	if (!StatPathInternal(path, &sd, SizeAccuracy::AsReported).value_or(false))
		return false;

	return (sd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::optional<bool> FileSystem::VFS::DirectoryExists(const char* path)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	// Same as FileExists(): the size is never looked at.
	FILESYSTEM_STAT_DATA sd;
	if (!StatPathInternal(path, &sd, SizeAccuracy::AsReported).value_or(false))
		return false;

	return (sd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::optional<bool> FileSystem::VFS::DeleteFilePath(const char* path)
{
	if (!IsActive() || !s_hooks->remove || path[0] == '\0')
		return std::nullopt;

	return (s_hooks->remove(path) == 0);
}

std::optional<bool> FileSystem::VFS::RenamePath(const char* old_path, const char* new_path)
{
	if (!IsActive() || !s_hooks->rename || old_path[0] == '\0' || new_path[0] == '\0')
		return std::nullopt;

	return (s_hooks->rename(old_path, new_path) == 0);
}

std::optional<bool> FileSystem::VFS::DirectoryIsEmpty(const char* path)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	void* dir = s_hooks->opendir(path, true);
	if (!dir)
		return true;

	bool empty = true;
	while (s_hooks->readdir(dir))
	{
		const char* name = s_hooks->dirent_get_name(dir);
		if (!name || name[0] == '\0')
			continue;

		if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
			continue;

		empty = false;
		break;
	}

	s_hooks->closedir(dir);
	return empty;
}

std::optional<bool> FileSystem::VFS::CreateDirectoryPath(const char* path, bool recursive)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	// -2 is "already there", which every caller of this treats as success.
	const auto make = [](const std::string& dir) {
		const int res = s_hooks->mkdir(dir.c_str());
		return (res == 0 || res == -2);
	};

	const std::string full_path(path);
	if (make(full_path))
		return true;

	if (!recursive)
		return false;

	// mkdir() creates one level at a time, so walk down the path creating the
	// parents first. Trailing separators are skipped by the empty-segment test.
	std::string partial;
	partial.reserve(full_path.size());
	for (size_t i = 0; i < full_path.size(); i++)
	{
		if (i > 0 && (full_path[i] == '/' || full_path[i] == FS_OSPATH_SEPARATOR_CHARACTER) && !partial.empty())
		{
			if (!make(partial))
				return false;
		}

		partial.push_back(full_path[i]);
	}

	return make(full_path);
}

std::optional<bool> FileSystem::VFS::DeleteDirectory(const char* path)
{
	if (!IsActive() || !s_hooks->remove || path[0] == '\0')
		return std::nullopt;

	// The frontend interface has one removal call for both kinds of entry.
	return (s_hooks->remove(path) == 0);
}

static u32 RecursiveFindFilesVFS(const std::string& origin_path, const std::string& relative_path, const char* pattern,
	u32 flags, FileSystem::FindResultsArray* results, ProgressCallback* cancel)
{
	if (cancel && cancel->IsCancelled())
		return 0;

	const std::string search_path =
		relative_path.empty() ? origin_path : Path::Combine(origin_path, relative_path);

	void* dir = s_hooks->opendir(search_path.c_str(), (flags & FILESYSTEM_FIND_HIDDEN_FILES) != 0);
	if (!dir)
		return 0;

	bool has_wildcards = false;
	bool wildcard_match_all = false;
	if (std::strpbrk(pattern, "*?"))
	{
		has_wildcards = true;
		wildcard_match_all = (std::strcmp(pattern, "*") == 0);
	}

	u32 files = 0;
	while (s_hooks->readdir(dir))
	{
		const char* name = s_hooks->dirent_get_name(dir);
		if (!name || name[0] == '\0')
			continue;

		if (name[0] == '.')
		{
			if (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))
				continue;

			if (!(flags & FILESYSTEM_FIND_HIDDEN_FILES))
				continue;
		}

		const std::string entry_relative_path =
			relative_path.empty() ? std::string(name) : Path::Combine(relative_path, name);
		std::string full_path = Path::Combine(origin_path, entry_relative_path);

		FILESYSTEM_FIND_DATA out_data;
		out_data.Attributes = 0;
		out_data.Size = 0;
		out_data.CreationTime = 0;
		out_data.ModificationTime = 0;

		const bool is_directory = s_hooks->dirent_is_dir(dir);
		if (is_directory)
		{
			if (flags & FILESYSTEM_FIND_RECURSIVE)
			{
				files += RecursiveFindFilesVFS(origin_path, entry_relative_path, pattern, flags, results, cancel);
			}

			if (!(flags & FILESYSTEM_FIND_FOLDERS))
				continue;

			out_data.Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;
		}
		else
		{
			if (!(flags & FILESYSTEM_FIND_FILES))
				continue;

			// Whatever the frontend says, without opening every entry to check
			// it: the sizes out of a listing are read for memory cards and
			// BIOS images, both orders of magnitude below the 32-bit ceiling,
			// and a directory of a few hundred files would otherwise cost that
			// many opens.
			FILESYSTEM_STAT_DATA sd;
			if (StatPathInternal(full_path.c_str(), &sd, SizeAccuracy::AsReported).value_or(false))
			{
				out_data.Size = sd.Size;
				out_data.CreationTime = sd.CreationTime;
				out_data.ModificationTime = sd.ModificationTime;
			}
		}

		if (has_wildcards)
		{
			if (!wildcard_match_all && !StringUtil::WildcardMatch(name, pattern))
				continue;
		}
		else
		{
			if (std::strcmp(name, pattern) != 0)
				continue;
		}

		out_data.FileName = (flags & FILESYSTEM_FIND_RELATIVE_PATHS) ? entry_relative_path : std::move(full_path);

		files++;
		results->push_back(std::move(out_data));
	}

	s_hooks->closedir(dir);
	return files;
}

std::optional<bool> FileSystem::VFS::FindFiles(
	const char* path, const char* pattern, u32 flags, FindResultsArray* results, ProgressCallback* cancel)
{
	if (!HasPathOps() || path[0] == '\0')
		return std::nullopt;

	if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
		results->clear();

	// No symlink loop guard: the frontend interface cannot resolve links, and
	// it does not report them either, so a link cycle below a VFS root would
	// only be caught by the directory depth running out. Nothing the core
	// scans (bios, memory cards, patches) is expected to contain one.
	if (RecursiveFindFilesVFS(path, std::string(), pattern, flags, results, cancel) == 0)
		return false;

	if (flags & FILESYSTEM_FIND_SORT_BY_NAME)
	{
		std::sort(results->begin(), results->end(), [](const FILESYSTEM_FIND_DATA& lhs, const FILESYSTEM_FIND_DATA& rhs) {
			if ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) !=
				(rhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY))
			{
				return ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) != 0);
			}

			return (StringUtil::Strcasecmp(lhs.FileName.c_str(), rhs.FileName.c_str()) < 0);
		});
	}

	return true;
}
