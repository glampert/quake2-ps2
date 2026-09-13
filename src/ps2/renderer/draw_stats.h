#pragma once
/* ================================================================================================
 * File: draw_stats.h
 * Brief: Per-frame draw counters for the 3D view, shared by the code that submits geometry
 *        and the debug overlay that reports it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::view {

// Counters for one RenderFrame, tracking what the 3D view walked, culled, clipped
// and submitted. Zeroed at the top of each RenderFrame; read by the
// ps2_show_drawstats overlay and the frame log.
struct DrawStats
{
    int nodesWalked;    // BSP nodes + leafs visited by the world walk.
    int surfaces;       // Opaque world surfaces drawn.
    int surfacesAlpha;  // Translucent surfaces deferred to the final alpha pass.
    int skyFaces;       // Skybox cube faces submitted (0-6).
    int trisDrawn;      // Triangles submitted to VU1 (after EE clipping).
    int trisClipped;    // Triangles re-cut against the VU clip volume.
    int trisCulled;     // Triangles dropped whole, entirely outside the view volume.
    int surfsUnclipped; // World surface gathers that skipped the clipper, counted once per pass.
    int boxesCulled;    // Whole meshes culled via bounding box checks.
    int drawBatches;    // VU1 triangle batches submitted (one or more per texture).
    int entities;       // Entity models drawn (after frustum culling).
    int particles;      // Particle billboards drawn.
    int dlights;        // Dynamic light flares drawn.
};

// Stats of the most recent RenderFrame; all zeros before the first 3D frame.
DrawStats & GetDrawStats();

} // namespace ps2::view
