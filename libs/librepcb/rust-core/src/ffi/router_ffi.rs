//! FFI for the [`pnsrouter`](https://docs.rs/pnsrouter) interactive push
//! and shove router.
//!
//! Two owned Rust objects cross the boundary as opaque pointers, in the
//! style of [`super::ibom_ffi`]:
//!
//! - [`PnsSnapshot`] is built up call by call from C++, where
//!   `librepcb::BoardPnsSnapshot` walks the board, and holds a
//!   [`WorldSnapshot`] plus the rule table the resolver reads. It also
//!   answers the debug queries the unit tests use.
//! - [`PnsRouter`] is the routing session. It is created from a snapshot,
//!   which it consumes, so a snapshot handle is dead after
//!   [`ffi_pnsrouter_new`] and must not be deleted again.
//!
//! Coordinates cross as `i64` nanometres, which is what LibrePCB's
//! `Length` holds, and are narrowed to the engine's `i32` nanometres by
//! [`to_coord`]. A coordinate outside the safe range is reported as
//! [`PnsResult::CoordinateOutOfRange`] rather than wrapped silently; the
//! C++ builder turns that into a `RuntimeError` naming the board item.

use super::cpp_ffi::{qstring_set, QString};
use pnsrouter::geometry::line_chain::LineChain;
use pnsrouter::geometry::seg::Seg;
use pnsrouter::geometry::shape::Shape;
use pnsrouter::geometry::vec2::Vec2;
use pnsrouter::item::{HostId, LayerRange, NetId, ViaType};
use pnsrouter::node::World;
use pnsrouter::router::{
  CommitDiff, FixOutcome, NewGeometry, NewItem, PreviewFrame, PreviewStyle,
  PreviewVia, Router, RouterState, StartError,
};
use pnsrouter::rules::{
  Constraint, ConstraintType, ItemRef, Keepout, RuleResolver,
};
use pnsrouter::settings::{RouterMode, RoutingSettings, Sizes};
use pnsrouter::snapshot::{
  HostIndex, WorldGeometry, WorldItem, WorldItemFlags, WorldSnapshot,
};

/// The largest coordinate magnitude the engine accepts, in nanometres.
///
/// The engine works in `i32` nanometres, about plus or minus 2.147 m. The
/// limit is set a little inside that so that inflating a shape by the
/// broad phase clearance near the edge of the board cannot wrap. See the
/// integration design note, section 1.4.
const MAX_COORDINATE: i64 = 2_000_000_000;

/// What went wrong, if anything.
///
/// Every snapshot builder entry point answers with one of these instead of
/// panicking, so that the C++ side can raise a `RuntimeError` naming the
/// board item that could not be converted.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsResult {
  /// The call succeeded.
  Ok = 0,
  /// A coordinate was outside plus or minus 2 metres.
  CoordinateOutOfRange = 1,
  /// A polygon shape carried fewer than three vertices.
  DegeneratePolygon = 2,
  /// The layer range was empty or outside the board's copper stack.
  InvalidLayerRange = 3,
  /// A debug query named a host id the snapshot does not know.
  UnknownItem = 4,
  /// A clearance query answered "these two can never collide".
  NoClearance = 5,
}

/// Which shape of the small geometry vocabulary a [`PnsShape`] carries.
///
/// The four the engine has native support for. Keeping circles and
/// rectangles native rather than polygonising everything is what keeps the
/// collision inner loop cheap; see the integration design note, section
/// 1.2.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsShapeKind {
  /// A circle of [`PnsShape::center`] and [`PnsShape::radius`].
  Circle = 0,
  /// A rectangle centred on [`PnsShape::center`], of
  /// [`PnsShape::half_size`], with corner radius [`PnsShape::radius`].
  Rect = 1,
  /// A capsule from [`PnsShape::p1`] to [`PnsShape::p2`] of full width
  /// [`PnsShape::radius`].
  Segment = 2,
  /// A closed polygon of [`PnsShape::vertices`].
  Polygon = 3,
}

/// One point in host coordinates, nanometres.
///
/// Mirrors LibrePCB's `Point`, whose two `Length` members are `int64_t`
/// nanometres.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsPoint {
  /// The x coordinate in nanometres.
  pub x: i64,
  /// The y coordinate in nanometres.
  pub y: i64,
}

/// One obstacle shape.
///
/// A flat struct rather than a tagged union so that cbindgen can describe
/// it to C++ without a variant type. Only the fields
/// [`PnsShape::kind`] names are read; the rest may hold anything.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsShape {
  /// Which fields below are meaningful.
  pub kind: PnsShapeKind,
  /// The centre of a circle or of a rectangle.
  pub center: PnsPoint,
  /// A circle radius, a rectangle corner radius, or a capsule's full
  /// width.
  pub radius: i64,
  /// Half the width and half the height of a rectangle.
  pub half_size: PnsPoint,
  /// The first end of a capsule.
  pub p1: PnsPoint,
  /// The second end of a capsule.
  pub p2: PnsPoint,
  /// The vertices of a polygon, never null even when the count is zero.
  pub vertices: *const PnsPoint,
  /// How many vertices [`PnsShape::vertices`] points at.
  pub vertex_count: usize,
}

/// The part of a snapshot item that does not depend on its geometry.
///
/// Port of the common fields of `pnsrouter::snapshot::WorldItem`.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsItemHeader {
  /// The host's own handle for the board object this came from, counted
  /// from one so that zero can serve as a null.
  pub host_id: u64,
  /// The net, counted from one, or zero for an object with no net at all.
  pub net: u32,
  /// The first dense copper layer index the item occupies.
  pub layer_start: i32,
  /// The last dense copper layer index the item occupies, inclusive.
  pub layer_end: i32,
  /// Whether the user pinned the object in place.
  pub locked: bool,
  /// Whether a trace may start or end on the object.
  pub routable: bool,
  /// Whether the object is a pad on a pin with no internal connection.
  pub free_pad: bool,
  /// Whether the object became several engine items.
  pub compound_primitive: bool,
  /// Whether the object is a board edge, which picks up the copper to
  /// board clearance rule.
  pub board_edge: bool,
  /// The pad's own copper clearance override in nanometres, or a negative
  /// value when the object has none.
  pub copper_clearance: i64,
}

/// A straight track.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsSegmentGeometry {
  /// One end of the centre line.
  pub p1: PnsPoint,
  /// The other end of the centre line.
  pub p2: PnsPoint,
  /// The full track width in nanometres.
  pub width: i64,
}

/// How far through the copper stack a via reaches.
///
/// Wrapper for the three of `pnsrouter::item::ViaType` LibrePCB can tell
/// apart through `Via::isBlind` and `Via::isBuried`.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsViaType {
  /// All the way through the board.
  Through = 0,
  /// From an outer layer to an inner one.
  Blind = 1,
  /// Between two inner layers.
  Buried = 2,
}

/// A plated through, blind or buried via.
///
/// The engine drills the hole itself from the drill diameter, so the host
/// never fills a hole for a via.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsViaGeometry {
  /// The centre.
  pub pos: PnsPoint,
  /// The copper diameter in nanometres.
  pub diameter: i64,
  /// The drill diameter in nanometres.
  pub drill: i64,
  /// How far through the copper stack the via reaches.
  pub via_type: PnsViaType,
  /// Whether the via has no net yet.
  pub is_free: bool,
}

/// A pad, a board outline, or a copper graphic.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsSolidGeometry {
  /// The copper, in board coordinates.
  pub shape: PnsShape,
  /// The point a trace snaps to.
  pub pos: PnsPoint,
  /// Whether [`PnsSolidGeometry::hole`] is meaningful.
  pub has_hole: bool,
  /// The shape drilled through the copper.
  pub hole: PnsShape,
}

/// A hole with no copper of its own, such as a board mounting hole.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsHoleGeometry {
  /// The drilled shape.
  pub shape: PnsShape,
}

/// The board wide design rule values the resolver reads.
///
/// Wrapper for `BoardDesignRuleCheckSettings` and `BoardDesignRules`; see
/// the integration design note, section 2.1. Every value is in
/// nanometres.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsBoardRules {
  /// `BoardDesignRuleCheckSettings::getMinCopperCopperClearance`.
  pub min_copper_copper_clearance: i64,
  /// `BoardDesignRuleCheckSettings::getMinCopperBoardClearance`.
  pub min_copper_board_clearance: i64,
  /// `BoardDesignRuleCheckSettings::getMinCopperNpthClearance`.
  pub min_copper_npth_clearance: i64,
  /// `BoardDesignRuleCheckSettings::getMinDrillDrillClearance`.
  pub min_drill_drill_clearance: i64,
  /// `BoardDesignRuleCheckSettings::getMinDrillBoardClearance`.
  pub min_drill_board_clearance: i64,
  /// `BoardDesignRuleCheckSettings::getMinCopperWidth`.
  pub min_copper_width: i64,
  /// `BoardDesignRuleCheckSettings::getMinPthDrillDiameter`.
  pub min_pth_drill_diameter: i64,
  /// `BoardDesignRules::getDefaultTraceWidth`.
  pub default_trace_width: i64,
  /// `BoardDesignRules::getDefaultViaDrillDiameter`.
  pub default_via_drill_diameter: i64,
}

/// The per net class design rule values the resolver reads.
///
/// Wrapper for `NetClass`; see the integration design note, section 2.1.
/// The two defaults are zero when the net class does not set them, which
/// is how `std::optional` crosses here.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsNetClassRules {
  /// `NetClass::getMinCopperCopperClearance`.
  pub min_copper_copper_clearance: i64,
  /// `NetClass::getMinCopperWidth`.
  pub min_copper_width: i64,
  /// `NetClass::getMinViaDrillDiameter`.
  pub min_via_drill_diameter: i64,
  /// `NetClass::getDefaultTraceWidth`, or zero when it is not set.
  pub default_trace_width: i64,
  /// `NetClass::getDefaultViaDrill`, or zero when it is not set.
  pub default_via_drill: i64,
}

