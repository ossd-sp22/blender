/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved.
 *           2003-2009 Blender Foundation.
 *           2005-2006 Peter Schlaile <peter [at] schlaile [dot] de> */

/** \file
 * \ingroup bke
 */

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_movieclip.h"
#include "BKE_scene.h"
#include "BKE_sound.h"

#include "DNA_sound_types.h"
#include "IMB_imbuf.h"

#include "SEQ_channels.h"
#include "SEQ_iterator.h"
#include "SEQ_render.h"
#include "SEQ_sequencer.h"
#include "SEQ_time.h"
#include "SEQ_transform.h"

#include "strip_time.h"
#include "utils.h"

float seq_give_frame_index(Sequence *seq, float timeline_frame)
{
  float frame_index;
  int sta = seq->start;
  int end = seq->start + seq->len - 1;

  if (seq->type & SEQ_TYPE_EFFECT) {
    end = seq->enddisp;
  }

  if (end < sta) {
    return -1;
  }

  if (seq->flag & SEQ_REVERSE_FRAMES) {
    /* Reverse frame in this sequence. */
    if (timeline_frame <= sta) {
      frame_index = end - sta;
    }
    else if (timeline_frame >= end) {
      frame_index = 0;
    }
    else {
      frame_index = end - timeline_frame;
    }
  }
  else {
    if (timeline_frame <= sta) {
      frame_index = 0;
    }
    else if (timeline_frame >= end) {
      frame_index = end - sta;
    }
    else {
      frame_index = timeline_frame - sta;
    }
  }

  if (seq->strobe < 1.0f) {
    seq->strobe = 1.0f;
  }

  if (seq->strobe > 1.0f) {
    frame_index -= fmodf((double)frame_index, (double)seq->strobe);
  }

  return frame_index;
}

static int metaseq_start(Sequence *metaseq)
{
  return metaseq->start + metaseq->startofs;
}

static int metaseq_end(Sequence *metaseq)
{
  return metaseq->start + metaseq->len - metaseq->endofs;
}

static void seq_update_sound_bounds_recursive_impl(Scene *scene,
                                                   Sequence *metaseq,
                                                   int start,
                                                   int end)
{
  Sequence *seq;

  /* For sound we go over full meta tree to update bounds of the sound strips,
   * since sound is played outside of evaluating the imbufs. */
  for (seq = metaseq->seqbase.first; seq; seq = seq->next) {
    if (seq->type == SEQ_TYPE_META) {
      seq_update_sound_bounds_recursive_impl(
          scene, seq, max_ii(start, metaseq_start(seq)), min_ii(end, metaseq_end(seq)));
    }
    else if (ELEM(seq->type, SEQ_TYPE_SOUND_RAM, SEQ_TYPE_SCENE)) {
      if (seq->scene_sound) {
        int startofs = seq->startofs;
        int endofs = seq->endofs;
        if (seq->startofs + seq->start < start) {
          startofs = start - seq->start;
        }

        if (seq->start + seq->len - seq->endofs > end) {
          endofs = seq->start + seq->len - end;
        }

        double offset_time = 0.0f;
        if (seq->sound != NULL) {
          offset_time = seq->sound->offset_time;
        }

        BKE_sound_move_scene_sound(scene,
                                   seq->scene_sound,
                                   seq->start + startofs,
                                   seq->start + seq->len - endofs,
                                   startofs + seq->anim_startofs,
                                   offset_time);
      }
    }
  }
}

void seq_update_sound_bounds_recursive(Scene *scene, Sequence *metaseq)
{
  seq_update_sound_bounds_recursive_impl(
      scene, metaseq, metaseq_start(metaseq), metaseq_end(metaseq));
}

static void seq_time_update_sequence_bounds(Scene *scene, Sequence *seq)
{
  if (seq->startofs && seq->startstill) {
    seq->startstill = 0;
  }
  if (seq->endofs && seq->endstill) {
    seq->endstill = 0;
  }

  seq->startdisp = seq->start + seq->startofs - seq->startstill;
  seq->enddisp = seq->start + seq->len - seq->endofs + seq->endstill;

  if (seq->type == SEQ_TYPE_META) {
    seq_update_sound_bounds_recursive(scene, seq);
  }
}

