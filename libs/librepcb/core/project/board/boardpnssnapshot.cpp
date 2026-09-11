/*
 * LibrePCB - Professional EDA for everyone!
 * Copyright (C) 2013 LibrePCB Developers, see AUTHORS.md for contributors.
 * https://librepcb.org/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "boardpnssnapshot.h"

#include "../../exceptions.h"
#include "../../geometry/pad.h"
#include "../../geometry/padgeometry.h"
#include "../../geometry/padhole.h"
#include "../../geometry/path.h"
#include "../../geometry/via.h"
#include "../../geometry/zone.h"
#include "../../types/layer.h"
#include "../../utils/transform.h"
#include "../circuit/circuit.h"
#include "../circuit/netclass.h"
#include "../circuit/netsignal.h"
#include "../project.h"
#include "board.h"
#include "boarddesignrules.h"
#include "drc/boarddesignrulechecksettings.h"
#include "items/bi_device.h"
#include "items/bi_hole.h"
#include "items/bi_netline.h"
#include "items/bi_netsegment.h"
#include "items/bi_pad.h"
#include "items/bi_polygon.h"
#include "items/bi_via.h"
#include "items/bi_zone.h"

#include <librepcb/rust-core/ffi.h>

#include <QtCore>

#include <algorithm>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

/**
 * The arc flattening tolerance ::librepcb::PadGeometry uses internally, so
 * that the router and the plane builder see the same polygon.
 */
static PositiveLength maxArcTolerance() noexcept {
  return PositiveLength(5000);
}

static rs::PnsPoint toFfi(const Point& p) noexcept {
  return rs::PnsPoint{p.getX().toNm(), p.getY().toNm()};
}

static rs::PnsShape circleShape(const Point& center,
                                const Length& diameter) noexcept {
  rs::PnsShape shape = {};
  shape.kind = rs::PnsShapeKind::Circle;
  shape.center = toFfi(center);
  shape.radius = diameter.toNm() / 2;
  return shape;
}

static rs::PnsShape segmentShape(const Point& p1, const Point& p2,
                                 const Length& width) noexcept {
  rs::PnsShape shape = {};
  shape.kind = rs::PnsShapeKind::Segment;
  shape.p1 = toFfi(p1);
  shape.p2 = toFfi(p2);
  shape.radius = width.toNm();
  return shape;
}

/**
 * Build a polygon shape referring to a caller owned vertex buffer.
 *
 * The buffer must outlive the FFI call. It is always allocated with at
 * least one element because an empty std::vector's data() pointer made a
 * Rust debug build panic, see
 * ::librepcb::InteractiveHtmlBom::addFootprint().
 */
static rs::PnsShape polygonShape(const Path& path,
                                 std::vector<rs::PnsPoint>& buffer) noexcept {
  const Path flat = path.flattenedArcs(maxArcTolerance()).toClosedPath();
  const QVector<Vertex>& vertices = flat.getVertices();

  buffer.clear();
  // Always allocate a non-empty buffer so that the pointer handed to Rust
  // points at a valid memory location, see
  // ::librepcb::InteractiveHtmlBom::addFootprint().
  buffer.reserve(std::max<std::size_t>(vertices.count(), 1));
  // A closed path repeats its first vertex; the engine closes the polygon
  // itself, so the repetition is dropped.
  const int count = flat.isClosed() ? (vertices.count() - 1) : vertices.count();
  for (int i = 0; i < count; ++i) {
    buffer.push_back(toFfi(vertices.at(i).getPos()));
  }
  buffer.shrink_to_fit();

  rs::PnsShape shape = {};
  shape.kind = rs::PnsShapeKind::Polygon;
  shape.vertices = buffer.data();
  shape.vertex_count = buffer.size();
  return shape;
}

/**
 * Build the shape of one pad or board drill.
 *
 * A round hole is a circle, a straight two point slot is a capsule. A
 * curved or multi segment slot is approximated by the capsule between its
 * first and last vertex, which is wider than the slot but never narrower.
 */
