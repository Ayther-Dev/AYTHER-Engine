//! Sprite-animation timing and geometric tweening.
//!
//! This module turns observed pose hashes into ordered [`AnimationClip`] values
//! and interpolates authored transforms without depending on emulator state.

// ---------------------------------------------------------------------------
// animation.rs — the temporal layer of a sprite animation (C-S1).
//
// `anim_group_id` (see vram_sprite::AnimationGrouper) tells us *which* hashes
// belong to one animation, but it is an **unordered set with no timing**. An
// AnimationClip adds the missing dimension: the **ordered sequence of poses and
// how long each is held** — the backbone of an "Acción" in the authoring model.
//
// The builder (`AnimationClip::from_sequence`) is a pure function of an ordered
// per-frame pose sequence, deliberately decoupled from where that sequence comes
// from: the live per-slot ring buffer of the AnimationGrouper feeds it now, and
// the full `.arp` hash-history (exact, offline) will feed the same builder in the
// Lab. The HD **asset** is NOT part of this struct — the engine only *observes*
// the animation here; the asset is authoring/pack data (animations.toml).
// ---------------------------------------------------------------------------

use std::collections::HashSet;

/// One frame of an animation: a pose (frame hash) held for `duration_ticks`
/// emulator frames.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AnimFrame {
    /// Hash of the pose shown this frame (a metasprite/frame hash).
    pub pose: u64,
    /// How many emulator frames the pose is held (the observed duration).
    pub duration_ticks: u16,
}

/// An observed animation: the ordered poses of one cycle with their timing.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AnimationClip {
    /// Stable identity — the `anim_group_id` of the members. Lets the pack's
    /// `animations.toml` reference the clip and the runtime play it in phase.
    pub id: u64,
    /// The frames of **one** cycle, in observed order, with per-frame duration.
    pub frames: Vec<AnimFrame>,
    /// True when the sequence returns to its first pose (a looping cycle);
    /// false for a one-shot animation observed once.
    pub looping: bool,
}

impl AnimationClip {
    /// Build a clip from an ordered per-frame pose sequence (`seq`, oldest →
    /// newest). Returns `None` when there are fewer than two distinct poses —
    /// i.e. a static sprite, not an animation.
    ///
    /// Algorithm:
    ///   1. Run-length encode consecutive identical poses → runs `(pose, ticks)`.
    ///   2. Extract **one cycle**: from the first run up to the reappearance of
    ///      the first pose (or the whole sequence if it never reappears — a
    ///      one-shot). This assumes the *first* pose occurs once per cycle, which
    ///      holds for typical walk/run cycles and even ping-pong (A B C B).
    ///   3. Each frame's `duration_ticks` = the **average** run length of that
    ///      pose across the window, for stability against a truncated window.
    ///
    /// Window-boundary note: a rolling window may cut the first/last run mid-pose,
    /// biasing that run's length. Averaging over all occurrences softens it; the
    /// offline `.arp` path (full recording) has no truncation at all.
    pub fn from_sequence(id: u64, seq: &[u64]) -> Option<AnimationClip> {
        if seq.len() < 2 {
            return None;
        }

        // 1. Run-length encode.
        let mut runs: Vec<(u64, u32)> = Vec::new();
        for &pose in seq {
            match runs.last_mut() {
                Some(last) if last.0 == pose => last.1 += 1,
                _ => runs.push((pose, 1)),
            }
        }

        // A single distinct pose is a static sprite, not an animation.
        let distinct: HashSet<u64> = runs.iter().map(|(p, _)| *p).collect();
        if distinct.len() < 2 {
            return None;
        }

        // 2. Extract one cycle: first run → reappearance of the first pose.
        let first_pose = runs[0].0;
        let reappear = runs.iter().skip(1).position(|(p, _)| *p == first_pose);
        let looping = reappear.is_some();
        let cycle_len = reappear.map(|i| i + 1).unwrap_or(runs.len());

        // 3. Per-frame duration = average run length of that pose over the window.
        let frames = runs[..cycle_len]
            .iter()
            .map(|(pose, _)| {
                let (sum, cnt) = runs
                    .iter()
                    .filter(|(p, _)| p == pose)
                    .fold((0u32, 0u32), |(s, c), (_, len)| (s + len, c + 1));
                // Rounded average, clamped to ≥1 and into u16.
                let dur = ((sum + cnt / 2) / cnt).clamp(1, u16::MAX as u32) as u16;
                AnimFrame {
                    pose: *pose,
                    duration_ticks: dur,
                }
            })
            .collect();

        Some(AnimationClip {
            id,
            frames,
            looping,
        })
    }
}

// ---------------------------------------------------------------------------
// Geometric tween (authoring tween Level 1 — modelo-de-autoria §2.3).
//
// "Pop" (Level 0) shows the HD of the current keyframe and jumps to the next —
// so a 6-fps game animation stays choppy in HD. Level 1 keeps *one HD per
// keyframe* but interpolates the **transform** (the bbox position/size the HD is
// drawn at) from each keyframe toward the next across the ticks it is held, so
// the HD glides instead of popping — no extra art. The timeline is fully known
// offline (over the `.arp`), so we can look ahead to the next keyframe. N
// keyframes give **N-1** tween segments for a one-shot; a loop adds the wrap
// segment (last → first) for N.
// ---------------------------------------------------------------------------