static void seq_time_update_meta_strip(Scene *scene, Sequence *seq_meta)
{
  if (BLI_listbase_is_empty(&seq_meta->seqbase)) {
    return;
  }

  int min = MAXFRAME * 2;
  int max = -MAXFRAME * 2;
  LISTBASE_FOREACH (Sequence *, seq, &seq_meta->seqbase) {
    min = min_ii(seq->startdisp, min);
    max = max_ii(seq->enddisp, max);
  }

  seq_meta->start = min + seq_meta->anim_startofs;
  seq_meta->len = max - min;
  seq_meta->len -= seq_meta->anim_startofs;
  seq_meta->len -= seq_meta->anim_endofs;

  seq_update_sound_bounds_recursive(scene, seq_meta);
}

void SEQ_time_update_meta_strip_range(Scene *scene, Sequence *seq_meta)
{
  if (seq_meta == NULL) {
    return;
  }

  seq_time_update_meta_strip(scene, seq_meta);

  /* Prevent meta-strip to move in timeline. */
  SEQ_transform_set_left_handle_frame(seq_meta, seq_meta->startdisp);
  SEQ_transform_set_right_handle_frame(seq_meta, seq_meta->enddisp);
}

void SEQ_time_update_sequence(Scene *scene, ListBase *seqbase, Sequence *seq)
{
  Sequence *seqm;

  /* Check all meta-strips recursively. */
  seqm = seq->seqbase.first;
  while (seqm) {
    if (seqm->seqbase.first) {
      SEQ_time_update_sequence(scene, &seqm->seqbase, seqm);
    }
    seqm = seqm->next;
  }

  /* effects and meta: automatic start and end */
  if (seq->type & SEQ_TYPE_EFFECT) {
    if (seq->seq1) {
      seq->startofs = seq->endofs = seq->startstill = seq->endstill = 0;
      if (seq->seq3) {
        seq->start = seq->startdisp = max_iii(
            seq->seq1->startdisp, seq->seq2->startdisp, seq->seq3->startdisp);
        seq->enddisp = min_iii(seq->seq1->enddisp, seq->seq2->enddisp, seq->seq3->enddisp);
      }
      else if (seq->seq2) {
        seq->start = seq->startdisp = max_ii(seq->seq1->startdisp, seq->seq2->startdisp);
        seq->enddisp = min_ii(seq->seq1->enddisp, seq->seq2->enddisp);
      }
      else {
        seq->start = seq->startdisp = seq->seq1->startdisp;
        seq->enddisp = seq->seq1->enddisp;
      }
      /* we can't help if strips don't overlap, it won't give useful results.
       * but at least ensure 'len' is never negative which causes bad bugs elsewhere. */
      if (seq->enddisp < seq->startdisp) {
        /* simple start/end swap */
        seq->start = seq->enddisp;
        seq->enddisp = seq->startdisp;
        seq->startdisp = seq->start;
        seq->flag |= SEQ_INVALID_EFFECT;
      }
      else {
        seq->flag &= ~SEQ_INVALID_EFFECT;
      }

      seq->len = seq->enddisp - seq->startdisp;
    }
    else {
      seq_time_update_sequence_bounds(scene, seq);
    }
  }
  else if (seq->type == SEQ_TYPE_META) {
    seq_time_update_meta_strip(scene, seq);
  }
  else {
    seq_time_update_sequence_bounds(scene, seq);
  }

  Editing *ed = SEQ_editing_get(scene);

  /* Strip is inside meta strip */
  if (seqbase != &ed->seqbase) {
    Sequence *meta = SEQ_get_meta_by_seqbase(&ed->seqbase, seqbase);
    SEQ_time_update_meta_strip_range(scene, meta);
  }

  seq_time_update_sequence_bounds(scene, seq);
}