/// How many items of each kind a snapshot holds.
///
/// A debug accessor for the unit tests, which have no other way to see
/// what the builder produced.
#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct PnsSnapshotStats {
  /// How many copper layers the board has.
  pub copper_layer_count: u8,
  /// The broad phase inflation radius in nanometres.
  pub max_clearance: i32,
  /// How many items the snapshot holds in total.
  pub item_count: usize,
  /// How many of them are tracks.
  pub segment_count: usize,
  /// How many of them are vias.
  pub via_count: usize,
  /// How many of them are solids.
  pub solid_count: usize,
  /// How many of them are bare holes.
  pub hole_count: usize,
  /// How many solids carry a drilled hole.
  pub drilled_solid_count: usize,
  /// How many nets the snapshot knows.
  pub net_count: usize,
  /// How many net classes the snapshot knows.
  pub net_class_count: usize,
}

/// Which of a host object's two engine items a debug query means.
///
/// A drilled pad becomes a solid plus a hole that the engine creates
/// itself, and the hole is the interesting side of a hole clearance
/// query.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsItemRole {
  /// The copper item the host object became.
  Copper = 0,
  /// The hole the engine drilled through it.
  Hole = 1,
}

/// Which design rule a debug constraint query means.
///
/// A subset of `pnsrouter::rules::ConstraintType`, holding the ones
/// LibrePCB can answer; see the integration design note, section 2.2.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsConstraintKind {
  /// Copper to copper clearance.
  Clearance = 0,
  /// Track width.
  Width = 1,
  /// Via drill diameter.
  ViaHole = 2,
  /// Copper to board edge clearance.
  EdgeClearance = 3,
  /// Hole to copper clearance.
  HoleClearance = 4,
  /// Hole to hole clearance.
  HoleToHole = 5,
}

/// The settings a routing session starts with.
///
/// Only the values the host has a control for. Everything else stays at
/// `RoutingSettings::default`, which reproduces KiCad's own constructor.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsRouterSettings {
  /// Zero for mark obstacles, one for shove, two for walkaround, which is
  /// `pnsrouter::settings::RouterMode`'s own numbering.
  pub mode: u8,
  /// The track width to place, in nanometres.
  pub track_width: i64,
  /// The via copper diameter to place, in nanometres.
  pub via_diameter: i64,
  /// The via drill diameter to place, in nanometres.
  pub via_drill: i64,
  /// How many times the shove may push before it gives up and the router
  /// falls back to walking around.
  ///
  /// `RoutingSettings::shove_iteration_limit`, whose default is KiCad's
  /// 250. The host is expected to keep it in a sane range. Zero would make
  /// every shove fail immediately.
  pub shove_iteration_limit: u32,
  /// Whether the session records everything it is driven with.
  ///
  /// Read by [`ffi_pnsrouter_new`] only, because a recording has to start
  /// from the snapshot the session was built on and that snapshot is gone
  /// by the time [`ffi_pnsrouter_set_settings`] runs. It is ignored there
  /// rather than refused, so that a host can hand the same struct to both
  /// entry points.
  pub record_session: bool,
}

// ---------------------------------------------------------------------
// Rule resolver
// ---------------------------------------------------------------------

/// What the resolver knows about one host object.
#[derive(Copy, Clone, Debug, Default)]
struct HostRules {
  /// The pad's own copper clearance override, or `None`.
  copper_clearance: Option<i32>,
  /// Whether the object is a board edge.
  board_edge: bool,
}

/// LibrePCB's design rules as a table the resolver reads without ever
/// calling back into C++.
///
/// Reproduces `BoardDesignRuleCheckData`'s `std::max` combination of a net
/// class value with the board setting
/// (`libs/librepcb/core/project/board/drc/boarddesignrulecheckdata.h:212`
/// onwards), so that the router cannot produce a board that fails
/// LibrePCB's own design rule check.
#[derive(Clone, Debug)]
pub struct LibrePcbRules {
  /// The board wide values, narrowed to nanometre `i32`.
  board: BoardRuleValues,
  /// The net class values, in the order the host added them.
  net_classes: Vec<NetClassRuleValues>,
  /// The net class index of each net, in `NetId` order.
  nets: Vec<usize>,
  /// What each host object contributes, indexed by host id. Entry zero is
  /// the unused null id.
  hosts: Vec<HostRules>,
  /// The largest clearance any query below can answer.
  max_clearance: i32,
}

/// [`PnsBoardRules`] after the range check.
#[derive(Copy, Clone, Debug, Default)]
struct BoardRuleValues {
  /// Copper to copper.
  min_copper_copper_clearance: i32,
  /// Copper to board edge.
  min_copper_board_clearance: i32,
  /// Copper to non plated hole.
  min_copper_npth_clearance: i32,
  /// Hole to hole.
  min_drill_drill_clearance: i32,
  /// Hole to board edge.
  min_drill_board_clearance: i32,
  /// The smallest permitted copper width.
  min_copper_width: i32,
  /// The smallest permitted plated hole diameter.
  min_pth_drill_diameter: i32,
  /// The width a new trace starts at.
  default_trace_width: i32,
  /// The drill a new via starts at.
  default_via_drill_diameter: i32,
}

/// [`PnsNetClassRules`] after the range check.
#[derive(Copy, Clone, Debug, Default)]
struct NetClassRuleValues {
  /// Copper to copper.
  min_copper_copper_clearance: i32,
  /// The smallest permitted copper width.
  min_copper_width: i32,
  /// The smallest permitted via drill diameter.
  min_via_drill_diameter: i32,
  /// The width a new trace on this net class starts at, or zero.
  default_trace_width: i32,
  /// The drill a new via on this net class starts at, or zero.
  default_via_drill: i32,
}

impl LibrePcbRules {
  /// An empty table with every rule at zero.
  fn new() -> Self {
    Self {
      board: BoardRuleValues::default(),
      net_classes: Vec::new(),
      nets: Vec::new(),
      hosts: vec![HostRules::default()],
      max_clearance: 0,
    }
  }

  /// The net class values of a net, or the all zero defaults.
  fn net_class_of(&self, net: Option<NetId>) -> NetClassRuleValues {
    let Some(net) = net else {
      return NetClassRuleValues::default();
    };

    self
      .nets
      .get(net.0 as usize)
      .and_then(|index| self.net_classes.get(*index))
      .copied()
      .unwrap_or_default()
  }

  /// What the resolver knows about the host object an item came from.
  fn host_rules_of(&self, item: ItemRef<'_>) -> HostRules {
    item
      .item()
      .host_id()
      .and_then(|host| self.hosts.get(host.0 as usize))
      .copied()
      .unwrap_or_default()
  }

  /// The copper to copper clearance one side of a pair asks for.
  ///
  /// The `std::max` of the board setting, the net class setting and the
  /// pad's own override, which is
  /// `BoardDesignRuleCheckData::getMinCopperCopperClearance` plus the pad
  /// override the design rule check applies separately.
  fn copper_clearance_of(&self, item: ItemRef<'_>) -> i32 {
    let net_class = self.net_class_of(item.item().net());
    let host = self.host_rules_of(item);

    self
      .board
      .min_copper_copper_clearance
      .max(net_class.min_copper_copper_clearance)
      .max(host.copper_clearance.unwrap_or(0))
  }

  /// The largest clearance [`LibrePcbRules::clearance`] can answer.
  ///
  /// Exact by construction: every branch of the ladder takes a maximum
  /// over the same inputs this maximum runs over. See the integration
  /// design note, section 2.3.
  fn recompute_max_clearance(&mut self) {
    let mut max = self
      .board
      .min_copper_copper_clearance
      .max(self.board.min_copper_board_clearance)
      .max(self.board.min_copper_npth_clearance)
      .max(self.board.min_drill_drill_clearance)
      .max(self.board.min_drill_board_clearance);

    for net_class in &self.net_classes {
      max = max.max(net_class.min_copper_copper_clearance);
    }

    for host in &self.hosts {
      max = max.max(host.copper_clearance.unwrap_or(0));
    }

    self.max_clearance = max;
  }

  /// The minimum copper width of a net, the `std::max` of the board
  /// setting and the net class setting.
  ///
  /// Port of `BoardDesignRuleCheckData::getMinCopperWidth`.
  fn min_copper_width(&self, net: Option<NetId>) -> i32 {
    self
      .board
      .min_copper_width
      .max(self.net_class_of(net).min_copper_width)
  }

  /// The minimum via drill diameter of a net, the `std::max` of the board
  /// setting and the net class setting.
  ///
  /// Port of `BoardDesignRuleCheckData::getMinViaDrillDiameter`.
  fn min_via_drill_diameter(&self, net: Option<NetId>) -> i32 {
    self
      .board
      .min_pth_drill_diameter
      .max(self.net_class_of(net).min_via_drill_diameter)
  }
}

