// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Hooks the frontend's virtual file system into common/FileSystem.
//
// RetroArch on Android hands the core content it opened through the storage
// access framework: the path in retro_game_info is a frontend-side handle, and
// nothing but the frontend can open it. The same goes for a system or save
// directory the user picked through the document picker. Asking for
// RETRO_ENVIRONMENT_GET_VFS_INTERFACE and routing file access back through it
// is what makes those work; on desktop frontends the interface is a thin
// wrapper over the platform calls, so behaviour there is unchanged.
//
// The core keeps doing its own file access when the frontend has no VFS
// interface, or when PCEE2_NO_VFS=1 is set in the environment.

#include "LibretroVFS.h"

#include "common/Console.h"
#include "common/FileSystemVFS.h"

#include "libretro.h"

#include "fmt/format.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	// The frontend's interface, and the hook table pointing at the adapters
	// below. Both outlive the core: FileSystem keeps the pointer.
	struct retro_vfs_interface* s_vfs = nullptr;
	u32 s_vfs_version = 0;
	FileSystem::VFS::Hooks s_hooks = {};

	// The interface has to be picked up in retro_set_environment, before the
	// frontend log sink is in place, so what happened is reported later.
	std::string s_status;

	retro_vfs_file_handle* AsFile(void* handle) { return static_cast<retro_vfs_file_handle*>(handle); }
	retro_vfs_dir_handle* AsDir(void* handle) { return static_cast<retro_vfs_dir_handle*>(handle); }

	void* VFSOpen(const char* path, u32 mode, u32 hints) { return s_vfs->open(path, mode, hints); }
	int VFSClose(void* handle) { return s_vfs->close(AsFile(handle)); }
	s64 VFSSize(void* handle) { return s_vfs->size(AsFile(handle)); }
	s64 VFSTell(void* handle) { return s_vfs->tell(AsFile(handle)); }
	s64 VFSSeek(void* handle, s64 offset, int whence) { return s_vfs->seek(AsFile(handle), offset, whence); }
	s64 VFSRead(void* handle, void* buffer, u64 length) { return s_vfs->read(AsFile(handle), buffer, length); }
	s64 VFSWrite(void* handle, const void* buffer, u64 length) { return s_vfs->write(AsFile(handle), buffer, length); }
	int VFSFlush(void* handle) { return s_vfs->flush(AsFile(handle)); }
	int VFSTruncate(void* handle, s64 length) { return static_cast<int>(s_vfs->truncate(AsFile(handle), length)); }
	int VFSRemove(const char* path) { return s_vfs->remove(path); }
	int VFSRename(const char* old_path, const char* new_path) { return s_vfs->rename(old_path, new_path); }

	int VFSStat(const char* path, s32* size) { return s_vfs->stat(path, size); }
	int VFSStat64(const char* path, s64* size) { return s_vfs->stat_64(path, size); }
	int VFSMkdir(const char* path) { return s_vfs->mkdir(path); }
	void* VFSOpendir(const char* path, bool include_hidden) { return s_vfs->opendir(path, include_hidden); }
	bool VFSReaddir(void* handle) { return s_vfs->readdir(AsDir(handle)); }
	const char* VFSDirentGetName(void* handle) { return s_vfs->dirent_get_name(AsDir(handle)); }
	bool VFSDirentIsDir(void* handle) { return s_vfs->dirent_is_dir(AsDir(handle)); }
	int VFSClosedir(void* handle) { return s_vfs->closedir(AsDir(handle)); }
} // namespace

void LibretroHost::InitializeVFS(retro_environment_t environ_cb)
{
	if (s_vfs)
		return;

	if (const char* disable = std::getenv("PCEE2_NO_VFS"); disable && std::strcmp(disable, "0") != 0)
	{
		s_status = "PCEE2_NO_VFS is set, using the core's own file access.";
		return;
	}

	// v3 is what makes stat and directory enumeration available; anything
	// older still gets us file access, which is the part that matters for
	// content the core cannot open itself.
	retro_vfs_interface_info info{};
	info.required_interface_version = 3;
	info.iface = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info) || !info.iface)
	{
		info.required_interface_version = 1;
		info.iface = nullptr;
		if (!environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info) || !info.iface)
		{
			s_status = "Frontend has no VFS interface, using the core's own file access.";
			return;
		}
	}

	s_vfs = info.iface;
	s_vfs_version = info.required_interface_version;

	if (!s_vfs->open || !s_vfs->close || !s_vfs->read || !s_vfs->seek || !s_vfs->tell)
	{
		s_status = "Frontend VFS interface is missing basic file operations, ignoring it.";
		s_vfs = nullptr;
		return;
	}

	s_hooks.open = &VFSOpen;
	s_hooks.close = &VFSClose;
	s_hooks.size = s_vfs->size ? &VFSSize : nullptr;
	s_hooks.tell = &VFSTell;
	s_hooks.seek = &VFSSeek;
	s_hooks.read = &VFSRead;
	s_hooks.write = s_vfs->write ? &VFSWrite : nullptr;
	s_hooks.flush = s_vfs->flush ? &VFSFlush : nullptr;
	s_hooks.truncate = (s_vfs_version >= 2 && s_vfs->truncate) ? &VFSTruncate : nullptr;
	s_hooks.remove = s_vfs->remove ? &VFSRemove : nullptr;
	s_hooks.rename = s_vfs->rename ? &VFSRename : nullptr;

	if (s_vfs_version >= 3)
	{
		s_hooks.stat = s_vfs->stat ? &VFSStat : nullptr;
		s_hooks.mkdir = s_vfs->mkdir ? &VFSMkdir : nullptr;
		s_hooks.opendir = s_vfs->opendir ? &VFSOpendir : nullptr;
		s_hooks.readdir = s_vfs->readdir ? &VFSReaddir : nullptr;
		s_hooks.dirent_get_name = s_vfs->dirent_get_name ? &VFSDirentGetName : nullptr;
		s_hooks.dirent_is_dir = s_vfs->dirent_is_dir ? &VFSDirentIsDir : nullptr;
		s_hooks.closedir = s_vfs->closedir ? &VFSClosedir : nullptr;
	}

	// stat_64 arrived in v4 and is what keeps the size of a DVD image intact;
	// the v3 call reports it through an int32.
	if (s_vfs_version >= 4)
		s_hooks.stat_64 = s_vfs->stat_64 ? &VFSStat64 : nullptr;

	FileSystem::VFS::SetHooks(&s_hooks);

	if (!FileSystem::VFS::IsActive())
	{
		// No stdio stream can be built around the frontend's handles on this
		// platform, so the hooks would only be half-used - drop them.
		FileSystem::VFS::SetHooks(nullptr);
		s_vfs = nullptr;
		s_status = "Frontend VFS interface cannot be used on this platform, using the core's own file access.";
		return;
	}

	s_status = fmt::format("Using the frontend VFS interface (version {}{}).", s_vfs_version,
		FileSystem::VFS::HasPathOps() ? "" : ", file access only");
}

void LibretroHost::LogVFSStatus()
{
	if (!s_status.empty())
		Console.WriteLn(s_status.c_str());
}