static bool update_changed_seq_recurs(Scene *scene, Sequence *seq, Sequence *changed_seq)
{
  Sequence *subseq;
  bool do_update = false;

  /* recurse downwards to see if this seq depends on the changed seq */

  if (seq == NULL) {
    return false;
  }

  if (seq == changed_seq) {
    do_update = true;
  }

  for (subseq = seq->seqbase.first; subseq; subseq = subseq->next) {
    if (update_changed_seq_recurs(scene, subseq, changed_seq)) {
      do_update = true;
    }
  }

  if (seq->seq1) {
    if (update_changed_seq_recurs(scene, seq->seq1, changed_seq)) {
      do_update = true;
    }
  }
  if (seq->seq2 && (seq->seq2 != seq->seq1)) {
    if (update_changed_seq_recurs(scene, seq->seq2, changed_seq)) {
      do_update = true;
    }
  }
  if (seq->seq3 && (seq->seq3 != seq->seq1) && (seq->seq3 != seq->seq2)) {
    if (update_changed_seq_recurs(scene, seq->seq3, changed_seq)) {
      do_update = true;
    }
  }

  if (do_update) {
    ListBase *seqbase = SEQ_active_seqbase_get(SEQ_editing_get(scene));
    SEQ_time_update_sequence(scene, seqbase, seq);
  }

  return do_update;
}

void SEQ_time_update_recursive(Scene *scene, Sequence *changed_seq)
{
  Editing *ed = SEQ_editing_get(scene);
  Sequence *seq;

  if (ed == NULL) {
    return;
  }

  for (seq = ed->seqbase.first; seq; seq = seq->next) {
    update_changed_seq_recurs(scene, seq, changed_seq);
  }
}

int SEQ_time_find_next_prev_edit(Scene *scene,
                                 int timeline_frame,
                                 const short side,
                                 const bool do_skip_mute,
                                 const bool do_center,
                                 const bool do_unselected)
{
  Editing *ed = SEQ_editing_get(scene);
  ListBase *channels = SEQ_channels_displayed_get(ed);
  Sequence *seq;

  int dist, best_dist, best_frame = timeline_frame;
  int seq_frames[2], seq_frames_tot;

  /* In case where both is passed,
   * frame just finds the nearest end while frame_left the nearest start. */

  best_dist = MAXFRAME * 2;

  if (ed == NULL) {
    return timeline_frame;
  }

  for (seq = ed->seqbasep->first; seq; seq = seq->next) {
    int i;

    if (do_skip_mute && SEQ_render_is_muted(channels, seq)) {
      continue;
    }

    if (do_unselected && (seq->flag & SELECT)) {
      continue;
    }

    if (do_center) {
      seq_frames[0] = (seq->startdisp + seq->enddisp) / 2;
      seq_frames_tot = 1;
    }
    else {
      seq_frames[0] = seq->startdisp;
      seq_frames[1] = seq->enddisp;

      seq_frames_tot = 2;
    }

    for (i = 0; i < seq_frames_tot; i++) {
      const int seq_frame = seq_frames[i];

      dist = MAXFRAME * 2;

      switch (side) {
        case SEQ_SIDE_LEFT:
          if (seq_frame < timeline_frame) {
            dist = timeline_frame - seq_frame;
          }
          break;
        case SEQ_SIDE_RIGHT:
          if (seq_frame > timeline_frame) {
            dist = seq_frame - timeline_frame;
          }
          break;
        case SEQ_SIDE_BOTH:
          dist = abs(seq_frame - timeline_frame);
          break;
      }

      if (dist < best_dist) {
        best_frame = seq_frame;
        best_dist = dist;
      }
    }
  }

  return best_frame;
}