impl RuleResolver for LibrePcbRules {
  /// The clearance ladder of the integration design note, section 2.2.
  ///
  /// The same shape as `pnsrouter::rules::FixedClearance`, with the board
  /// edge rung added and with the copper rung reading the per net class
  /// and per pad values instead of one constant. There is no `else`
  /// between the hole rungs and the copper rung, deliberately: KiCad's
  /// own resolver lets a plated hole pick up both.
  fn clearance(
    &self,
    a: ItemRef<'_>,
    b: Option<ItemRef<'_>>,
    use_epsilon: bool,
  ) -> Option<i32> {
    let a_is_hole = self.is_drilled_hole(a);
    let b_is_hole = b.is_some_and(|b| self.is_drilled_hole(b));

    // A null net is never the same as anything, not even as another null
    // net.
    let same_net = b.is_some_and(|b| {
      a.item().net().is_some() && a.item().net() == b.item().net()
    });
    let free_pad =
      b.is_some_and(|b| a.item().is_free_pad() || b.item().is_free_pad());
    let board_edge = self.host_rules_of(a).board_edge
      || b.is_some_and(|b| self.host_rules_of(b).board_edge);

    let mut result = 0;

    if a_is_hole && b_is_hole {
      result = result.max(self.board.min_drill_drill_clearance);
    } else if (a_is_hole || b_is_hole) && !same_net {
      result = result.max(self.board.min_copper_npth_clearance);
    }

    if !a_is_hole && !b_is_hole && !same_net && !free_pad {
      let mut copper = self.copper_clearance_of(a);

      if let Some(b) = b {
        copper = copper.max(self.copper_clearance_of(b));
      }

      result = result.max(copper);
    }

    if board_edge {
      result = result.max(self.board.min_copper_board_clearance);

      if a_is_hole || b_is_hole {
        result = result.max(self.board.min_drill_board_clearance);
      }
    }

    debug_assert!(
      result <= self.max_clearance,
      "a clearance of {result} exceeds max_clearance {}",
      self.max_clearance
    );

    if (same_net || free_pad) && result == 0 {
      return None;
    }

    if use_epsilon && result > 0 {
      result = (result - self.clearance_epsilon()).max(0);
    }

    Some(result)
  }

  /// Zero. LibrePCB has no design rule check epsilon, which makes the
  /// router marginally stricter than KiCad, the safe direction.
  fn clearance_epsilon(&self) -> i32 {
    0
  }

  /// The four rules of the integration design note, section 2.2, that
  /// LibrePCB can answer as a plain number.
  ///
  /// `ViaDiameter` is not among them: LibrePCB expresses it as an annular
  /// ring ratio of the drill rather than as an absolute length, so it
  /// cannot be answered without the drill diameter the caller does not
  /// pass.
  fn constraint(
    &self,
    constraint_type: ConstraintType,
    a: ItemRef<'_>,
    b: Option<ItemRef<'_>>,
    _layer: i32,
  ) -> Option<Constraint> {
    let net = a.item().net();
    let net_class = self.net_class_of(net);

    let (min, opt) = match constraint_type {
      ConstraintType::Clearance => (self.clearance(a, b, false)?, 0),
      ConstraintType::Width => (
        self.min_copper_width(net),
        if net_class.default_trace_width > 0 {
          net_class.default_trace_width
        } else {
          self.board.default_trace_width
        },
      ),
      ConstraintType::ViaHole => (
        self.min_via_drill_diameter(net),
        if net_class.default_via_drill > 0 {
          net_class.default_via_drill
        } else {
          self.board.default_via_drill_diameter
        },
      ),
      ConstraintType::EdgeClearance => {
        (self.board.min_copper_board_clearance, 0)
      }
      ConstraintType::HoleClearance => {
        (self.board.min_copper_npth_clearance, 0)
      }
      ConstraintType::HoleToHole => (self.board.min_drill_drill_clearance, 0),
      _ => return None,
    };

    Some(Constraint {
      constraint_type,
      min: Some(min),
      opt: if opt > 0 { Some(opt) } else { None },
      max: None,
      allowed: true,
    })
  }

  /// Never, until zones are synced. See the integration design note,
  /// section 1.10.
  fn is_keepout(&self, _obstacle: ItemRef<'_>, _item: ItemRef<'_>) -> Keepout {
    Keepout::None
  }

  /// Every hole, because LibrePCB has no plating attribute the snapshot
  /// could carry yet; see the integration design note, question Q4.
  fn is_drilled_hole(&self, item: ItemRef<'_>) -> bool {
    item.item().kind() == pnsrouter::item::Kind::HOLE
  }

  /// Never, which keeps the collision ladder off its castellation path.
  fn is_non_plated_slot(&self, _item: ItemRef<'_>) -> bool {
    false
  }

  /// The net's own number offset by one, because the engine's topology
  /// code reads zero and below as "no net". The orphan below is the one
  /// net that is not a host net and answers `-1`.
  fn net_code(&self, net: NetId) -> i32 {
    if net == self.orphaned_net() {
      return -1;
    }

    i32::try_from(net.0).map_or(i32::MAX, |code| code.saturating_add(1))
  }

  /// A net number the host cannot send, for a route started in free
  /// space.
  ///
  /// The engine places such a route on this net rather than on no net at
  /// all, so that its head and the tail it has already fixed count as the
  /// same net and a shove can push an obstacle instead of the placer
  /// walking around it. The contract on the trait method asks for three
  /// things: a stable value, a net code of zero or below, and a net no
  /// snapshot item carries.
  ///
  /// `u32::MAX` gives the third one for free. [`PnsItemHeader::net`]
  /// counts from one and `to_item` subtracts that one again, so a host
  /// net would have to arrive as `u32::MAX + 1` to land here. The net
  /// code needs its own case: the `try_from` above fails on `u32::MAX`
  /// and its fallback would report the orphan as `i32::MAX`, a real net.
  ///
  /// Nothing else needs a case. [`LibrePcbRules::net_class_of`] finds no
  /// entry for this net and falls back to the all zero defaults, so a
  /// route in free space is sized by the board settings alone, which is
  /// what a net with no net class should get.
  fn orphaned_net(&self) -> NetId {
    NetId(u32::MAX)
  }
}

// ---------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------

/// Narrow one host coordinate to the engine's `i32` nanometres.
fn to_coord(value: i64) -> Result<i32, PnsResult> {
  if value.abs() > MAX_COORDINATE {
    return Err(PnsResult::CoordinateOutOfRange);
  }

  i32::try_from(value).map_err(|_| PnsResult::CoordinateOutOfRange)
}

/// Narrow one host point to the engine's `i32` nanometres.
fn to_vec2(point: PnsPoint) -> Result<Vec2, PnsResult> {
  Ok(Vec2::new(to_coord(point.x)?, to_coord(point.y)?))
}

/// Narrow one host length, which is never negative, to the engine's `i32`
/// nanometres.
fn to_length(value: i64) -> Result<i32, PnsResult> {
  to_coord(value)
}

/// Convert one host shape into an engine shape.
///
/// # Safety
///
/// [`PnsShape::vertices`] must point at [`PnsShape::vertex_count`]
/// readable points when the kind is [`PnsShapeKind::Polygon`].
unsafe fn to_shape(shape: &PnsShape) -> Result<Shape, PnsResult> {
  match shape.kind {
    PnsShapeKind::Circle => Ok(Shape::circle(
      to_vec2(shape.center)?,
      to_length(shape.radius)?,
    )),
    PnsShapeKind::Rect => {
      let center = to_vec2(shape.center)?;
      let half = to_vec2(shape.half_size)?;
      let origin = Vec2::new(center.x - half.x, center.y - half.y);
      let size = Vec2::new(half.x.saturating_mul(2), half.y.saturating_mul(2));

      Ok(Shape::rounded_rect(origin, size, to_length(shape.radius)?))
    }
    PnsShapeKind::Segment => Ok(Shape::segment(
      Seg::new(to_vec2(shape.p1)?, to_vec2(shape.p2)?),
      to_length(shape.radius)?,
    )),
    PnsShapeKind::Polygon => {
      if shape.vertex_count < 3 {
        return Err(PnsResult::DegeneratePolygon);
      }

      let mut points = Vec::with_capacity(shape.vertex_count);

      // SAFETY: the caller promises the pointer and the count agree.
      let raw = unsafe {
        std::slice::from_raw_parts(shape.vertices, shape.vertex_count)
      };

      for point in raw {
        points.push(to_vec2(*point)?);
      }

      Ok(Shape::simple(LineChain::from_points(points, true)))
    }
  }
}

/// Convert one item header into the common fields of a snapshot item.
fn to_item(
  header: &PnsItemHeader,
  copper_layer_count: u8,
  geometry: WorldGeometry,
) -> Result<WorldItem, PnsResult> {
  let last = i32::from(copper_layer_count) - 1;

  if header.layer_start < 0
    || header.layer_end < header.layer_start
    || header.layer_end > last
  {
    return Err(PnsResult::InvalidLayerRange);
  }

  let mut item = WorldItem::new(
    HostId(header.host_id),
    if header.net > 0 {
      Some(NetId(header.net - 1))
    } else {
      None
    },
    LayerRange::new(header.layer_start, header.layer_end),
    geometry,
  );

  item.flags = WorldItemFlags {
    locked: header.locked,
    routable: header.routable,
    free_pad: header.free_pad,
    compound_primitive: header.compound_primitive,
  };

  Ok(item)
}

// ---------------------------------------------------------------------
// The snapshot handle
// ---------------------------------------------------------------------

/// A board snapshot under construction, plus the rules the resolver reads.
///
/// Owned by C++ through a `RustHandle`. Deleted either by
/// [`ffi_pnsrouter_snapshot_delete`] or by [`ffi_pnsrouter_new`], which
/// consumes it.
pub struct PnsSnapshot {
  /// The snapshot itself.
  snapshot: WorldSnapshot,
  /// The rule table that becomes the session's resolver.
  rules: LibrePcbRules,
  /// A world built from the snapshot on demand, for the debug queries.
  world: Option<(World, HostIndex)>,
}