static rs::PnsShape drillShape(const Path& path,
                               const Length& diameter) noexcept {
  const QVector<Vertex>& vertices = path.getVertices();
  if (vertices.count() < 2) {
    return circleShape(vertices.first().getPos(), diameter);
  }
  return segmentShape(vertices.first().getPos(), vertices.last().getPos(),
                      diameter);
}

/**
 * Twice the signed area of the triangle a, b, c.
 *
 * Positive when the three turn counterclockwise. The arithmetic is floating
 * point because the product of two nanometre coordinate differences
 * overflows a signed 64 bit integer on an outline spanning the whole
 * coordinate range the router accepts. Only the sign is read, and a double
 * carries it everywhere except within a few micrometres of a degenerate
 * triangle, where a wrong answer costs one ear rather than a wrong shape.
 */
static qreal doubledArea(const Point& a, const Point& b,
                         const Point& c) noexcept {
  const qreal abx = static_cast<qreal>(b.getX().toNm() - a.getX().toNm());
  const qreal aby = static_cast<qreal>(b.getY().toNm() - a.getY().toNm());
  const qreal acx = static_cast<qreal>(c.getX().toNm() - a.getX().toNm());
  const qreal acy = static_cast<qreal>(c.getY().toNm() - a.getY().toNm());
  return (abx * acy) - (aby * acx);
}

/**
 * Whether p lies in the triangle a, b, c, which must turn counterclockwise.
 *
 * A point exactly on an edge counts as inside, so that a corner touching a
 * candidate ear blocks it instead of letting two triangles overlap.
 */
static bool isInTriangle(const Point& a, const Point& b, const Point& c,
                         const Point& p) noexcept {
  return (doubledArea(a, b, p) >= 0) && (doubledArea(b, c, p) >= 0) &&
      (doubledArea(c, a, p) >= 0);
}

/**
 * Cut a closed outline into triangles by ear clipping.
 *
 * The router's polygon shape is filled and assumed convex, so an area
 * obstacle has to arrive as triangles. That is what KiCad's own router does
 * with a rule area too: one simple shape per triangle of the outline, all
 * flagged as compound primitives.
 *
 * LibrePCB has no polygon triangulation to borrow here. The Delaunay library
 * under `libs/` triangulates a point set, so on a concave outline it covers
 * the convex hull rather than the outline (::librepcb::AirWiresBuilder is
 * its only user), and the 3D scene builder's GLU tesselator lives in the
 * editor library, needs an OpenGL dependency LibrePCB can be built without,
 * and works in floating point millimetres. Ear clipping is exact on the
 * integer coordinates and handles a concave outline, which is what this
 * needs.
 *
 * @return One three vertex path per triangle, or an empty vector for an
 *         outline which is not a simple polygon. A convex hull is
 *         deliberately not offered as a fallback: it would keep the router
 *         out of area the zone does not cover, which is a wrong obstacle
 *         rather than a missing one.
 */