/// The visual anchor a keyframe's HD asset is drawn at: the metasprite bbox
/// (top-left + size, in screen px). Interpolating it is the geometric tween.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Transform {
    /// Horizontal position of the asset's top-left corner, in screen pixels.
    pub x: f32,
    /// Vertical position of the asset's top-left corner, in screen pixels.
    pub y: f32,
    /// Rendered width, in screen pixels.
    pub w: f32,
    /// Rendered height, in screen pixels.
    pub h: f32,
}

impl Transform {
    /// Linear interpolate `a → b` by `t` (clamped to `[0,1]`).
    pub fn lerp(a: Transform, b: Transform, t: f32) -> Transform {
        let t = t.clamp(0.0, 1.0);
        Transform {
            x: a.x + (b.x - a.x) * t,
            y: a.y + (b.y - a.y) * t,
            w: a.w + (b.w - a.w) * t,
            h: a.h + (b.h - a.h) * t,
        }
    }
}

/// A geometric tween over an animation's keyframes: each key is a
/// `(transform, duration_ticks)`; sampling at any tick interpolates the
/// transform from the current key toward the next. Pure and deterministic.
#[derive(Clone, Debug)]
pub struct GeometricTween {
    keys: Vec<(Transform, u16)>,
    looping: bool,
    total: u32, // ticks in one cycle
}

impl GeometricTween {
    /// Build from keyframe transforms + their hold durations. `looping` wraps
    /// the last key back to the first. Returns None if there are no keys.
    pub fn new(keys: Vec<(Transform, u16)>, looping: bool) -> Option<Self> {
        if keys.is_empty() {
            return None;
        }
        let total = keys.iter().map(|(_, d)| *d as u32).sum();
        Some(Self {
            keys,
            looping,
            total,
        })
    }

    /// Pair a clip with the per-keyframe transforms (same order/length as
    /// `clip.frames`) to tween it. None on a length mismatch.
    pub fn from_clip(clip: &AnimationClip, transforms: &[Transform]) -> Option<Self> {
        if transforms.len() != clip.frames.len() {
            return None;
        }
        let keys = clip
            .frames
            .iter()
            .zip(transforms)
            .map(|(f, t)| (*t, f.duration_ticks))
            .collect();
        Self::new(keys, clip.looping)
    }

    /// Total ticks in one cycle.
    pub fn duration(&self) -> u32 {
        self.total
    }
    /// Returns the number of authored keyframes.
    pub fn key_count(&self) -> usize {
        self.keys.len()
    }

    /// Interpolated transform at `tick` within one cycle. For a one-shot, ticks
    /// at/after the end clamp to the last key (no wrap); for a loop they wrap and
    /// the last key tweens back to the first.
    pub fn sample(&self, tick: u32) -> Transform {
        let n = self.keys.len();
        if n == 1 || self.total == 0 {
            return self.keys[0].0;
        }

        let tick = if self.looping {
            tick % self.total
        } else {
            tick.min(self.total.saturating_sub(1))
        };

        // Walk the keys to find the one holding `tick`.
        let mut start = 0u32;
        for (i, (tf, dur)) in self.keys.iter().enumerate() {
            let d = (*dur as u32).max(1);
            if tick < start + d {
                let next = if i + 1 < n {
                    i + 1
                } else if self.looping {
                    0
                } else {
                    return *tf; // one-shot last key: hold, no forward tween
                };
                let t = (tick - start) as f32 / d as f32;
                return Transform::lerp(*tf, self.keys[next].0, t);
            }
            start += d;
        }
        self.keys[n - 1].0 // unreachable for a valid tick; clamp to last
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Expand `(pose, count)` runs into a flat per-frame sequence.
    fn seq(runs: &[(u64, usize)]) -> Vec<u64> {
        runs.iter()
            .flat_map(|&(p, n)| std::iter::repeat_n(p, n))
            .collect()
    }

    #[test]
    fn static_sprite_is_not_an_animation() {
        assert!(AnimationClip::from_sequence(1, &seq(&[(0xA, 10)])).is_none());
        assert!(AnimationClip::from_sequence(1, &[]).is_none());
        assert!(AnimationClip::from_sequence(1, &[0xA]).is_none());
    }

    #[test]
    fn looping_cycle_orders_and_times_frames() {
        // Two clean cycles of A(2) B(3) C(1).
        let s = seq(&[(0xA, 2), (0xB, 3), (0xC, 1), (0xA, 2), (0xB, 3), (0xC, 1)]);
        let clip = AnimationClip::from_sequence(0x42, &s).unwrap();

        assert_eq!(clip.id, 0x42);
        assert!(clip.looping);
        assert_eq!(
            clip.frames,
            vec![
                AnimFrame {
                    pose: 0xA,
                    duration_ticks: 2
                },
                AnimFrame {
                    pose: 0xB,
                    duration_ticks: 3
                },
                AnimFrame {
                    pose: 0xC,
                    duration_ticks: 1
                },
            ]
        );
    }

    #[test]
    fn one_shot_sequence_is_not_looping() {
        // A → B → C observed once, never returns to A.
        let s = seq(&[(0xA, 2), (0xB, 2), (0xC, 2)]);
        let clip = AnimationClip::from_sequence(7, &s).unwrap();
        assert!(!clip.looping);
        assert_eq!(
            clip.frames.iter().map(|f| f.pose).collect::<Vec<_>>(),
            vec![0xA, 0xB, 0xC]
        );
    }

    #[test]
    fn ping_pong_cycle_preserves_order() {
        // A B C B | A B C B — first pose A is unique per cycle, so the B repeat
        // inside the cycle is preserved.
        let s = seq(&[
            (0xA, 1),
            (0xB, 1),
            (0xC, 1),
            (0xB, 1),
            (0xA, 1),
            (0xB, 1),
            (0xC, 1),
            (0xB, 1),
        ]);
        let clip = AnimationClip::from_sequence(9, &s).unwrap();
        assert!(clip.looping);
        assert_eq!(
            clip.frames.iter().map(|f| f.pose).collect::<Vec<_>>(),
            vec![0xA, 0xB, 0xC, 0xB]
        );
    }

    #[test]
    fn duration_averages_across_the_window() {
        // A runs of 2 and 4 → average 3; B constant 1.
        let s = seq(&[(0xA, 2), (0xB, 1), (0xA, 4), (0xB, 1)]);
        let clip = AnimationClip::from_sequence(1, &s).unwrap();
        // Cycle is [A, B]; A averages (2+4)/2 = 3, B averages 1.
        assert_eq!(
            clip.frames[0],
            AnimFrame {
                pose: 0xA,
                duration_ticks: 3
            }
        );
        assert_eq!(
            clip.frames[1],
            AnimFrame {
                pose: 0xB,
                duration_ticks: 1
            }
        );
    }

    // ----- Geometric tween (Level 1) -----------------------------------------

    fn tf(x: f32, y: f32) -> Transform {
        Transform {
            x,
            y,
            w: 10.0,
            h: 10.0,
        }
    }

    #[test]
    fn lerp_midpoint_and_clamp() {
        let a = Transform {
            x: 0.0,
            y: 0.0,
            w: 10.0,
            h: 20.0,
        };
        let b = Transform {
            x: 10.0,
            y: 40.0,
            w: 30.0,
            h: 20.0,
        };
        assert_eq!(
            Transform::lerp(a, b, 0.5),
            Transform {
                x: 5.0,
                y: 20.0,
                w: 20.0,
                h: 20.0
            }
        );
        assert_eq!(Transform::lerp(a, b, -1.0), a); // clamps low
        assert_eq!(Transform::lerp(a, b, 2.0), b); // clamps high
    }

    #[test]
    fn tween_interpolates_position_across_a_held_keyframe() {
        // Key0 at x=0 held 4 ticks → key1 at x=8. Midway (tick 2) → x=4.
        let t = GeometricTween::new(vec![(tf(0.0, 0.0), 4), (tf(8.0, 0.0), 1)], false).unwrap();
        assert_eq!(t.sample(0).x, 0.0);
        assert_eq!(t.sample(2).x, 4.0); // half of the 4-tick hold
        assert_eq!(t.duration(), 5);
    }

    #[test]
    fn one_shot_holds_last_key_no_wrap() {
        let t = GeometricTween::new(vec![(tf(0.0, 0.0), 2), (tf(10.0, 0.0), 2)], false).unwrap();
        // Inside the last key it does not tween back toward key0.
        assert_eq!(t.sample(3).x, 10.0);
        assert_eq!(t.sample(99).x, 10.0); // past the end clamps to last
    }

    #[test]
    fn looping_wraps_last_key_back_to_first() {
        // key0 x=0 (2t), key1 x=10 (2t); looping → key1 tweens back to key0.
        let t = GeometricTween::new(vec![(tf(0.0, 0.0), 2), (tf(10.0, 0.0), 2)], true).unwrap();
        assert_eq!(t.sample(2).x, 10.0); // start of key1
        assert_eq!(t.sample(3).x, 5.0); // halfway key1 → key0 (wrap)
        assert_eq!(t.sample(4).x, 0.0); // tick 4 % 4 = 0 → key0
    }

    #[test]
    fn from_clip_pairs_transforms_and_rejects_mismatch() {
        let s = seq(&[(0xA, 1), (0xB, 1), (0xA, 1), (0xB, 1)]);
        let clip = AnimationClip::from_sequence(1, &s).unwrap(); // [A,B], looping
        assert!(GeometricTween::from_clip(&clip, &[tf(0.0, 0.0)]).is_none()); // wrong len
        let t = GeometricTween::from_clip(&clip, &[tf(0.0, 0.0), tf(20.0, 0.0)]).unwrap();
        assert_eq!(t.key_count(), 2);
        assert!(t.sample(0).x == 0.0);
    }
}