impl PnsSnapshot {
  /// Record what one item contributes to the rule table.
  fn note_host(&mut self, header: &PnsItemHeader) {
    let index = header.host_id as usize;

    if self.rules.hosts.len() <= index {
      self.rules.hosts.resize(index + 1, HostRules::default());
    }

    let entry = &mut self.rules.hosts[index];

    entry.board_edge |= header.board_edge;

    if header.copper_clearance >= 0 {
      let clearance = to_coord(header.copper_clearance).unwrap_or(i32::MAX);

      entry.copper_clearance =
        Some(entry.copper_clearance.unwrap_or(0).max(clearance));
    }
  }

  /// Store one finished item and forget any cached world.
  fn push(&mut self, item: WorldItem) -> PnsResult {
    self.snapshot.items.push(item);
    self.world = None;

    PnsResult::Ok
  }

  /// The world the debug queries run against, built on first use.
  fn ensure_world(&mut self) -> &(World, HostIndex) {
    if self.world.is_none() {
      self.rules.recompute_max_clearance();
      self.snapshot.max_clearance = self.rules.max_clearance;
      self.world = Some(World::from_snapshot(&self.snapshot));
    }

    self.world.as_ref().expect("the world was just built")
  }
}

/// Create an empty snapshot of a board with `copper_layer_count` copper
/// layers.
///
/// The layer indices every later call takes are dense and zero based,
/// `0 ..= copper_layer_count - 1`; see the integration design note,
/// section 1.3.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_new(
  copper_layer_count: u8,
) -> *mut PnsSnapshot {
  Box::into_raw(Box::new(PnsSnapshot {
    snapshot: WorldSnapshot::new(copper_layer_count, 0),
    rules: LibrePcbRules::new(),
    world: None,
  }))
}

/// Delete a [`PnsSnapshot`] that was never handed to
/// [`ffi_pnsrouter_new`].
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_delete(obj: *mut PnsSnapshot) {
  assert!(!obj.is_null());
  unsafe { drop(Box::from_raw(obj)) };
}

/// Set the board wide design rule values.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_set_board_rules(
  obj: &mut PnsSnapshot,
  rules: &PnsBoardRules,
) -> PnsResult {
  let values = BoardRuleValues {
    min_copper_copper_clearance: match to_length(
      rules.min_copper_copper_clearance,
    ) {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_copper_board_clearance: match to_length(
      rules.min_copper_board_clearance,
    ) {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_copper_npth_clearance: match to_length(rules.min_copper_npth_clearance)
    {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_drill_drill_clearance: match to_length(rules.min_drill_drill_clearance)
    {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_drill_board_clearance: match to_length(rules.min_drill_board_clearance)
    {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_copper_width: match to_length(rules.min_copper_width) {
      Ok(value) => value,
      Err(error) => return error,
    },
    min_pth_drill_diameter: match to_length(rules.min_pth_drill_diameter) {
      Ok(value) => value,
      Err(error) => return error,
    },
    default_trace_width: match to_length(rules.default_trace_width) {
      Ok(value) => value,
      Err(error) => return error,
    },
    default_via_drill_diameter: match to_length(
      rules.default_via_drill_diameter,
    ) {
      Ok(value) => value,
      Err(error) => return error,
    },
  };

  obj.rules.board = values;
  obj.world = None;

  PnsResult::Ok
}

/// Add one net class and return its index.
///
/// The index is what [`ffi_pnsrouter_snapshot_add_net`] takes.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_add_net_class(
  obj: &mut PnsSnapshot,
  rules: &PnsNetClassRules,
) -> usize {
  obj.rules.net_classes.push(NetClassRuleValues {
    min_copper_copper_clearance: to_length(rules.min_copper_copper_clearance)
      .unwrap_or(i32::MAX),
    min_copper_width: to_length(rules.min_copper_width).unwrap_or(i32::MAX),
    min_via_drill_diameter: to_length(rules.min_via_drill_diameter)
      .unwrap_or(i32::MAX),
    default_trace_width: to_length(rules.default_trace_width).unwrap_or(0),
    default_via_drill: to_length(rules.default_via_drill).unwrap_or(0),
  });
  obj.world = None;

  obj.rules.net_classes.len() - 1
}

/// Add one net belonging to a net class and return the net number the item
/// headers take, which is the dense net index plus one.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_add_net(
  obj: &mut PnsSnapshot,
  net_class_index: usize,
) -> u32 {
  obj.rules.nets.push(net_class_index);
  obj.world = None;

  obj.rules.nets.len() as u32
}

/// Add one track.
///
/// Wraps `pnsrouter::snapshot::WorldGeometry::Segment`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_add_segment(
  obj: &mut PnsSnapshot,
  header: &PnsItemHeader,
  geometry: &PnsSegmentGeometry,
) -> PnsResult {
  let body = match (|| -> Result<WorldGeometry, PnsResult> {
    Ok(WorldGeometry::Segment {
      seg: Seg::new(to_vec2(geometry.p1)?, to_vec2(geometry.p2)?),
      width: to_length(geometry.width)?,
    })
  })() {
    Ok(body) => body,
    Err(error) => return error,
  };

  match to_item(header, obj.snapshot.copper_layer_count, body) {
    Ok(item) => {
      obj.note_host(header);
      obj.push(item)
    }
    Err(error) => error,
  }
}

/// Add one via.
///
/// Wraps `pnsrouter::snapshot::WorldGeometry::Via`. The engine drills the
/// hole itself, so no hole crosses here.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_add_via(
  obj: &mut PnsSnapshot,
  header: &PnsItemHeader,
  geometry: &PnsViaGeometry,
) -> PnsResult {
  let body = match (|| -> Result<WorldGeometry, PnsResult> {
    Ok(WorldGeometry::Via {
      pos: to_vec2(geometry.pos)?,
      diameter: to_length(geometry.diameter)?,
      drill: to_length(geometry.drill)?,
      via_type: match geometry.via_type {
        PnsViaType::Through => ViaType::Through,
        PnsViaType::Blind => ViaType::Blind,
        PnsViaType::Buried => ViaType::Buried,
      },
      is_free: geometry.is_free,
    })
  })() {
    Ok(body) => body,
    Err(error) => return error,
  };

  match to_item(header, obj.snapshot.copper_layer_count, body) {
    Ok(item) => {
      obj.note_host(header);
      obj.push(item)
    }
    Err(error) => error,
  }
}

/// Add one solid: a pad, a copper polygon or a board outline.
///
/// Wraps `pnsrouter::snapshot::WorldGeometry::Solid`. A pad becomes one
/// solid per copper layer and the hole rides on exactly one of them; see
/// the integration design note, section 1.7.
///
/// # Safety
///
/// The shapes' vertex pointers must stay valid for the duration of the
/// call.
#[no_mangle]
unsafe extern "C" fn ffi_pnsrouter_snapshot_add_solid(
  obj: &mut PnsSnapshot,
  header: &PnsItemHeader,
  geometry: &PnsSolidGeometry,
) -> PnsResult {
  let body = match (|| -> Result<WorldGeometry, PnsResult> {
    Ok(WorldGeometry::Solid {
      // SAFETY: forwarded from this function's own contract.
      shape: unsafe { to_shape(&geometry.shape)? },
      pos: to_vec2(geometry.pos)?,
      offset: Vec2::new(0, 0),
      orientation_degrees: 0.0,
      anchors: Vec::new(),
    })
  })() {
    Ok(body) => body,
    Err(error) => return error,
  };

  let hole = if geometry.has_hole {
    // SAFETY: forwarded from this function's own contract.
    match unsafe { to_shape(&geometry.hole) } {
      Ok(shape) => Some(shape),
      Err(error) => return error,
    }
  } else {
    None
  };

  match to_item(header, obj.snapshot.copper_layer_count, body) {
    Ok(mut item) => {
      item.hole = hole;
      obj.note_host(header);
      obj.push(item)
    }
    Err(error) => error,
  }
}

/// Add one hole with no copper of its own, such as a board mounting hole.
///
/// Wraps `pnsrouter::snapshot::WorldGeometry::Hole`.
///
/// # Safety
///
/// The shape's vertex pointer must stay valid for the duration of the
/// call.
#[no_mangle]
unsafe extern "C" fn ffi_pnsrouter_snapshot_add_hole(
  obj: &mut PnsSnapshot,
  header: &PnsItemHeader,
  geometry: &PnsHoleGeometry,
) -> PnsResult {
  // SAFETY: forwarded from this function's own contract.
  let shape = match unsafe { to_shape(&geometry.shape) } {
    Ok(shape) => shape,
    Err(error) => return error,
  };

  match to_item(
    header,
    obj.snapshot.copper_layer_count,
    WorldGeometry::Hole { shape },
  ) {
    Ok(item) => {
      obj.note_host(header);
      obj.push(item)
    }
    Err(error) => error,
  }
}

/// Read back what the snapshot holds, for the unit tests.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_stats(
  obj: &mut PnsSnapshot,
  out: &mut PnsSnapshotStats,
) {
  obj.rules.recompute_max_clearance();
  obj.snapshot.max_clearance = obj.rules.max_clearance;

  let mut stats = PnsSnapshotStats {
    copper_layer_count: obj.snapshot.copper_layer_count,
    max_clearance: obj.rules.max_clearance,
    item_count: obj.snapshot.items.len(),
    net_count: obj.rules.nets.len(),
    net_class_count: obj.rules.net_classes.len(),
    ..PnsSnapshotStats::default()
  };

  for item in &obj.snapshot.items {
    match item.geometry {
      WorldGeometry::Segment { .. } => stats.segment_count += 1,
      WorldGeometry::Via { .. } => stats.via_count += 1,
      WorldGeometry::Solid { .. } => stats.solid_count += 1,
      WorldGeometry::Hole { .. } => stats.hole_count += 1,
    }

    if item.hole.is_some() {
      stats.drilled_solid_count += 1;
    }
  }

  *out = stats;
}