static QVector<Path> triangulate(const Path& outline) noexcept {
  // Arcs are flattened as the board outline's are, so that the router and
  // the design rule check see the same polygon.
  const Path flat = outline.flattenedArcs(maxArcTolerance()).toClosedPath();

  // A repeated vertex is no corner, and the closing repetition of a closed
  // path is one of them.
  QVector<Point> ring;
  foreach (const Vertex& vertex, flat.getVertices()) {
    if (ring.isEmpty() || (ring.last() != vertex.getPos())) {
      ring.append(vertex.getPos());
    }
  }
  if ((ring.count() > 1) && (ring.first() == ring.last())) {
    ring.removeLast();
  }
  if (ring.count() < 3) {
    return QVector<Path>();
  }

  // A corner exactly on the line between its neighbours adds no area and
  // would leave the ear clipping below with a degenerate triangle it cannot
  // cut, so it goes first. Removing one can make a neighbour collinear in
  // turn, hence the loop. Users produce these by clicking on an edge while
  // drawing a zone.
  for (bool removed = true; removed && (ring.count() >= 3);) {
    removed = false;
    for (int i = 0; (!removed) && (i < ring.count()); ++i) {
      const Point& previous = ring.at((i + ring.count() - 1) % ring.count());
      const Point& next = ring.at((i + 1) % ring.count());
      if (doubledArea(previous, ring.at(i), next) == 0) {
        ring.removeAt(i);
        removed = true;
      }
    }
  }
  if (ring.count() < 3) {
    return QVector<Path>();
  }

  // Ear clipping wants a counterclockwise ring, which is the winding
  // isInTriangle() assumes.
  qreal area = 0;
  for (int i = 1; i < (ring.count() - 1); ++i) {
    area += doubledArea(ring.first(), ring.at(i), ring.at(i + 1));
  }
  if (area == 0) {
    return QVector<Path>();  // No area, so no obstacle.
  }
  if (area < 0) {
    std::reverse(ring.begin(), ring.end());
  }

  QVector<Path> triangles;
  triangles.reserve(ring.count() - 2);
  while (ring.count() > 2) {
    bool clipped = false;
    for (int i = 0; (!clipped) && (i < ring.count()); ++i) {
      const Point previous = ring.at((i + ring.count() - 1) % ring.count());
      const Point current = ring.at(i);
      const Point next = ring.at((i + 1) % ring.count());

      // A reflex or a collinear corner is no ear.
      if (doubledArea(previous, current, next) <= 0) {
        continue;
      }

      // Nor is one which would swallow another corner of the ring.
      bool occupied = false;
      for (int k = 0; (!occupied) && (k < ring.count()); ++k) {
        const Point& candidate = ring.at(k);
        if ((candidate == previous) || (candidate == current) ||
            (candidate == next)) {
          continue;
        }
        occupied = isInTriangle(previous, current, next, candidate);
      }
      if (occupied) {
        continue;
      }

      triangles.append(Path({Vertex(previous), Vertex(current), Vertex(next)}));
      ring.removeAt(i);
      clipped = true;
    }

    if (!clipped) {
      // Every corner was rejected, so the outline is not a simple polygon.
      return QVector<Path>();
    }
  }

  return triangles;
}

static rs::PnsItemHeader makeHeader(quint64 hostId, quint32 net, int layerStart,
                                    int layerEnd) noexcept {
  rs::PnsItemHeader header = {};
  header.host_id = hostId;
  header.net = net;
  header.layer_start = layerStart;
  header.layer_end = layerEnd;
  header.locked = false;
  header.routable = true;
  header.free_pad = false;
  header.compound_primitive = false;
  header.board_edge = false;
  header.keepout = false;
  header.copper_clearance = -1;
  return header;
}

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

static RustHandle<rs::PnsSnapshot> construct(int copperLayerCount) {
  if (auto obj = rs::ffi_pnsrouter_snapshot_new(
          static_cast<uint8_t>(copperLayerCount))) {
    return RustHandle<rs::PnsSnapshot>(*obj,
                                       &rs::ffi_pnsrouter_snapshot_delete);
  } else {
    throw RuntimeError(__FILE__, __LINE__,
                       QString("Failed to create PnsSnapshot"));
  }
}

BoardPnsSnapshot::BoardPnsSnapshot(const Board& board)
  : mInnerLayerCount(board.getInnerLayerCount()),
    mCopperLayerCount(board.getInnerLayerCount() + 2),
    mHandle(construct(board.getInnerLayerCount() + 2)),
    mHostRefs{BoardPnsHostRef()},  // Host ID 0 is reserved as a null value.
    mHostIds(),
    mNetNumbers(),
    mNetSignals() {
  addRules(board);
  addNets(board);
  addTracesAndVias(board);
  addPads(board);
  addHoles(board);
  addPolygons(board);
  addZones(board);
}

BoardPnsSnapshot::~BoardPnsSnapshot() noexcept {
}

/*******************************************************************************
 *  Getters
 ******************************************************************************/

quint64 BoardPnsSnapshot::getHostId(const BI_NetLine& netLine) const noexcept {
  return mHostIds.value(&netLine, 0);
}

