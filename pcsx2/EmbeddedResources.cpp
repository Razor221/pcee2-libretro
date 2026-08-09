// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "EmbeddedResources.h"

#include <cstdlib>

bool EmbeddedResourcesPreferred()
{
	static const bool prefer_files = (std::getenv("PCEE2_EXTERNAL_RESOURCES") != nullptr);
	return !prefer_files;
}