/// The clearance the resolver requires between two host objects.
///
/// A debug entry point for the unit tests, which have no other way to
/// reach `pnsrouter::rules::RuleResolver`. Answers
/// [`PnsResult::NoClearance`] where the resolver says the two can never
/// collide, and [`PnsResult::UnknownItem`] where a host id or a role is
/// not in the snapshot.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_clearance(
  obj: &mut PnsSnapshot,
  a_host: u64,
  a_role: PnsItemRole,
  b_host: u64,
  b_role: PnsItemRole,
  out: &mut i32,
) -> PnsResult {
  // The debug assertion inside the ladder compares against
  // max_clearance, so the table has to be up to date before it is cloned.
  obj.rules.recompute_max_clearance();

  let rules = obj.rules.clone();
  let (world, index) = obj.ensure_world();

  let Some(a) = resolve_item(world, index, a_host, a_role) else {
    return PnsResult::UnknownItem;
  };
  let Some(b) = resolve_item(world, index, b_host, b_role) else {
    return PnsResult::UnknownItem;
  };

  match rules.clearance(a, Some(b), false) {
    Some(clearance) => {
      *out = clearance;
      PnsResult::Ok
    }
    None => PnsResult::NoClearance,
  }
}

/// One design rule value the resolver answers for a host object.
///
/// A debug entry point for the unit tests, mirroring
/// `BoardDesignRuleCheckData`'s helper methods.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_constraint(
  obj: &mut PnsSnapshot,
  kind: PnsConstraintKind,
  host: u64,
  out_min: &mut i32,
  out_opt: &mut i32,
) -> PnsResult {
  obj.rules.recompute_max_clearance();

  let rules = obj.rules.clone();
  let (world, index) = obj.ensure_world();

  let Some(item) = resolve_item(world, index, host, PnsItemRole::Copper) else {
    return PnsResult::UnknownItem;
  };

  let constraint_type = match kind {
    PnsConstraintKind::Clearance => ConstraintType::Clearance,
    PnsConstraintKind::Width => ConstraintType::Width,
    PnsConstraintKind::ViaHole => ConstraintType::ViaHole,
    PnsConstraintKind::EdgeClearance => ConstraintType::EdgeClearance,
    PnsConstraintKind::HoleClearance => ConstraintType::HoleClearance,
    PnsConstraintKind::HoleToHole => ConstraintType::HoleToHole,
  };

  match rules.constraint(constraint_type, item, None, item.item().layer()) {
    Some(constraint) => {
      *out_min = constraint.min.unwrap_or(0);
      *out_opt = constraint.opt.unwrap_or(0);
      PnsResult::Ok
    }
    None => PnsResult::NoClearance,
  }
}

/// The broad phase inflation radius the snapshot will carry.
///
/// Every answer of [`ffi_pnsrouter_snapshot_clearance`] is bounded by it,
/// which the unit tests assert.
#[no_mangle]
extern "C" fn ffi_pnsrouter_snapshot_max_clearance(
  obj: &mut PnsSnapshot,
) -> i32 {
  obj.rules.recompute_max_clearance();
  obj.snapshot.max_clearance = obj.rules.max_clearance;

  obj.rules.max_clearance
}

/// The engine item one host object and role name, if the world has it.
fn resolve_item<'a>(
  world: &'a World,
  index: &HostIndex,
  host: u64,
  role: PnsItemRole,
) -> Option<ItemRef<'a>> {
  let id = *index.items_of(HostId(host)).first()?;
  let id = match role {
    PnsItemRole::Copper => id,
    PnsItemRole::Hole => world.item(id)?.hole()?,
  };

  Some(ItemRef::stored(id, world.item(id)?))
}

// ---------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------

/// How a host should draw one element of a preview frame.
///
/// Mirrors `pnsrouter::router::PreviewStyle` value for value.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsPreviewStyle {
  /// `PreviewStyle::Head`, the track being placed right now.
  Head = 0,
  /// `PreviewStyle::Tail`, geometry this session has already fixed.
  Tail = 1,
  /// `PreviewStyle::Hover`, the item under the cursor. Set by a host and
  /// never by the engine.
  Hover = 2,
  /// `PreviewStyle::SemiSolid`, one primitive of a rule area. Nothing
  /// emits it yet, because zones are not synced.
  SemiSolid = 3,
  /// `PreviewStyle::Collision`, something a violation was found on.
  Collision = 4,
}

/// Which fields of a [`PnsNewItem`] are meaningful.
///
/// Mirrors the two variants of `pnsrouter::router::NewGeometry`, which is
/// all a single track placer emits.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsNewGeometryKind {
  /// `NewGeometry::Segment`: [`PnsNewItem::p1`], [`PnsNewItem::p2`] and
  /// [`PnsNewItem::width`].
  Segment = 0,
  /// `NewGeometry::Via`: [`PnsNewItem::pos`], [`PnsNewItem::diameter`],
  /// [`PnsNewItem::drill`] and [`PnsNewItem::via_type`].
  Via = 1,
}

/// Why a routing session refused to start.
///
/// Mirrors `Result<(), pnsrouter::router::StartError>`, flattened into one
/// enum with success as its first value. The host id and the item id the
/// two naming variants carry are dropped: the host already knows which
/// object it asked about, and the engine's item id means nothing to it.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsStartResult {
  /// The point may be routed from.
  Ok = 0,
  /// `StartError::AlreadyRouting`.
  AlreadyRouting = 1,
  /// `StartError::UnknownStartItem`.
  UnknownStartItem = 2,
  /// `StartError::NotRoutable`.
  NotRoutable = 3,
  /// `StartError::StartPointViolatesRules`.
  StartPointViolatesRules = 4,
  /// `StartError::PlacerRefused`.
  PlacerRefused = 5,
  /// `StartError::NothingToDrag`.
  NothingToDrag = 6,
  /// `StartError::ComponentDragUnsupported`.
  ComponentDragUnsupported = 7,
  /// `StartError::NotDraggable`.
  NotDraggable = 8,
}

/// What happened to a fix.
///
/// Mirrors `pnsrouter::router::FixOutcome` plus the "nothing was being
/// routed" case, which the crate spells as `Option::None` on
/// `Router::finish`.
#[repr(C)]
#[allow(dead_code)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum PnsFixOutcome {
  /// `FixOutcome::Continue`: the placement carries on, and the session
  /// holds the frame after the fix.
  Continue = 0,
  /// `FixOutcome::Finished`: the route reached its target and was
  /// committed, so the session holds the commit and no frame.
  Finished = 1,
  /// Nothing was being routed, so nothing was committed either.
  NotRouting = 2,
}

/// One polyline of the session's latest preview frame.
///
/// Mirrors `pnsrouter::router::PreviewItem`. The centre line is read point
/// by point with [`ffi_pnsrouter_preview_item_point`], because a variable
/// length list cannot ride in a `#[repr(C)]` struct the host did not
/// allocate.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsPreviewItem {
  /// How many points the centre line has.
  pub point_count: usize,
  /// The full width in nanometres.
  pub width: i64,
  /// The dense copper layer index to draw on.
  pub layer: i32,
  /// The net, counted from one, or zero for no net.
  pub net: u32,
  /// How to draw it.
  pub style: PnsPreviewStyle,
  /// The clearance outline to draw around it in nanometres, or a negative
  /// value when no rule applies.
  pub clearance: i64,
}

/// One via of the session's latest preview frame.
///
/// Mirrors `pnsrouter::router::PreviewVia`.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsPreviewVia {
  /// The centre.
  pub pos: PnsPoint,
  /// The copper diameter in nanometres.
  pub diameter: i64,
  /// The drill diameter in nanometres.
  pub drill: i64,
  /// The first dense copper layer index it spans.
  pub layer_start: i32,
  /// The last dense copper layer index it spans, inclusive.
  pub layer_end: i32,
  /// The net, counted from one, or zero for no net.
  pub net: u32,
  /// How to draw it.
  pub style: PnsPreviewStyle,
  /// The clearance outline to draw around it in nanometres, or a negative
  /// value when no rule applies.
  pub clearance: i64,
}

/// One obstacle the route being placed runs into.
///
/// Mirrors `pnsrouter::router::ViolationMarker`, minus the engine item id,
/// which means nothing to the host.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsViolationMarker {
  /// The obstacle as the host knows it, or zero for something this
  /// session created and the host has no id for yet.
  pub host_id: u64,
  /// The clearance that was asked for and not met, in nanometres.
  pub clearance: i64,
  /// The dense copper layer index to draw the obstacle on instead of its
  /// own, or a negative value to draw it on its own layers.
  pub forced_layer: i32,
  /// Whether the host should hide the obstacle's normal rendering while
  /// this marker is drawn.
  pub hide_original: bool,
}

/// One item a host has to create or rewrite after a commit.
///
/// Mirrors `pnsrouter::router::NewItem` with its
/// `pnsrouter::router::NewGeometry` flattened into the fields
/// [`PnsNewItem::kind`] names, the same way [`PnsShape`] flattens a shape.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct PnsNewItem {
  /// Which of the fields below are meaningful.
  pub kind: PnsNewGeometryKind,
  /// The net, counted from one, or zero for no net. The engine's orphan
  /// net, which a route started in free space is placed on, also reads
  /// back as zero because the host has no net for it.
  pub net: u32,
  /// The first dense copper layer index the item occupies.
  pub layer_start: i32,
  /// The last dense copper layer index the item occupies, inclusive.
  pub layer_end: i32,
  /// The host object the item descends from, or zero for a freshly routed
  /// one.
  pub source: u64,
  /// One end of a segment's centre line.
  pub p1: PnsPoint,
  /// The other end of a segment's centre line.
  pub p2: PnsPoint,
  /// The full width of a segment in nanometres.
  pub width: i64,
  /// The centre of a via.
  pub pos: PnsPoint,
  /// The copper diameter of a via in nanometres.
  pub diameter: i64,
  /// The drill diameter of a via in nanometres.
  pub drill: i64,
  /// How far through the copper stack a via reaches.
  pub via_type: PnsViaType,
}

