#pragma once

#include <cstring>

// Canonicalizes the one misspelled animation key in the retail bundle, in place.
inline bool pcCanonicalizeBundleAnimationPath(char* path)
{
	static constexpr char badLogoPath[]  = "cinemas/titles/logoMotioe/logo.dck";
	static constexpr char goodLogoPath[] = "cinemas/titles/logoMotion/logo.dck";
	static_assert(sizeof(badLogoPath) == sizeof(goodLogoPath), "logo path alias must be in-place");
	if (!path || std::strcmp(path, badLogoPath) != 0) {
		return false;
	}
	std::memcpy(path, goodLogoPath, sizeof(goodLogoPath));
	return true;
}