quint64 BoardPnsSnapshot::getHostId(const BI_Via& via) const noexcept {
  return mHostIds.value(&via, 0);
}

quint64 BoardPnsSnapshot::getHostId(const BI_Pad& pad) const noexcept {
  return mHostIds.value(&pad, 0);
}

quint64 BoardPnsSnapshot::getHostId(const BI_Hole& hole) const noexcept {
  return mHostIds.value(&hole, 0);
}

quint32 BoardPnsSnapshot::getNetNumber(const NetSignal* net) const noexcept {
  return net ? mNetNumbers.value(net, 0) : 0;
}

NetSignal* BoardPnsSnapshot::getNetSignal(quint32 netNumber) const noexcept {
  if ((netNumber == 0) ||
      (netNumber > static_cast<quint32>(mNetSignals.count()))) {
    return nullptr;
  }
  return mNetSignals.at(static_cast<int>(netNumber - 1));
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

rs::PnsSnapshot* BoardPnsSnapshot::release() noexcept {
  rs::PnsSnapshot* obj = mHandle.mObj;
  mHandle.mObj = nullptr;
  return obj;
}

/*******************************************************************************
 *  Static Methods
 ******************************************************************************/

int BoardPnsSnapshot::toDenseLayerIndex(const Layer& layer,
                                        int innerLayerCount) noexcept {
  if (!layer.isCopper()) {
    return -1;
  }
  // The bottom layer's sparse copper number is always
  // Layer::innerCopperCount() + 1, whatever the board's own stack is, so
  // it is moved down to the end of the dense range.
  const int index =
      layer.isBottom() ? (innerLayerCount + 1) : layer.getCopperNumber();
  // An inner layer the board does not have is not a layer of this board,
  // and must not alias the bottom layer's index.
  if (index > (innerLayerCount + 1)) {
    return -1;
  }
  if ((index == (innerLayerCount + 1)) && (!layer.isBottom())) {
    return -1;
  }
  // Layer mapping bugs are silent and catastrophic, so the round trip is
  // asserted on every call in debug builds.
  Q_ASSERT(fromDenseLayerIndex(index, innerLayerCount) == &layer);
  return index;
}

const Layer* BoardPnsSnapshot::fromDenseLayerIndex(
    int index, int innerLayerCount) noexcept {
  if ((index < 0) || (index > (innerLayerCount + 1))) {
    return nullptr;
  }
  return Layer::copper(
      index == (innerLayerCount + 1) ? (Layer::innerCopperCount() + 1) : index);
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void BoardPnsSnapshot::addRules(const Board& board) {
  const BoardDesignRuleCheckSettings& drc = board.getDrcSettings();
  const BoardDesignRules& rules = board.getDesignRules();

  const rs::PnsBoardRules ffiRules{
      (*drc.getMinCopperCopperClearance()).toNm(),
      (*drc.getMinCopperBoardClearance()).toNm(),
      (*drc.getMinCopperNpthClearance()).toNm(),
      (*drc.getMinDrillDrillClearance()).toNm(),
      (*drc.getMinDrillBoardClearance()).toNm(),
      (*drc.getMinCopperWidth()).toNm(),
      (*drc.getMinPthDrillDiameter()).toNm(),
      (*rules.getDefaultTraceWidth()).toNm(),
      (*rules.getDefaultViaDrillDiameter()).toNm(),
  };
  check(static_cast<int>(
            rs::ffi_pnsrouter_snapshot_set_board_rules(*mHandle, &ffiRules)),
        "board design rules");
}

void BoardPnsSnapshot::addNets(const Board& board) {
  const Circuit& circuit = board.getProject().getCircuit();

  // Every net class of the circuit is taken, not only the ones in use, so
  // that max_clearance stays valid when a net class is assigned later.
  QHash<const NetClass*, std::size_t> netClassIndex;
  foreach (const NetClass* netClass, circuit.getNetClasses()) {
    const rs::PnsNetClassRules ffiRules{
        (*netClass->getMinCopperCopperClearance()).toNm(),
        (*netClass->getMinCopperWidth()).toNm(),
        (*netClass->getMinViaDrillDiameter()).toNm(),
        netClass->getDefaultTraceWidth()
            ? (**netClass->getDefaultTraceWidth()).toNm()
            : 0,
        netClass->getDefaultViaDrill()
            ? (**netClass->getDefaultViaDrill()).toNm()
            : 0,
    };
    netClassIndex.insert(
        netClass,
        rs::ffi_pnsrouter_snapshot_add_net_class(*mHandle, &ffiRules));
  }

  foreach (NetSignal* net, circuit.getNetSignals()) {
    const std::size_t index = netClassIndex.value(&net->getNetClass(), 0);
    const quint32 number = rs::ffi_pnsrouter_snapshot_add_net(*mHandle, index);
    mNetNumbers.insert(net, number);
    // The numbers are handed out densely from 1, so appending in the same
    // order makes the vector the reverse lookup.
    Q_ASSERT(number == static_cast<quint32>(mNetSignals.count() + 1));
    mNetSignals.append(net);
  }
}

void BoardPnsSnapshot::addTracesAndVias(const Board& board) {
  foreach (BI_NetSegment* segment, board.getNetSegments()) {
    const quint32 net = getNetNumber(segment->getNetSignal());

    foreach (BI_NetLine* netLine, segment->getNetLines()) {
      const int layer =
          toDenseLayerIndex(netLine->getLayer(), mInnerLayerCount);
      if (layer < 0) {
        continue;
      }

      BoardPnsHostRef ref;
      ref.netLine = netLine;
      const rs::PnsItemHeader header =
          makeHeader(addHostRef(ref), net, layer, layer);
      const rs::PnsSegmentGeometry geometry{
          toFfi(netLine->getP1().getPosition()),
          toFfi(netLine->getP2().getPosition()),
          (*netLine->getWidth()).toNm(),
      };
      check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_segment(
                *mHandle, &header, &geometry)),
            QString("trace %1").arg(netLine->getUuid().toStr()));
    }

    foreach (BI_Via* via, segment->getVias()) {
      const int start =
          toDenseLayerIndex(via->getVia().getStartLayer(), mInnerLayerCount);
      const int end =
          toDenseLayerIndex(via->getVia().getEndLayer(), mInnerLayerCount);
      if ((start < 0) || (end < 0)) {
        continue;
      }

      BoardPnsHostRef ref;
      ref.via = via;
      const rs::PnsItemHeader header = makeHeader(
          addHostRef(ref), net, std::min(start, end), std::max(start, end));

      rs::PnsViaType type = rs::PnsViaType::Through;
      if (via->getVia().isBuried()) {
        type = rs::PnsViaType::Buried;
      } else if (via->getVia().isBlind()) {
        type = rs::PnsViaType::Blind;
      }

      const rs::PnsViaGeometry geometry{
          toFfi(via->getPosition()),
          (*via->getActualSize()).toNm(),
          (*via->getActualDrillDiameter()).toNm(),
          type,
          net == 0,
      };
      check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_via(
                *mHandle, &header, &geometry)),
            QString("via %1").arg(via->getUuid().toStr()));
    }
  }
}

