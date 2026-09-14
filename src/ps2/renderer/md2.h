#pragma once
/* ================================================================================================
 * File: md2.h
 * Brief: MD2 "alias" entity model rendering: monsters, items, the view weapon.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/math/vec_mat.h"

namespace ps2::md2 {

void Init();

// Draws one MD2 entity: frustum-culled, keyframe-lerped between
// entity.oldframe and entity.frame by backlerp, and textured with its skin.
// 'viewProj' is the frame's world-to-clip transform. Call from the 3D pass,
// between gs::Begin/EndFrame.
void DrawAliasMD2Entity(const refdef_t & viewDef, const entity_t & entity, const math::Mat4 & viewProj);

} // namespace ps2::md2