float SEQ_time_sequence_get_fps(Scene *scene, Sequence *seq)
{
  switch (seq->type) {
    case SEQ_TYPE_MOVIE: {
      seq_open_anim_file(scene, seq, true);
      if (BLI_listbase_is_empty(&seq->anims)) {
        return 0.0f;
      }
      StripAnim *strip_anim = seq->anims.first;
      if (strip_anim->anim == NULL) {
        return 0.0f;
      }
      short frs_sec;
      float frs_sec_base;
      if (IMB_anim_get_fps(strip_anim->anim, &frs_sec, &frs_sec_base, true)) {
        return (float)frs_sec / frs_sec_base;
      }
      break;
    }
    case SEQ_TYPE_MOVIECLIP:
      if (seq->clip != NULL) {
        return BKE_movieclip_get_fps(seq->clip);
      }
      break;
    case SEQ_TYPE_SCENE:
      if (seq->scene != NULL) {
        return (float)seq->scene->r.frs_sec / seq->scene->r.frs_sec_base;
      }
      break;
  }
  return 0.0f;
}

void SEQ_timeline_init_boundbox(const Scene *scene, rctf *rect)
{
  rect->xmin = scene->r.sfra;
  rect->xmax = scene->r.efra + 1;
  rect->ymin = 0.0f;
  rect->ymax = 8.0f;
}

void SEQ_timeline_expand_boundbox(const ListBase *seqbase, rctf *rect)
{
  if (seqbase == NULL) {
    return;
  }

  LISTBASE_FOREACH (Sequence *, seq, seqbase) {
    if (rect->xmin > seq->startdisp - 1) {
      rect->xmin = seq->startdisp - 1;
    }
    if (rect->xmax < seq->enddisp + 1) {
      rect->xmax = seq->enddisp + 1;
    }
    if (rect->ymax < seq->machine) {
      rect->ymax = seq->machine + 2;
    }
  }
}

void SEQ_timeline_boundbox(const Scene *scene, const ListBase *seqbase, rctf *rect)
{
  SEQ_timeline_init_boundbox(scene, rect);
  SEQ_timeline_expand_boundbox(seqbase, rect);
}

static bool strip_exists_at_frame(SeqCollection *all_strips, const int timeline_frame)
{
  Sequence *seq;
  SEQ_ITERATOR_FOREACH (seq, all_strips) {
    if (SEQ_time_strip_intersects_frame(seq, timeline_frame)) {
      return true;
    }
  }
  return false;
}

void seq_time_gap_info_get(const Scene *scene,
                           ListBase *seqbase,
                           const int initial_frame,
                           GapInfo *r_gap_info)
{
  rctf rectf;
  /* Get first and last frame. */
  SEQ_timeline_boundbox(scene, seqbase, &rectf);
  const int sfra = (int)rectf.xmin;
  const int efra = (int)rectf.xmax;
  int timeline_frame = initial_frame;
  r_gap_info->gap_exists = false;

  SeqCollection *collection = SEQ_query_all_strips(seqbase);

  if (!strip_exists_at_frame(collection, initial_frame)) {
    /* Search backward for gap_start_frame. */
    for (; timeline_frame >= sfra; timeline_frame--) {
      if (strip_exists_at_frame(collection, timeline_frame)) {
        break;
      }
    }
    r_gap_info->gap_start_frame = timeline_frame + 1;
    timeline_frame = initial_frame;
  }
  else {
    /* Search forward for gap_start_frame. */
    for (; timeline_frame <= efra; timeline_frame++) {
      if (!strip_exists_at_frame(collection, timeline_frame)) {
        r_gap_info->gap_start_frame = timeline_frame;
        break;
      }
    }
  }
  /* Search forward for gap_end_frame. */
  for (; timeline_frame <= efra; timeline_frame++) {
    if (strip_exists_at_frame(collection, timeline_frame)) {
      const int gap_end_frame = timeline_frame;
      r_gap_info->gap_length = gap_end_frame - r_gap_info->gap_start_frame;
      r_gap_info->gap_exists = true;
      break;
    }
  }
}

bool SEQ_time_strip_intersects_frame(const Sequence *seq, const int timeline_frame)
{
  return (seq->startdisp <= timeline_frame) && (seq->enddisp > timeline_frame);
}