void BoardPnsSnapshot::addPads(const Board& board) {
  foreach (BI_NetSegment* segment, board.getNetSegments()) {
    foreach (BI_Pad* pad, segment->getPads()) {
      addPad(*pad, mInnerLayerCount);
    }
  }
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    foreach (BI_Pad* pad, device->getPads()) {
      addPad(*pad, mInnerLayerCount);
    }
  }
}

void BoardPnsSnapshot::addPad(BI_Pad& pad, int innerLayerCount) {
  if (mHostIds.contains(&pad)) {
    return;  // A pad reachable from both a net segment and a device.
  }

  const Transform transform(pad.getPosition(), pad.getRotation(),
                            pad.getMirrored());

  // Collect the layers that actually carry copper, in dense index order.
  QMap<int, QList<PadGeometry>> geometriesByLayer;
  for (auto it = pad.getGeometries().begin(); it != pad.getGeometries().end();
       ++it) {
    const int layer = toDenseLayerIndex(*it.key(), innerLayerCount);
    if ((layer >= 0) && (!it.value().isEmpty())) {
      geometriesByLayer.insert(layer, it.value());
    }
  }
  if (geometriesByLayer.isEmpty()) {
    return;
  }

  BoardPnsHostRef ref;
  ref.pad = &pad;
  const quint64 hostId = addHostRef(ref);
  const quint32 net = getNetNumber(pad.getNetSignal());
  const PadHoleList& holes = pad.getProperties().getHoles();

  // The drill rides on the solid of the pad's component side layer, or on
  // the lowest copper layer that carries any copper at all. Putting it on
  // every layer's solid would make the pad collide with itself under the
  // drill to drill clearance rule.
  const Layer& componentSideLayer =
      (pad.getComponentSide() == Pad::ComponentSide::Bottom)
      ? Layer::botCopper()
      : Layer::topCopper();
  const int componentSide =
      toDenseLayerIndex(componentSideLayer, innerLayerCount);
  const int holeLayer = geometriesByLayer.contains(componentSide)
      ? componentSide
      : geometriesByLayer.firstKey();

  for (auto it = geometriesByLayer.begin(); it != geometriesByLayer.end();
       ++it) {
    const int layer = it.key();

    // Every outline of every geometry becomes one solid. The engine takes
    // the convex hull of the pieces of one host object, so a compound pad
    // still walks around as one shape. A circle and a capsule are rotation
    // invariant, so they survive the transform as native shapes; anything
    // else is flattened into a polygon.
    QList<rs::PnsShape> natives;
    QList<Path> outlines;
    for (const PadGeometry& geometry : it.value()) {
      if ((geometry.getShape() == PadGeometry::Shape::RoundedRect) &&
          (geometry.getWidth() == geometry.getHeight()) &&
          ((*geometry.getCornerRadius()) * 2 == geometry.getWidth())) {
        natives.append(
            circleShape(transform.map(Point(0, 0)), geometry.getWidth()));
      } else if ((geometry.getShape() == PadGeometry::Shape::Stroke) &&
                 (geometry.getPath().getVertices().count() == 2) &&
                 (!geometry.getPath().isCurved())) {
        const QVector<Vertex>& v = geometry.getPath().getVertices();
        natives.append(segmentShape(transform.map(v.at(0).getPos()),
                                    transform.map(v.at(1).getPos()),
                                    geometry.getWidth()));
      } else {
        for (const Path& outline : geometry.toOutlines()) {
          outlines.append(transform.map(outline));
        }
      }
    }

    const int pieceCount = natives.count() + outlines.count();
    for (int i = 0; i < pieceCount; ++i) {
      const bool emitHole =
          (!holes.isEmpty()) && (layer == holeLayer) && (i == 0);
      rs::PnsItemHeader header = makeHeader(hostId, net, layer, layer);
      header.compound_primitive = (pieceCount > 1);
      header.copper_clearance =
          (*pad.getProperties().getCopperClearance()).toNm();
      if (emitHole) {
        // The drill is a first class obstacle on every copper layer, which
        // is what KiCad does too, so the solid that carries it is widened
        // to the whole stack. On a layer where the pad has no copper this
        // adds a phantom annular ring around the drill, which is over
        // conservative rather than unsound.
        header.layer_start = 0;
        header.layer_end = mCopperLayerCount - 1;
      }

      std::vector<rs::PnsPoint> buffer;
      rs::PnsSolidGeometry geometry = {};
      if (i < natives.count()) {
        geometry.shape = natives.at(i);
      } else {
        geometry.shape = polygonShape(outlines.at(i - natives.count()), buffer);
        if (geometry.shape.vertex_count < 3) {
          continue;  // A degenerate outline is no obstacle.
        }
      }
      geometry.pos = toFfi(pad.getPosition());
      geometry.has_hole = emitHole;
      if (emitHole) {
        const PadHole& hole = *holes.first();
        geometry.hole =
            drillShape(transform.map(*hole.getPath()), *hole.getDiameter());
      }
      check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_solid(
                *mHandle, &header, &geometry)),
            QString("pad %1").arg(pad.getUuid().toStr()));
    }
  }

  // A pad with more than one drill gets the remaining ones as bare holes,
  // because one solid can carry only one.
  for (int i = 1; i < holes.count(); ++i) {
    const PadHole& hole = *holes.value(i);
    rs::PnsItemHeader header =
        makeHeader(hostId, net, 0, mCopperLayerCount - 1);
    header.routable = false;
    const rs::PnsHoleGeometry geometry{
        drillShape(transform.map(*hole.getPath()), *hole.getDiameter()),
    };
    check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_hole(
              *mHandle, &header, &geometry)),
          QString("pad %1").arg(pad.getUuid().toStr()));
  }
}

