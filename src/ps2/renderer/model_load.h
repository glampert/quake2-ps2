#pragma once
/* ================================================================================================
 * File: model_load.h
 * Brief: Loaders for the Quake 2 on-disk model formats (world map, sprite, md2).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::mod {

struct ModelInstance;

bool LoadBrushModel(ModelInstance & outModel, const void * modelData, int dataLenBytes);
bool LoadSpriteModel(ModelInstance & outModel, const void * modelData, int dataLenBytes);
bool LoadAliasMD2Model(ModelInstance & outModel, const void * modelData, int dataLenBytes);

} // namespace ps2::mod