/// A routing session over one board snapshot.
///
/// Owned by C++ through a `RustHandle` and deleted by
/// [`ffi_pnsrouter_delete`].
///
/// The session keeps the latest preview frame and the latest commit inside
/// itself, and C++ reads them back through the accessors below right after
/// the call that produced them. That keeps the boundary to one opaque
/// handle: a `PreviewFrame` and a `CommitDiff` both hold variable length
/// lists, so handing either out by value would need a second handle type
/// and a second deleter for a value that is read once and dropped.
pub struct PnsRouter {
  /// The session itself.
  router: Router,
  /// The rule table, kept so that [`ffi_pnsrouter_set_settings`] can
  /// derive the sizes again the way [`ffi_pnsrouter_new`] did.
  rules: LibrePcbRules,
  /// How many copper layers the snapshot described.
  copper_layer_count: u8,
  /// The frame of the last event that produced one.
  frame: PreviewFrame,
  /// The commit of the last `stop_routing` or terminal fix.
  diff: CommitDiff,
  /// The answer of the last [`ffi_pnsrouter_hover`].
  hover: Vec<HostId>,
}

impl PnsRouter {
  /// Store one preview frame as the session's latest.
  fn set_frame(&mut self, frame: PreviewFrame) {
    self.frame = frame;
  }

  /// Store one commit as the session's latest, which also ends the frame
  /// the commit grew out of.
  fn set_diff(&mut self, diff: CommitDiff) {
    self.diff = diff;
    self.frame = PreviewFrame::default();
  }
}

/// Derive the engine's settings and sizes from the host's values.
///
/// Shared by [`ffi_pnsrouter_new`] and [`ffi_pnsrouter_set_settings`], so
/// that a session created with one set of values and a session updated to
/// it are the same session.
///
/// `base` is the settings to change, which is
/// `RoutingSettings::default()` for a fresh session and the session's own
/// settings for an update, so that a corner mode the user cycled survives
/// a width change.
fn derive_settings(
  base: RoutingSettings,
  rules: &LibrePcbRules,
  copper_layer_count: u8,
  settings: &PnsRouterSettings,
) -> (RoutingSettings, Sizes) {
  let routing_settings = RoutingSettings {
    mode: match settings.mode {
      0 => RouterMode::MarkObstacles,
      1 => RouterMode::Shove,
      _ => RouterMode::Walkaround,
    },
    shove_iteration_limit: settings.shove_iteration_limit,
    ..base
  };

  let mut sizes = Sizes {
    clearance: rules.max_clearance,
    min_clearance: rules.board.min_copper_copper_clearance,
    board_min_track_width: rules.board.min_copper_width,
    track_width: to_length(settings.track_width).unwrap_or(0),
    via_diameter: to_length(settings.via_diameter).unwrap_or(0),
    via_drill: to_length(settings.via_drill).unwrap_or(0),
    // The router only ever places through vias for now, so the via layer
    // pair covers the whole copper stack; see the integration design
    // note, section 1.8.
    via_type: ViaType::Through,
    hole_to_hole: rules.board.min_drill_drill_clearance,
    ..Sizes::default()
  };

  sizes.clear_layer_pairs();
  sizes.add_layer_pair(0, i32::from(copper_layer_count).max(1) - 1);

  (routing_settings, sizes)
}

/// Create a routing session, consuming the snapshot.
///
/// Wraps `pnsrouter::router::Router::new`. The snapshot pointer is invalid
/// afterwards and must not be deleted again.
#[no_mangle]
extern "C" fn ffi_pnsrouter_new(
  snapshot: *mut PnsSnapshot,
  settings: &PnsRouterSettings,
) -> *mut PnsRouter {
  assert!(!snapshot.is_null());

  let mut snapshot = unsafe { Box::from_raw(snapshot) };

  snapshot.rules.recompute_max_clearance();
  snapshot.snapshot.max_clearance = snapshot.rules.max_clearance;

  let copper_layer_count = snapshot.snapshot.copper_layer_count;
  let (routing_settings, sizes) = derive_settings(
    RoutingSettings::default(),
    &snapshot.rules,
    copper_layer_count,
    settings,
  );

  let mut router = Router::new(
    &snapshot.snapshot,
    Box::new(snapshot.rules.clone()),
    routing_settings,
    sizes,
  );

  if settings.record_session {
    // The recording has to open with the board the session runs on, and
    // this is the only place that still holds it.
    router.start_recording(&snapshot.snapshot);
  }

  Box::into_raw(Box::new(PnsRouter {
    router,
    rules: snapshot.rules.clone(),
    copper_layer_count,
    frame: PreviewFrame::default(),
    diff: CommitDiff::default(),
    hover: Vec::new(),
  }))
}

/// Delete a [`PnsRouter`] object.
#[no_mangle]
extern "C" fn ffi_pnsrouter_delete(obj: *mut PnsRouter) {
  assert!(!obj.is_null());
  unsafe { drop(Box::from_raw(obj)) };
}

/// How many copper layers the session's board has.
///
/// The smallest useful read back, so that a test can prove the session
/// really was built from the snapshot it was handed.
#[no_mangle]
extern "C" fn ffi_pnsrouter_copper_layer_count(obj: &PnsRouter) -> u8 {
  obj.copper_layer_count
}

/// Whether a route is currently being placed.
///
/// Wraps `pnsrouter::router::Router::routing_in_progress`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_routing_in_progress(obj: &PnsRouter) -> bool {
  obj.router.routing_in_progress()
}

/// Replace the routing mode and the sizes of a session.
///
/// Wraps `pnsrouter::router::Router::set_settings` and
/// `pnsrouter::router::Router::set_sizes`. A running placement keeps the
/// sizes it started with, because the crate's placer has no mid route
/// entry point for them yet; see the port note on `Router::set_sizes`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_set_settings(
  obj: &mut PnsRouter,
  settings: &PnsRouterSettings,
) {
  let (routing_settings, sizes) = derive_settings(
    *obj.router.settings(),
    &obj.rules,
    obj.copper_layer_count,
    settings,
  );

  obj.router.set_settings(routing_settings);
  obj.router.set_sizes(sizes);
}

/// The copper layer the route is being placed on, or a negative value when
/// nothing is being routed.
///
/// Wraps `pnsrouter::router::Router::current_layer`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_current_layer(obj: &PnsRouter) -> i32 {
  obj.router.current_layer().unwrap_or(-1)
}

/// Whether the next fix would place a via.
///
/// Wraps `pnsrouter::router::Router::placing_via`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_placing_via(obj: &PnsRouter) -> bool {
  obj.router.placing_via()
}

/// Take the session recording out, in the crate's own text format.
///
/// Wraps `pnsrouter::router::Router::take_recording` followed by
/// `pnsrouter::eventlog::SessionRecording::to_text`. Taking the recording
/// ends it, which is the crate's semantics, so a host that wants to keep
/// recording has to build a new session.
///
/// Answers false and leaves `out` alone when the session was not created
/// with `record_session`, or when its recording has already been taken.
/// The text parses back with
/// `pnsrouter::eventlog::SessionRecording::from_text`, so it can be
/// dropped into the crate's `tests/fixtures/sessions/` unchanged.
#[no_mangle]
extern "C" fn ffi_pnsrouter_take_recording(
  obj: &mut PnsRouter,
  out: &mut QString,
) -> bool {
  let Some(recording) = obj.router.take_recording() else {
    return false;
  };

  qstring_set(out, &recording.to_text());
  true
}

// ---------------------------------------------------------------------
// Session events
// ---------------------------------------------------------------------

/// Find every host object under a point and return how many there are.
///
/// Wraps `pnsrouter::router::Router::hover`. `layer` is the dense copper
/// layer index to filter by, or a negative value for "any layer". The
/// answer is kept in the session and read back with
/// [`ffi_pnsrouter_hover_at`], for the same reason the preview is; see
/// [`PnsRouter`].
#[no_mangle]
extern "C" fn ffi_pnsrouter_hover(
  obj: &mut PnsRouter,
  at: PnsPoint,
  layer: i32,
) -> usize {
  let filter = if layer < 0 { None } else { Some(layer) };

  obj.hover = obj.router.hover(to_cursor(at), filter);

  obj.hover.len()
}

/// One host id of the last [`ffi_pnsrouter_hover`].
#[no_mangle]
extern "C" fn ffi_pnsrouter_hover_at(obj: &PnsRouter, index: usize) -> u64 {
  let Some(host) = obj.hover.get(index) else {
    debug_assert!(false, "hover index {index} is out of range");

    return 0;
  };

  host.0
}

/// Whether a route may be started at a point.
///
/// Wraps `pnsrouter::router::Router::is_starting_point_routable`. `start`
/// is the host object under the cursor, or zero for free space.
#[no_mangle]
extern "C" fn ffi_pnsrouter_is_starting_point_routable(
  obj: &PnsRouter,
  at: PnsPoint,
  start: u64,
  layer: i32,
) -> PnsStartResult {
  to_start_result(obj.router.is_starting_point_routable(
    to_cursor(at),
    to_host_id(start),
    layer,
  ))
}