void BoardPnsSnapshot::addHoles(const Board& board) {
  foreach (BI_Hole* hole, board.getHoles()) {
    BoardPnsHostRef ref;
    ref.hole = hole;
    rs::PnsItemHeader header =
        makeHeader(addHostRef(ref), 0, 0, mCopperLayerCount - 1);
    header.routable = false;

    const rs::PnsHoleGeometry geometry{
        drillShape(*hole->getData().getPath(), *hole->getData().getDiameter()),
    };
    check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_hole(
              *mHandle, &header, &geometry)),
          QString("hole %1").arg(hole->getData().getUuid().toStr()));
  }
}

void BoardPnsSnapshot::addPolygons(const Board& board) {
  foreach (BI_Polygon* polygon, board.getPolygons()) {
    const Layer& layer = polygon->getData().getLayer();
    const bool boardEdge = layer.isBoardEdge();
    const int denseLayer = toDenseLayerIndex(layer, mInnerLayerCount);
    if ((!boardEdge) && (denseLayer < 0)) {
      continue;
    }

    BoardPnsHostRef ref;
    ref.polygon = polygon;
    const quint64 hostId = addHostRef(ref);

    if (boardEdge) {
      addBoardEdge(*polygon, hostId);
      continue;
    }

    rs::PnsItemHeader header = makeHeader(hostId, 0, denseLayer, denseLayer);

    std::vector<rs::PnsPoint> buffer;
    rs::PnsSolidGeometry geometry = {};
    geometry.shape = polygonShape(polygon->getData().getPath(), buffer);
    if (geometry.shape.vertex_count < 3) {
      continue;  // A degenerate outline is no obstacle.
    }
    geometry.pos = buffer.front();
    geometry.has_hole = false;
    check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_solid(
              *mHandle, &header, &geometry)),
          QString("polygon %1").arg(polygon->getData().getUuid().toStr()));
  }
}

