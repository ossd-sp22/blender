/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include "curves_sculpt_intern.hh"

#include "BLI_float4x4.hh"
#include "BLI_index_mask_ops.hh"
#include "BLI_kdtree.h"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "PIL_time.h"

#include "DEG_depsgraph.h"

#include "BKE_attribute_math.hh"
#include "BKE_brush.h"
#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_curves.hh"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_paint.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "UI_interface.h"

/**
 * The code below uses a prefix naming convention to indicate the coordinate space:
 * cu: Local space of the curves object that is being edited.
 * su: Local space of the surface object.
 * wo: World space.
 * re: 2D coordinates within the region.
 */

namespace blender::ed::sculpt_paint {

using blender::bke::CurvesGeometry;
using threading::EnumerableThreadSpecific;

/**
 * Moves individual points under the brush and does a length preservation step afterwards.
 */
class CombOperation : public CurvesSculptStrokeOperation {
 private:
  /** Last mouse position. */
  float2 brush_pos_last_re_;

  /** Only used when a 3D brush is used. */
  CurvesBrush3D brush_3d_;

  /** Length of each segment indexed by the index of the first point in the segment. */
  Array<float> segment_lengths_cu_;

  friend struct CombOperationExecutor;

 public:
  void on_stroke_extended(bContext *C, const StrokeExtension &stroke_extension) override;
};

/**
 * Utility class that actually executes the update when the stroke is updated. That's useful
 * because it avoids passing a very large number of parameters between functions.
 */
struct CombOperationExecutor {
  CombOperation *self_ = nullptr;
  bContext *C_ = nullptr;
  Depsgraph *depsgraph_ = nullptr;
  Scene *scene_ = nullptr;
  Object *object_ = nullptr;
  ARegion *region_ = nullptr;
  View3D *v3d_ = nullptr;
  RegionView3D *rv3d_ = nullptr;

  CurvesSculpt *curves_sculpt_ = nullptr;
  Brush *brush_ = nullptr;
  float brush_radius_re_;
  float brush_strength_;

  eBrushFalloffShape falloff_shape_;

  Curves *curves_id_ = nullptr;
  CurvesGeometry *curves_ = nullptr;

  const Object *surface_ob_ = nullptr;
  const Mesh *surface_ = nullptr;
  Span<MLoopTri> surface_looptris_;

  float2 brush_pos_prev_re_;
  float2 brush_pos_re_;
  float2 brush_pos_diff_re_;
  float brush_pos_diff_length_re_;

  float4x4 curves_to_world_mat_;
  float4x4 world_to_curves_mat_;
  float4x4 surface_to_world_mat_;
  float4x4 world_to_surface_mat_;

  BVHTreeFromMesh surface_bvh_;

  void execute(CombOperation &self, bContext *C, const StrokeExtension &stroke_extension)
  {
    self_ = &self;

    BLI_SCOPED_DEFER([&]() { self_->brush_pos_last_re_ = stroke_extension.mouse_position; });

    C_ = C;
    depsgraph_ = CTX_data_depsgraph_pointer(C);
    scene_ = CTX_data_scene(C);
    object_ = CTX_data_active_object(C);
    region_ = CTX_wm_region(C);
    v3d_ = CTX_wm_view3d(C);
    rv3d_ = CTX_wm_region_view3d(C);

    curves_sculpt_ = scene_->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush(&curves_sculpt_->paint);
    brush_radius_re_ = BKE_brush_size_get(scene_, brush_);
    brush_strength_ = BKE_brush_alpha_get(scene_, brush_);

    curves_to_world_mat_ = object_->obmat;
    world_to_curves_mat_ = curves_to_world_mat_.inverted();

    falloff_shape_ = static_cast<eBrushFalloffShape>(brush_->falloff_shape);

    curves_id_ = static_cast<Curves *>(object_->data);
    curves_ = &CurvesGeometry::wrap(curves_id_->geometry);

    brush_pos_prev_re_ = self_->brush_pos_last_re_;
    brush_pos_re_ = stroke_extension.mouse_position;
    brush_pos_diff_re_ = brush_pos_re_ - brush_pos_prev_re_;
    brush_pos_diff_length_re_ = math::length(brush_pos_diff_re_);

    surface_ob_ = curves_id_->surface;
    if (surface_ob_ != nullptr) {
      surface_ = static_cast<const Mesh *>(surface_ob_->data);
      surface_looptris_ = {BKE_mesh_runtime_looptri_ensure(surface_),
                           BKE_mesh_runtime_looptri_len(surface_)};
      surface_to_world_mat_ = surface_ob_->obmat;
      world_to_surface_mat_ = surface_to_world_mat_.inverted();
      BKE_bvhtree_from_mesh_get(&surface_bvh_, surface_, BVHTREE_FROM_LOOPTRI, 2);
    }

    BLI_SCOPED_DEFER([&]() {
      if (surface_ob_ != nullptr) {
        free_bvhtree_from_mesh(&surface_bvh_);
      }
    });

    if (stroke_extension.is_first) {
      if (falloff_shape_ == PAINT_FALLOFF_SHAPE_SPHERE) {
        this->initialize_spherical_brush_reference_point();
      }
      this->initialize_segment_lengths();
      /* Combing does nothing when there is no mouse movement, so return directly. */
      return;
    }

    EnumerableThreadSpecific<Vector<int>> changed_curves;

    if (falloff_shape_ == PAINT_FALLOFF_SHAPE_TUBE) {
      this->comb_projected(changed_curves);
    }
    else if (falloff_shape_ == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->comb_spherical(changed_curves);
    }
    else {
      BLI_assert_unreachable();
    }

    this->restore_segment_lengths(changed_curves);

    curves_->tag_positions_changed();
    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    ED_region_tag_redraw(region_);
  }

