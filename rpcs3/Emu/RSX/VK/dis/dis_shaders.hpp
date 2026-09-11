// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "util/types.hpp"

#include <vector>

namespace dis
{
	enum class DisShader
	{
		luma_r16,
		luma_r32,
		gradient,
		inverse_search,
		propagate,
		densify,
		interpolate,
		vr_prep,
		vr_d1,
		vr_d2,
		vr_w,
		vr_coef,
		vr_sor,
		vr_add
	};

	const std::vector<u32>* GetDisShader(DisShader id);
}
