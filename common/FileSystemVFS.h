// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "FileSystem.h"

// Optional indirection layer for FileSystem.
//
// A host (today: the libretro core, from the frontend's
// RETRO_ENVIRONMENT_GET_VFS_INTERFACE) can install a table of file callbacks
// here; FileSystem then performs its path-based work through them instead of
// the platform's own calls. That is what makes the emulator able to touch
// content the process cannot open by itself - most importantly Android's
// storage-access-framework handles, where the frontend holds the only usable
// reference to the file.
//
// Nothing is installed by default, and with nothing installed every entry
// point below reports "not handled" so FileSystem keeps its native behaviour.

namespace FileSystem::VFS
{
	// Mirrors the subset of retro_vfs_interface that FileSystem can express,
	// with the frontend's opaque handles reduced to void*. The hosting layer
	// supplies adapters rather than casting function pointers, so a frontend
	// interface change cannot silently produce a bad call here.
	struct Hooks
	{
		// Directory and stat entries were added in VFS API v3; when the
		// frontend offers something older they are left null and the native
		// implementation keeps handling those.
		void* (*open)(const char* path, u32 mode, u32 hints);
		int (*close)(void* handle);
		s64 (*size)(void* handle);
		s64 (*tell)(void* handle);
		s64 (*seek)(void* handle, s64 offset, int whence);
		s64 (*read)(void* handle, void* buffer, u64 length);
		s64 (*write)(void* handle, const void* buffer, u64 length);
		int (*flush)(void* handle);
		int (*truncate)(void* handle, s64 length);
		int (*remove)(const char* path);
		int (*rename)(const char* old_path, const char* new_path);

		int (*stat)(const char* path, s32* size);
		int (*stat_64)(const char* path, s64* size); // VFS API v4, may be null
		int (*mkdir)(const char* path);
		void* (*opendir)(const char* path, bool include_hidden);
		bool (*readdir)(void* handle);
		const char* (*dirent_get_name)(void* handle);
		bool (*dirent_is_dir)(void* handle);
		int (*closedir)(void* handle);
	};

	/// Installs the callback table, or removes it again when given nullptr.
	/// The table is only read, and has to outlive the process' use of it.
	void SetHooks(const Hooks* hooks);

	/// True when file access goes through the installed hooks.
	bool IsActive();

	/// True when the installed hooks also cover stat and directory access.
	bool HasPathOps();

	/// Opens a file through the hooks, wrapped in a std::FILE* so that the
	/// rest of the emulator can keep using stdio on it. Returns nullptr when
	/// VFS is inactive or the frontend refused the file.
	std::FILE* OpenFile(const char* path, const char* mode, Error* error);

	/// True if the stream came from OpenFile() above.
	bool IsVFSStream(std::FILE* fp);

	/// Size of a VFS-backed stream, which has no descriptor to fstat().
	std::optional<s64> GetStreamSize(std::FILE* fp);

	/// Stand-in for mapping a file, for the callers that would otherwise mmap a
	/// descriptor the frontend never gave us: the contents are read into memory
	/// and handed out as a span. Empty when VFS is inactive or the read failed.
	std::span<const u8> MapWholeFile(const char* path);

	/// As above, for a stream that is already open through the hooks. Empty
	/// when the stream is not one of ours.
	std::span<const u8> MapWholeStream(std::FILE* fp);

	/// Releases a span from MapWholeFile(); false if it did not come from there,
	/// in which case the caller still has to unmap it itself.
	bool UnmapWholeFile(std::span<const u8> span);

	// Path-based operations. Each returns std::nullopt when VFS is not
	// handling this (no hooks, or the frontend is too old), which means the
	// caller has to fall through to its native implementation.
	std::optional<bool> StatPath(const char* path, FILESYSTEM_STAT_DATA* sd);
	std::optional<bool> FileExists(const char* path);
	std::optional<bool> DirectoryExists(const char* path);
	std::optional<bool> DirectoryIsEmpty(const char* path);
	std::optional<bool> DeleteFilePath(const char* path);
	std::optional<bool> RenamePath(const char* old_path, const char* new_path);
	std::optional<bool> CreateDirectoryPath(const char* path, bool recursive);
	std::optional<bool> DeleteDirectory(const char* path);
	std::optional<bool> FindFiles(
		const char* path, const char* pattern, u32 flags, FindResultsArray* results, ProgressCallback* cancel);
} // namespace FileSystem::VFS