  /**
   * Do combing in screen space.
   */
  void comb_projected(EnumerableThreadSpecific<Vector<int>> &r_changed_curves)
  {
    MutableSpan<float3> positions_cu = curves_->positions_for_write();

    float4x4 projection;
    ED_view3d_ob_project_mat_get(rv3d_, object_, projection.values);

    const float brush_radius_sq_re = pow2f(brush_radius_re_);

    threading::parallel_for(curves_->curves_range(), 256, [&](const IndexRange curves_range) {
      Vector<int> &local_changed_curves = r_changed_curves.local();
      for (const int curve_i : curves_range) {
        bool curve_changed = false;
        const IndexRange points = curves_->points_for_curve(curve_i);
        for (const int point_i : points.drop_front(1)) {
          const float3 old_pos_cu = positions_cu[point_i];

          /* Find the position of the point in screen space. */
          float2 old_pos_re;
          ED_view3d_project_float_v2_m4(region_, old_pos_cu, old_pos_re, projection.values);

          const float distance_to_brush_sq_re = dist_squared_to_line_segment_v2(
              old_pos_re, brush_pos_prev_re_, brush_pos_re_);
          if (distance_to_brush_sq_re > brush_radius_sq_re) {
            /* Ignore the point because it's too far away. */
            continue;
          }

          const float distance_to_brush_re = std::sqrt(distance_to_brush_sq_re);
          /* A falloff that is based on how far away the point is from the stroke. */
          const float radius_falloff = BKE_brush_curve_strength(
              brush_, distance_to_brush_re, brush_radius_re_);
          /* Combine the falloff and brush strength. */
          const float weight = brush_strength_ * radius_falloff;

          /* Offset the old point position in screen space and transform it back into 3D space. */
          const float2 new_position_re = old_pos_re + brush_pos_diff_re_ * weight;
          float3 new_position_wo;
          ED_view3d_win_to_3d(
              v3d_, region_, curves_to_world_mat_ * old_pos_cu, new_position_re, new_position_wo);
          const float3 new_position_cu = world_to_curves_mat_ * new_position_wo;
          positions_cu[point_i] = new_position_cu;

          curve_changed = true;
        }
        if (curve_changed) {
          local_changed_curves.append(curve_i);
        }
      }
    });
  }