/// Begin routing a track.
///
/// Wraps `pnsrouter::router::Router::start_routing`. `start` is the host
/// object under the cursor, or zero for free space. On success the
/// session holds the frame of a placement that has not been moved yet,
/// and on failure it holds an empty one.
#[no_mangle]
extern "C" fn ffi_pnsrouter_start_routing(
  obj: &mut PnsRouter,
  at: PnsPoint,
  start: u64,
  layer: i32,
) -> PnsStartResult {
  match obj
    .router
    .start_routing(to_cursor(at), to_host_id(start), layer)
  {
    Ok(frame) => {
      obj.set_frame(frame);

      PnsStartResult::Ok
    }
    Err(error) => {
      obj.set_frame(PreviewFrame::default());

      to_start_result(Err(error))
    }
  }
}

/// Begin dragging an existing track or via.
///
/// Wraps `pnsrouter::router::Router::start_dragging`. `host_id` is the
/// board object to drag, or zero for none, which is
/// [`PnsStartResult::NothingToDrag`]. The crate takes a slice and drags
/// several traces at once when it gets several; a set of nothing but
/// pads is KiCad's component drag and is refused with
/// [`PnsStartResult::ComponentDragUnsupported`]. One host id is what
/// crosses here today, so a multi drag waits for a host gesture that
/// selects several traces.
///
/// `free_angle` drags the clicked corner without the 45 degree
/// constraint; every other drag mode is decided by the crate from the
/// clicked object and the click position.
///
/// On success the session holds the frame of a drag that has not moved
/// yet, which is empty, so a host follows this with a move exactly as
/// KiCad's does.
#[no_mangle]
extern "C" fn ffi_pnsrouter_start_dragging(
  obj: &mut PnsRouter,
  at: PnsPoint,
  host_id: u64,
  free_angle: bool,
) -> PnsStartResult {
  let host = to_host_id(host_id);
  let items: &[HostId] = match &host {
    Some(host) => std::slice::from_ref(host),
    None => &[],
  };

  match obj.router.start_dragging(to_cursor(at), items, free_angle) {
    Ok(frame) => {
      obj.set_frame(frame);

      PnsStartResult::Ok
    }
    Err(error) => {
      obj.set_frame(PreviewFrame::default());

      to_start_result(Err(error))
    }
  }
}

/// Whether an existing object is being dragged.
///
/// `pnsrouter::router::RouterState::DragSegment`, which is the one thing
/// [`ffi_pnsrouter_routing_in_progress`] cannot tell apart from a
/// placement. The state enum itself does not cross: it has three values
/// and this pair of predicates already answers all of them.
#[no_mangle]
extern "C" fn ffi_pnsrouter_is_dragging(obj: &PnsRouter) -> bool {
  obj.router.state() == RouterState::DragSegment
}

/// Move the end of the route, and store the frame it produced.
///
/// Wraps `pnsrouter::router::Router::move_to`. `at` is already snapped:
/// snapping is host work. `end` is the host object under the cursor, or
/// zero for free space.
#[no_mangle]
extern "C" fn ffi_pnsrouter_move_to(
  obj: &mut PnsRouter,
  at: PnsPoint,
  end: u64,
) {
  let frame = obj.router.move_to(to_cursor(at), to_host_id(end));

  obj.set_frame(frame);
}

/// Pin the route down to where the cursor is.
///
/// Wraps `pnsrouter::router::Router::fix_route`. A
/// [`PnsFixOutcome::Continue`] leaves the frame after the fix in the
/// session; a [`PnsFixOutcome::Finished`] leaves the commit there instead
/// and clears the frame.
#[no_mangle]
extern "C" fn ffi_pnsrouter_fix_route(
  obj: &mut PnsRouter,
  at: PnsPoint,
  end: u64,
  force_finish: bool,
) -> PnsFixOutcome {
  match obj
    .router
    .fix_route(to_cursor(at), to_host_id(end), force_finish)
  {
    FixOutcome::Continue(frame) => {
      obj.set_frame(frame);

      PnsFixOutcome::Continue
    }
    FixOutcome::Finished(diff) => {
      obj.set_diff(diff);

      PnsFixOutcome::Finished
    }
  }
}

/// Route the rest of the way to the nearest unconnected anchor and finish.
///
/// Wraps `pnsrouter::router::Router::finish`, whose `None` becomes
/// [`PnsFixOutcome::NotRouting`]: nothing was being routed, nothing
/// unconnected was left to reach, or the route did not settle on the
/// anchor. Nothing was committed in that case and the session is left
/// exactly as it was.
#[no_mangle]
extern "C" fn ffi_pnsrouter_finish(obj: &mut PnsRouter) -> PnsFixOutcome {
  match obj.router.finish() {
    Some(FixOutcome::Continue(frame)) => {
      obj.set_frame(frame);

      PnsFixOutcome::Continue
    }
    Some(FixOutcome::Finished(diff)) => {
      obj.set_diff(diff);

      PnsFixOutcome::Finished
    }
    None => PnsFixOutcome::NotRouting,
  }
}

/// Undo the last fix and answer where the undone leg began.
///
/// Wraps `pnsrouter::router::Router::undo_last_segment`, whose answer a
/// host uses to warp the cursor back there. False when nothing was being
/// routed or when there was nothing to undo, in which case `out` is not
/// written.
#[no_mangle]
extern "C" fn ffi_pnsrouter_undo_last_segment(
  obj: &mut PnsRouter,
  out: &mut PnsPoint,
) -> bool {
  match obj.router.undo_last_segment() {
    Some(at) => {
      *out = to_ffi_point(at);

      true
    }
    None => false,
  }
}

/// Move the route to another copper layer.
///
/// Wraps `pnsrouter::router::Router::switch_layer`, which refuses once a
/// fix has ended a leg without leaving a via behind.
#[no_mangle]
extern "C" fn ffi_pnsrouter_switch_layer(
  obj: &mut PnsRouter,
  layer: i32,
) -> bool {
  obj.router.switch_layer(layer)
}

/// Arm or disarm the via the next fix would place.
///
/// Wraps `pnsrouter::router::Router::toggle_via_placement`. The answer is
/// whether the request was honoured, not the new state; read that back
/// with [`ffi_pnsrouter_placing_via`]. The via is only materialised on the
/// next move, so a host has to move before the preview shows it.
#[no_mangle]
extern "C" fn ffi_pnsrouter_toggle_via_placement(obj: &mut PnsRouter) -> bool {
  obj.router.toggle_via_placement()
}

/// Turn the route's first corner the other way.
///
/// Wraps `pnsrouter::router::Router::flip_posture`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_flip_posture(obj: &mut PnsRouter) {
  obj.router.flip_posture();
}

/// Cycle between the 45 and the 90 degree corner mode.
///
/// Wraps `pnsrouter::router::Router::toggle_corner_mode`.
#[no_mangle]
extern "C" fn ffi_pnsrouter_toggle_corner_mode(obj: &mut PnsRouter) {
  obj.router.toggle_corner_mode();
}

/// Commit what was routed and end the session.
///
/// Wraps `pnsrouter::router::Router::stop_routing`. The commit is left in
/// the session and read back with the accessors below. An idle session
/// answers with an empty commit.
#[no_mangle]
extern "C" fn ffi_pnsrouter_stop_routing(obj: &mut PnsRouter) {
  let diff = obj.router.stop_routing();

  obj.set_diff(diff);
}

/// Throw the session away without committing anything.
///
/// Wraps `pnsrouter::router::Router::abort_routing`. Both the frame and
/// the commit are cleared, so a host that reads them afterwards sees
/// nothing rather than the state the aborted route left behind.
#[no_mangle]
extern "C" fn ffi_pnsrouter_abort_routing(obj: &mut PnsRouter) {
  obj.router.abort_routing();
  obj.set_diff(CommitDiff::default());
}

// ---------------------------------------------------------------------
// Reading the latest preview frame
// ---------------------------------------------------------------------

/// How many polylines the latest frame holds.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_item_count(obj: &PnsRouter) -> usize {
  obj.frame.items.len()
}

/// One polyline of the latest frame, minus its points.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_item(
  obj: &PnsRouter,
  index: usize,
  out: &mut PnsPreviewItem,
) {
  let Some(item) = obj.frame.items.get(index) else {
    debug_assert!(false, "preview item index {index} is out of range");

    return;
  };

  *out = PnsPreviewItem {
    point_count: item.chain.point_count(),
    width: i64::from(item.width),
    layer: item.layer,
    net: to_net_number(item.net),
    style: to_ffi_style(item.style),
    clearance: to_ffi_clearance(item.clearance),
  };
}

/// One point of one polyline of the latest frame.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_item_point(
  obj: &PnsRouter,
  item_index: usize,
  point_index: usize,
) -> PnsPoint {
  let Some(item) = obj.frame.items.get(item_index) else {
    debug_assert!(false, "preview item index {item_index} is out of range");

    return PnsPoint { x: 0, y: 0 };
  };

  if point_index >= item.chain.point_count() {
    debug_assert!(false, "preview point index {point_index} is out of range");

    return PnsPoint { x: 0, y: 0 };
  }

  to_ffi_point(item.chain.point(point_index))
}

/// Whether the latest frame holds the via the next fix would place.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_has_via(obj: &PnsRouter) -> bool {
  obj.frame.via.is_some()
}

/// The via the next fix would place, when
/// [`ffi_pnsrouter_preview_has_via`] answers true.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_via(
  obj: &PnsRouter,
  out: &mut PnsPreviewVia,
) {
  let Some(via) = obj.frame.via.as_ref() else {
    debug_assert!(false, "the frame holds no head via");

    return;
  };

  *out = to_ffi_via(via);
}