void BoardPnsSnapshot::addBoardEdge(BI_Polygon& polygon, quint64 id) {
  const Path path = polygon.getData()
                        .getPath()
                        .flattenedArcs(maxArcTolerance())
                        .toClosedPath();
  const QVector<Vertex>& vertices = path.getVertices();

  for (int i = 1; i < vertices.count(); ++i) {
    const Point& p1 = vertices.at(i - 1).getPos();
    const Point& p2 = vertices.at(i).getPos();
    if (p1 == p2) {
      continue;  // A repeated vertex is no edge.
    }

    // A board edge spans every copper layer, carries no width and is not
    // routable: its clearance comes from the copper to board rule and not
    // from its shape.
    rs::PnsItemHeader header = makeHeader(id, 0, 0, mCopperLayerCount - 1);
    header.routable = false;
    header.board_edge = true;
    header.compound_primitive = true;

    rs::PnsSolidGeometry geometry = {};
    geometry.shape = segmentShape(p1, p2, Length(0));
    geometry.pos = toFfi(p1);
    geometry.has_hole = false;
    check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_solid(
              *mHandle, &header, &geometry)),
          QString("board edge %1").arg(polygon.getData().getUuid().toStr()));
  }
}

void BoardPnsSnapshot::addZones(const Board& board) {
  foreach (BI_Zone* zone, board.getZones()) {
    addZone(*zone);
  }
}