  /**
   * Do combing in 3D space.
   */
  void comb_spherical(EnumerableThreadSpecific<Vector<int>> &r_changed_curves)
  {
    MutableSpan<float3> positions_cu = curves_->positions_for_write();

    float4x4 projection;
    ED_view3d_ob_project_mat_get(rv3d_, object_, projection.values);

    float3 brush_start_wo, brush_end_wo;
    ED_view3d_win_to_3d(v3d_,
                        region_,
                        curves_to_world_mat_ * self_->brush_3d_.position_cu,
                        brush_pos_prev_re_,
                        brush_start_wo);
    ED_view3d_win_to_3d(v3d_,
                        region_,
                        curves_to_world_mat_ * self_->brush_3d_.position_cu,
                        brush_pos_re_,
                        brush_end_wo);
    const float3 brush_start_cu = world_to_curves_mat_ * brush_start_wo;
    const float3 brush_end_cu = world_to_curves_mat_ * brush_end_wo;

    const float3 brush_diff_cu = brush_end_cu - brush_start_cu;

    const float brush_radius_cu = self_->brush_3d_.radius_cu;
    const float brush_radius_sq_cu = pow2f(brush_radius_cu);

    threading::parallel_for(curves_->curves_range(), 256, [&](const IndexRange curves_range) {
      Vector<int> &local_changed_curves = r_changed_curves.local();
      for (const int curve_i : curves_range) {
        bool curve_changed = false;
        const IndexRange points = curves_->points_for_curve(curve_i);
        for (const int point_i : points.drop_front(1)) {
          const float3 pos_old_cu = positions_cu[point_i];

          /* Compute distance to the brush. */
          const float distance_to_brush_sq_cu = dist_squared_to_line_segment_v3(
              pos_old_cu, brush_start_cu, brush_end_cu);
          if (distance_to_brush_sq_cu > brush_radius_sq_cu) {
            /* Ignore the point because it's too far away. */
            continue;
          }

          const float distance_to_brush_cu = std::sqrt(distance_to_brush_sq_cu);

          /* A falloff that is based on how far away the point is from the stroke. */
          const float radius_falloff = BKE_brush_curve_strength(
              brush_, distance_to_brush_cu, brush_radius_cu);
          /* Combine the falloff and brush strength. */
          const float weight = brush_strength_ * radius_falloff;

          /* Update the point position. */
          positions_cu[point_i] = pos_old_cu + weight * brush_diff_cu;
          curve_changed = true;
        }
        if (curve_changed) {
          local_changed_curves.append(curve_i);
        }
      }
    });
  }

  /**
   * Sample depth under mouse by looking at curves and the surface.
   */
  void initialize_spherical_brush_reference_point()
  {
    std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(
        *C_, *object_, brush_pos_re_, brush_radius_re_);
    if (brush_3d.has_value()) {
      self_->brush_3d_ = *brush_3d;
    }
  }

  /**
   * Remember the initial length of all curve segments. This allows restoring the length after
   * combing.
   */
  void initialize_segment_lengths()
  {
    const Span<float3> positions_cu = curves_->positions();
    self_->segment_lengths_cu_.reinitialize(curves_->points_num());
    threading::parallel_for(curves_->curves_range(), 128, [&](const IndexRange range) {
      for (const int curve_i : range) {
        const IndexRange points = curves_->points_for_curve(curve_i);
        for (const int point_i : points.drop_back(1)) {
          const float3 &p1_cu = positions_cu[point_i];
          const float3 &p2_cu = positions_cu[point_i + 1];
          const float length_cu = math::distance(p1_cu, p2_cu);
          self_->segment_lengths_cu_[point_i] = length_cu;
        }
      }
    });
  }

  /**
   * Restore previously stored length for each segment in the changed curves.
   */
  void restore_segment_lengths(EnumerableThreadSpecific<Vector<int>> &changed_curves)
  {
    const Span<float> expected_lengths_cu = self_->segment_lengths_cu_;
    MutableSpan<float3> positions_cu = curves_->positions_for_write();

    threading::parallel_for_each(changed_curves, [&](const Vector<int> &changed_curves) {
      threading::parallel_for(changed_curves.index_range(), 256, [&](const IndexRange range) {
        for (const int curve_i : changed_curves.as_span().slice(range)) {
          const IndexRange points = curves_->points_for_curve(curve_i);
          for (const int segment_i : IndexRange(points.size() - 1)) {
            const float3 &p1_cu = positions_cu[points[segment_i]];
            float3 &p2_cu = positions_cu[points[segment_i] + 1];
            const float3 direction = math::normalize(p2_cu - p1_cu);
            const float expected_length_cu = expected_lengths_cu[points[segment_i]];
            p2_cu = p1_cu + direction * expected_length_cu;
          }
        }
      });
    });
  }
};

void CombOperation::on_stroke_extended(bContext *C, const StrokeExtension &stroke_extension)
{
  CombOperationExecutor executor;
  executor.execute(*this, C, stroke_extension);
}

std::unique_ptr<CurvesSculptStrokeOperation> new_comb_operation()
{
  return std::make_unique<CombOperation>();
}

}  // namespace blender::ed::sculpt_paint