/// How many vias this session has already fixed.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_fixed_via_count(obj: &PnsRouter) -> usize {
  obj.frame.fixed_vias.len()
}

/// One via this session has already fixed.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_fixed_via(
  obj: &PnsRouter,
  index: usize,
  out: &mut PnsPreviewVia,
) {
  let Some(via) = obj.frame.fixed_vias.get(index) else {
    debug_assert!(false, "fixed via index {index} is out of range");

    return;
  };

  *out = to_ffi_via(via);
}

/// How many points the rat line from the end of the route holds, zero
/// when there is none.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_ratline_point_count(
  obj: &PnsRouter,
) -> usize {
  obj.frame.ratline.as_ref().map_or(0, LineChain::point_count)
}

/// One point of the rat line from the end of the route.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_ratline_point(
  obj: &PnsRouter,
  index: usize,
) -> PnsPoint {
  let Some(ratline) = obj.frame.ratline.as_ref() else {
    debug_assert!(false, "the frame holds no rat line");

    return PnsPoint { x: 0, y: 0 };
  };

  if index >= ratline.point_count() {
    debug_assert!(false, "rat line point index {index} is out of range");

    return PnsPoint { x: 0, y: 0 };
  }

  to_ffi_point(ratline.point(index))
}

/// How many obstacles the route being placed runs into.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_violation_count(obj: &PnsRouter) -> usize {
  obj.frame.violations.len()
}

/// One obstacle the route being placed runs into.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_violation(
  obj: &PnsRouter,
  index: usize,
  out: &mut PnsViolationMarker,
) {
  let Some(marker) = obj.frame.violations.get(index) else {
    debug_assert!(false, "violation index {index} is out of range");

    return;
  };

  *out = PnsViolationMarker {
    host_id: marker.host.map_or(0, |host| host.0),
    clearance: i64::from(marker.clearance),
    forced_layer: marker.forced_layer.unwrap_or(-1),
    hide_original: marker.hide_original,
  };
}

/// How many board objects the host must stop drawing.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_hidden_count(obj: &PnsRouter) -> usize {
  obj.frame.hidden.len()
}

/// One board object the host must stop drawing.
#[no_mangle]
extern "C" fn ffi_pnsrouter_preview_hidden_at(
  obj: &PnsRouter,
  index: usize,
) -> u64 {
  let Some(host) = obj.frame.hidden.get(index) else {
    debug_assert!(false, "hidden index {index} is out of range");

    return 0;
  };

  host.0
}

// ---------------------------------------------------------------------
// Reading the latest commit
// ---------------------------------------------------------------------

/// How many board objects the latest commit deletes.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_removed_count(obj: &PnsRouter) -> usize {
  obj.diff.removed.len()
}

/// One board object the latest commit deletes.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_removed_at(
  obj: &PnsRouter,
  index: usize,
) -> u64 {
  let Some(host) = obj.diff.removed.get(index) else {
    debug_assert!(false, "removed index {index} is out of range");

    return 0;
  };

  host.0
}

/// How many board objects the latest commit creates.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_added_count(obj: &PnsRouter) -> usize {
  obj.diff.added.len()
}

/// One board object the latest commit creates.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_added_at(
  obj: &PnsRouter,
  index: usize,
  out: &mut PnsNewItem,
) {
  let Some(item) = obj.diff.added.get(index) else {
    debug_assert!(false, "added index {index} is out of range");

    return;
  };

  *out = to_ffi_new_item(item);
}

/// How many board objects the latest commit rewrites in place.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_updated_count(obj: &PnsRouter) -> usize {
  obj.diff.updated.len()
}

/// One board object the latest commit rewrites in place, and the host id
/// whose identity it keeps.
#[no_mangle]
extern "C" fn ffi_pnsrouter_commit_updated_at(
  obj: &PnsRouter,
  index: usize,
  out_host: &mut u64,
  out_item: &mut PnsNewItem,
) {
  let Some((host, item)) = obj.diff.updated.get(index) else {
    debug_assert!(false, "updated index {index} is out of range");

    return;
  };

  *out_host = host.0;
  *out_item = to_ffi_new_item(item);
}

// ---------------------------------------------------------------------
// Session conversion helpers
// ---------------------------------------------------------------------

/// Narrow one cursor point to the engine's `i32` nanometres.
///
/// Unlike the snapshot builder, which rejects an out of range coordinate
/// so that the host can name the board item, a cursor is clamped: it is
/// not a board object, there is nothing to name, and clamping to the range
/// the engine works in cannot wrap.
fn to_cursor(point: PnsPoint) -> Vec2 {
  let clamp = |value: i64| {
    value.clamp(-MAX_COORDINATE, MAX_COORDINATE) as i32 // Never truncates.
  };

  Vec2::new(clamp(point.x), clamp(point.y))
}

/// Widen one engine point back to host nanometres.
fn to_ffi_point(at: Vec2) -> PnsPoint {
  PnsPoint {
    x: i64::from(at.x),
    y: i64::from(at.y),
  }
}

/// Turn a host id of zero into "no object".
fn to_host_id(host: u64) -> Option<HostId> {
  (host > 0).then_some(HostId(host))
}

/// Turn an engine net back into the host's net number.
///
/// The inverse of `to_item`'s net handling: no net is zero and net `n` is
/// `n + 1`. The engine's orphan net, `NetId(u32::MAX)`, is the one net a
/// host never sent and has no signal for, so it reads back as "no net"
/// too; the C++ side turns both into a null `NetSignal`.
fn to_net_number(net: Option<NetId>) -> u32 {
  match net {
    Some(net) if net.0 < u32::MAX => net.0 + 1,
    _ => 0,
  }
}

/// Turn an optional clearance into the negative-for-none convention.
fn to_ffi_clearance(clearance: Option<i32>) -> i64 {
  clearance.map_or(-1, i64::from)
}

/// Turn one engine preview style into its FFI value.
fn to_ffi_style(style: PreviewStyle) -> PnsPreviewStyle {
  match style {
    PreviewStyle::Head => PnsPreviewStyle::Head,
    PreviewStyle::Tail => PnsPreviewStyle::Tail,
    PreviewStyle::Hover => PnsPreviewStyle::Hover,
    PreviewStyle::SemiSolid => PnsPreviewStyle::SemiSolid,
    PreviewStyle::Collision => PnsPreviewStyle::Collision,
  }
}

/// Turn one engine via type into its FFI value.
///
/// The engine has five values where LibrePCB has three. A micro via is a
/// blind via between an outer layer and its neighbour, so it reports as
/// blind, and the unset value reports as a through via, which is the only
/// kind this router places.
fn to_ffi_via_type(via_type: ViaType) -> PnsViaType {
  match via_type {
    ViaType::Blind | ViaType::MicroVia => PnsViaType::Blind,
    ViaType::Buried => PnsViaType::Buried,
    ViaType::Through | ViaType::NotDefined => PnsViaType::Through,
  }
}

/// Turn one engine preview via into its FFI struct.
fn to_ffi_via(via: &PreviewVia) -> PnsPreviewVia {
  PnsPreviewVia {
    pos: to_ffi_point(via.pos),
    diameter: i64::from(via.diameter),
    drill: i64::from(via.drill),
    layer_start: via.layers.start(),
    layer_end: via.layers.end(),
    net: to_net_number(via.net),
    style: to_ffi_style(via.style),
    clearance: to_ffi_clearance(via.clearance),
  }
}

/// Turn one engine commit item into its FFI struct.
fn to_ffi_new_item(item: &NewItem) -> PnsNewItem {
  let mut out = PnsNewItem {
    kind: PnsNewGeometryKind::Segment,
    net: to_net_number(item.net),
    layer_start: item.layers.start(),
    layer_end: item.layers.end(),
    source: item.source.map_or(0, |host| host.0),
    p1: PnsPoint { x: 0, y: 0 },
    p2: PnsPoint { x: 0, y: 0 },
    width: 0,
    pos: PnsPoint { x: 0, y: 0 },
    diameter: 0,
    drill: 0,
    via_type: PnsViaType::Through,
  };

  match item.geometry {
    NewGeometry::Segment { seg, width } => {
      out.kind = PnsNewGeometryKind::Segment;
      out.p1 = to_ffi_point(seg.a);
      out.p2 = to_ffi_point(seg.b);
      out.width = i64::from(width);
    }
    NewGeometry::Via {
      pos,
      diameter,
      drill,
      via_type,
    } => {
      out.kind = PnsNewGeometryKind::Via;
      out.pos = to_ffi_point(pos);
      out.diameter = i64::from(diameter);
      out.drill = i64::from(drill);
      out.via_type = to_ffi_via_type(via_type);
    }
  }

  out
}

/// Flatten a start result into its FFI enum.
fn to_start_result(result: Result<(), StartError>) -> PnsStartResult {
  match result {
    Ok(()) => PnsStartResult::Ok,
    Err(StartError::AlreadyRouting) => PnsStartResult::AlreadyRouting,
    Err(StartError::UnknownStartItem(_)) => PnsStartResult::UnknownStartItem,
    Err(StartError::NotRoutable(_)) => PnsStartResult::NotRoutable,
    Err(StartError::StartPointViolatesRules) => {
      PnsStartResult::StartPointViolatesRules
    }
    Err(StartError::PlacerRefused) => PnsStartResult::PlacerRefused,
    Err(StartError::NothingToDrag) => PnsStartResult::NothingToDrag,
    Err(StartError::ComponentDragUnsupported) => {
      PnsStartResult::ComponentDragUnsupported
    }
    Err(StartError::NotDraggable(_)) => PnsStartResult::NotDraggable,
  }
}