void BoardPnsSnapshot::addZone(BI_Zone& zone) {
  const BoardZoneData& data = zone.getData();

  // Only the no copper rule keeps the router out. The other three are about
  // planes, stop mask and devices, none of which the router places, and the
  // design rule check reads the no copper rule the same way: it reports any
  // copper whose area intersects the zone, at no clearance at all.
  if (!data.getRules().testFlag(Zone::Rule::NoCopper)) {
    return;
  }

  // The board zone already carries board layers, so no layer flag has to be
  // resolved here. A layer the board does not have is not a layer of this
  // zone either.
  QVector<int> layers;
  foreach (const Layer* layer, data.getLayers()) {
    const int denseLayer = toDenseLayerIndex(*layer, mInnerLayerCount);
    if (denseLayer >= 0) {
      layers.append(denseLayer);
    }
  }
  if (layers.isEmpty()) {
    return;
  }
  // A QSet iterates in an unspecified order, and the snapshot must not
  // depend on the memory addresses the layers happen to have.
  std::sort(layers.begin(), layers.end());

  const QVector<Path> triangles = triangulate(data.getOutline());
  if (triangles.isEmpty()) {
    qWarning() << "The router ignores zone" << data.getUuid().toStr()
               << "because its outline could not be cut into triangles. "
                  "Check whether it intersects itself.";
    return;
  }

  BoardPnsHostRef ref;
  ref.zone = &zone;
  const quint64 hostId = addHostRef(ref);

  foreach (int layer, layers) {
    foreach (const Path& triangle, triangles) {
      // A keepout carries no net, is never routed on and never moved, and
      // all its triangles are one host object, so a violation on one of
      // them is reported on the whole zone.
      rs::PnsItemHeader header = makeHeader(hostId, 0, layer, layer);
      header.routable = false;
      header.compound_primitive = true;
      header.keepout = true;

      std::vector<rs::PnsPoint> buffer;
      rs::PnsSolidGeometry geometry = {};
      geometry.shape = polygonShape(triangle, buffer);
      if (geometry.shape.vertex_count < 3) {
        continue;  // A degenerate triangle is no obstacle.
      }
      geometry.pos = buffer.front();
      geometry.has_hole = false;
      check(static_cast<int>(rs::ffi_pnsrouter_snapshot_add_solid(
                *mHandle, &header, &geometry)),
            QString("zone %1").arg(data.getUuid().toStr()));
    }
  }
}

quint64 BoardPnsSnapshot::addHostRef(const BoardPnsHostRef& ref) noexcept {
  const quint64 id = static_cast<quint64>(mHostRefs.count());
  mHostRefs.append(ref);
  const void* obj = ref.netLine ? static_cast<const void*>(ref.netLine)
      : ref.via                 ? static_cast<const void*>(ref.via)
      : ref.pad                 ? static_cast<const void*>(ref.pad)
      : ref.hole                ? static_cast<const void*>(ref.hole)
      : ref.polygon             ? static_cast<const void*>(ref.polygon)
                                : static_cast<const void*>(ref.zone);
  if (obj) {
    mHostIds.insert(obj, id);
  }
  return id;
}

void BoardPnsSnapshot::check(int result, const QString& item) {
  if (result == static_cast<int>(rs::PnsResult::Ok)) {
    return;
  }
  if (result == static_cast<int>(rs::PnsResult::CoordinateOutOfRange)) {
    throw RuntimeError(
        __FILE__, __LINE__,
        QString("The router does not support coordinates outside +/- 2 "
                "metres, but %1 has one.")
            .arg(item));
  }
  throw RuntimeError(
      __FILE__, __LINE__,
      QString("Failed to add %1 to the router snapshot (error %2).")
          .arg(item)
          .arg(result));
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb
