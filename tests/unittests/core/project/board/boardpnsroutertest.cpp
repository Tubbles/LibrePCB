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
#include <gtest/gtest.h>
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/geometry/path.h>
#include <librepcb/core/geometry/zone.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/boardpnsrouter.h>
#include <librepcb/core/project/board/boardzonedata.h>
#include <librepcb/core/project/board/drc/boarddesignrulechecksettings.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_hole.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_zone.h>
#include <librepcb/core/project/circuit/circuit.h>
#include <librepcb/core/project/circuit/netsignal.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/types/uuid.h>

#include <QtCore>

#include <algorithm>
#include <memory>
#include <optional>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace tests {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static std::unique_ptr<Project> openGerberTestProject() {
  const FilePath fp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
  std::shared_ptr<TransactionalFileSystem> fs =
      TransactionalFileSystem::openRO(fp.getParentDir());
  ProjectLoader loader;
  return loader.open(std::make_unique<TransactionalDirectory>(fs),
                     fp.getFilename());  // can throw
}

static BoardPnsRouter::Settings makeSettings(
    BoardPnsRouter::Mode mode = BoardPnsRouter::Mode::Walkaround,
    bool allowDrcViolations = false) noexcept {
  BoardPnsRouter::Settings settings{
      mode,
      PositiveLength(Length(250000)),  // 0.25 mm trace, above the minimum.
      PositiveLength(Length(700000)),  // 0.7 mm via.
      PositiveLength(Length(300000)),  // 0.3 mm via drill.
  };
  settings.allowDrcViolations = allowDrcViolations;
  return settings;
}

static bool isNull(const BoardPnsHostRef& ref) noexcept {
  return (!ref.netLine) && (!ref.via) && (!ref.pad) && (!ref.hole) &&
      (!ref.polygon) && (!ref.zone);
}

/**
 * @brief Where a route is started from
 */
struct RouteStart {
  const BI_Pad* pad = nullptr;
  quint64 hostId = 0;
  Point pos;
};

/**
 * @brief Find a footprint pad on the top copper layer to route away from
 *
 * The first pad, in the board's own deterministic order, that carries a net,
 * has copper on the top layer and that the router accepts as a start point.
 */
static std::optional<RouteStart> findStartPad(const BoardPnsRouter& router,
                                              const Board& board) {
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    foreach (const BI_Pad* pad, device->getPads()) {
      if (!pad->getNetSignal()) {
        continue;
      }
      if (pad->getGeometries().value(&Layer::topCopper()).isEmpty()) {
        continue;
      }
      const quint64 hostId = router.getSnapshot().getHostId(*pad);
      if (hostId == 0) {
        continue;
      }
      if (router.isStartingPointRoutable(pad->getPosition(), hostId,
                                         Layer::topCopper()) !=
          BoardPnsRouter::StartResult::Ok) {
        continue;
      }
      return RouteStart{pad, hostId, pad->getPosition()};
    }
  }
  return std::nullopt;
}

/**
 * @brief Points a few millimetres away from the start, in eight directions
 */
static QVector<Point> freeSpaceCandidates(const Point& start) noexcept {
  static const int directions[8][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1},
                                       {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  QVector<Point> candidates;
  for (qint64 distance : {5000000LL, 3000000LL}) {  // 5 mm, then 3 mm.
    for (const auto& direction : directions) {
      candidates.append(start +
                        Point(Length(distance * direction[0]),
                              Length(distance * direction[1])));
    }
  }
  return candidates;
}

/**
 * @brief Find a point near the start that the router routes to and commits
 *
 * Which directions out of a pad are free depends on the fixture's geometry,
 * which is exactly what the tests must not depend on, so the first direction
 * that works is picked here and the tests assert on whichever one that is.
 * Each attempt gets its own session because a successful attempt commits.
 */
static std::optional<Point> findFreeTarget(const Board& board,
                                           const RouteStart& start) {
  foreach (const Point& target, freeSpaceCandidates(start.pos)) {
    BoardPnsRouter probe(board, makeSettings());
    if (probe.startRouting(start.pos, start.hostId, Layer::topCopper()) !=
        BoardPnsRouter::StartResult::Ok) {
      continue;
    }
    probe.moveTo(target, 0);
    if (probe.fixRoute(target, 0, true) !=
        BoardPnsRouter::FixOutcome::Finished) {
      continue;
    }
    if (probe.getCommit().added.isEmpty()) {
      continue;
    }
    return target;
  }
  return std::nullopt;
}

/**
 * @brief A net line to drag, and where to drag it to
 */
struct DragCase {
  const BI_NetLine* netLine = nullptr;
  quint64 hostId = 0;

  /// The middle of the net line, which is what makes it a segment drag
  /// rather than a corner drag.
  Point pos;

  /// One millimetre away from there.
  Point target;
};

static Point middleOf(const BI_NetLine& netLine) noexcept {
  const Point p1 = netLine.getP1().getPosition();
  const Point p2 = netLine.getP2().getPosition();
  return Point(Length((p1.getX().toNm() + p2.getX().toNm()) / 2),
               Length((p1.getY().toNm() + p2.getY().toNm()) / 2));
}

static bool holdsNetLine(const QVector<BoardPnsHostRef>& refs,
                         const BI_NetLine& netLine) noexcept {
  foreach (const BoardPnsHostRef& ref, refs) {
    if (ref.netLine == &netLine) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Check whether a commit takes a net line off the board
 *
 * A dragged trace comes back as an update rather than as a removal when the
 * router could pair it with one of the segments it replaced it with, which
 * is how the board object keeps its identity across the drag. Both mean the
 * trace as it was is gone, and the commit applier treats a net line update
 * as a removal plus an addition either way.
 */
static bool dropsNetLine(const BoardPnsCommit& commit,
                         const BI_NetLine& netLine) noexcept {
  if (holdsNetLine(commit.removed, netLine)) {
    return true;
  }
  for (const auto& pair : commit.updated) {
    if (pair.first.netLine == &netLine) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Every net line of the board, each with the four directions to try
 *
 * Which net line can be moved where depends on the fixture's geometry, which
 * is exactly what the tests must not depend on, so every combination is
 * offered and the first one the router accepts is picked.
 */
static QVector<DragCase> dragCandidates(const BoardPnsRouter& router,
                                        const Board& board) {
  static const int directions[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
  QVector<DragCase> candidates;
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      const quint64 hostId = router.getSnapshot().getHostId(*netLine);
      if (hostId == 0) {
        continue;
      }
      const Point middle = middleOf(*netLine);
      for (const auto& direction : directions) {
        const Point offset(Length(1000000LL * direction[0]),
                           Length(1000000LL * direction[1]));  // 1 mm.
        candidates.append(DragCase{netLine, hostId, middle, middle + offset});
      }
    }
  }
  return candidates;
}

/**
 * @brief Find a net line the router drags one millimetre aside and commits
 *
 * Each attempt gets its own session because a successful one commits.
 */
static std::optional<DragCase> findDragCase(const Board& board) {
  BoardPnsRouter probeRouter(board, makeSettings());
  foreach (const DragCase& candidate, dragCandidates(probeRouter, board)) {
    BoardPnsRouter probe(board, makeSettings());
    if (probe.startDragging(candidate.pos, candidate.hostId, false) !=
        BoardPnsRouter::StartResult::Ok) {
      continue;
    }
    probe.moveTo(candidate.target, 0);
    if (probe.fixRoute(candidate.target, 0, true) !=
        BoardPnsRouter::FixOutcome::Finished) {
      continue;
    }
    if (probe.getCommit().added.isEmpty() ||
        (!dropsNetLine(probe.getCommit(), *candidate.netLine))) {
      continue;
    }
    return candidate;
  }
  return std::nullopt;
}

/**
 * @brief Every pad of a device the snapshot gave a host ID to
 *
 * A pad with no copper on any copper layer is not synced, so it is not part
 * of a footprint drag and must not make one look incomplete either.
 */
static QVector<quint64> syncedPadsOf(const BoardPnsRouter& router,
                                     const BI_Device& device) noexcept {
  QVector<quint64> hostIds;
  foreach (const BI_Pad* pad, device.getPads()) {
    if (const quint64 hostId = router.getSnapshot().getHostId(*pad)) {
      hostIds.append(hostId);
    }
  }
  return hostIds;
}

/**
 * @brief A device to drag by its pads, and where to drag it to
 */
struct FootprintDragCase {
  const BI_Device* device = nullptr;

  /// Every pad of the device the router knows, which is what makes the
  /// drag a footprint drag rather than a refused half of one.
  QVector<quint64> pads;

  /// Where the drag starts, which is the position of one of the pads.
  Point pos;

  /// One millimetre away from there.
  Point target;
};

/**
 * @brief Find a device the router moves one millimetre and commits
 *
 * Which device that is depends on the fixture's geometry, which is exactly
 * what the tests must not depend on, so every device and four directions are
 * offered and the first combination that commits is picked. Each attempt
 * gets its own session because a successful one commits.
 */
static std::optional<FootprintDragCase> findFootprintDragCase(
    const Board& board) {
  static const int directions[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
  BoardPnsRouter probeRouter(board, makeSettings());
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    const QVector<quint64> pads = syncedPadsOf(probeRouter, *device);
    if (pads.isEmpty()) {
      continue;
    }
    const BoardPnsHostRef ref = probeRouter.getHostRef(pads.first());
    if (!ref.pad) {
      continue;
    }
    const Point pos = ref.pad->getPosition();
    for (const auto& direction : directions) {
      const Point target =
          pos + Point(Length(1000000LL * direction[0]),
                      Length(1000000LL * direction[1]));  // 1 mm.
      BoardPnsRouter probe(board, makeSettings());
      if (probe.startDragging(pos, pads, false) !=
          BoardPnsRouter::StartResult::Ok) {
        continue;
      }
      probe.moveTo(target, 0);
      if (probe.fixRoute(target, 0, true) !=
          BoardPnsRouter::FixOutcome::Finished) {
        continue;
      }
      if (probe.getCommit().movedDevices.isEmpty()) {
        continue;
      }
      return FootprintDragCase{device, pads, pos, target};
    }
  }
  return std::nullopt;
}

/**
 * @brief Two net lines to drag together, and where to drag them to
 */
struct MultiDragCase {
  const BI_NetLine* primary = nullptr;
  const BI_NetLine* other = nullptr;

  /// The host IDs of both, which is what a multi drag is: a set the router
  /// finds more than one track in.
  QVector<quint64> hostIds;

  /// The middle of the primary net line, which is where the cursor is.
  Point pos;

  /// One millimetre away from there.
  Point target;
};

/**
 * @brief Find two net lines the router drags together and commits
 *
 * The router's multi drag moves the line the cursor is on and takes the
 * others along, and it leaves behind whatever does not run parallel to the
 * primary, so which pair works depends on the fixture's geometry. Every
 * ordered pair on one layer and four directions are offered and the first
 * combination that drops both is picked.
 *
 * The pair this fixture answers with is two pieces of one chain, because no
 * two of its traces in different net segments run parallel; the multi
 * dragger's own geometry is the router crate's business anyway, and what is
 * checked here is the host side of it: a set of several host IDs crosses the
 * wrapper and the FFI, the router takes its multi drag path, and everything
 * the set named comes back replaced.
 */
static std::optional<MultiDragCase> findMultiDragCase(const Board& board) {
  static const int directions[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
  BoardPnsRouter probeRouter(board, makeSettings());

  QVector<const BI_NetLine*> netLines;
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      if (probeRouter.getSnapshot().getHostId(*netLine) != 0) {
        netLines.append(netLine);
      }
    }
  }

  foreach (const BI_NetLine* primary, netLines) {
    const Point pos = middleOf(*primary);
    const quint64 primaryId = probeRouter.getSnapshot().getHostId(*primary);
    foreach (const BI_NetLine* other, netLines) {
      if ((other == primary) || (&other->getLayer() != &primary->getLayer())) {
        continue;
      }
      const QVector<quint64> hostIds{
          primaryId, probeRouter.getSnapshot().getHostId(*other)};
      for (const auto& direction : directions) {
        const Point target =
            pos + Point(Length(1000000LL * direction[0]),
                        Length(1000000LL * direction[1]));  // 1 mm.
        BoardPnsRouter probe(board, makeSettings());
        if (probe.startDragging(pos, hostIds, false) !=
            BoardPnsRouter::StartResult::Ok) {
          continue;
        }
        probe.moveTo(target, 0);
        if (probe.fixRoute(target, 0, true) !=
            BoardPnsRouter::FixOutcome::Finished) {
          continue;
        }
        if ((!dropsNetLine(probe.getCommit(), *primary)) ||
            (!dropsNetLine(probe.getCommit(), *other))) {
          continue;
        }
        return MultiDragCase{primary, other, hostIds, pos, target};
      }
    }
  }
  return std::nullopt;
}

/**
 * @brief Add a square keepout zone on the top layer to a board
 *
 * The board takes ownership: ::librepcb::Board::~Board() deletes every zone
 * it holds, so the caller must not.
 */
static const BI_Zone* addKeepoutZone(Board& board, const Point& center,
                                     const Length& size) {
  const Point half(Length(size.toNm() / 2), Length(size.toNm() / 2));
  BI_Zone* zone = new BI_Zone(
      board,
      BoardZoneData(Uuid::createRandom(), {&Layer::topCopper()},
                    Zone::Rules(Zone::Rule::NoCopper),
                    Path::rect(center - half, center + half), false));
  board.addZone(*zone);
  return zone;
}

/**
 * @brief Whether the copper of one straight trace reaches into a zone
 *
 * The design rule check's own test, which intersects the two areas and
 * applies no clearance at all (`BoardDesignRuleCheck::checkZones`). The trace
 * is narrowed by two micrometres first because a keepout is an exact
 * boundary: the router is entitled to place copper right up against the zone,
 * and Qt reads two areas which only touch as intersecting.
 */
static bool isInZone(const Point& p1, const Point& p2,
                     const PositiveLength& width,
                     const Path& outline) noexcept {
  const Length narrowed = (*width) - Length(2000);
  if ((p1 == p2) || (narrowed <= 0)) {
    return false;
  }
  const Path area = Path::obround(p1, p2, PositiveLength(narrowed));
  return outline.toQPainterPathPx().intersects(area.toQPainterPathPx());
}

static bool isCopperInZone(const BoardPnsNewItem& item,
                           const Path& outline) noexcept {
  const BoardPnsNewSegment* segment = item.getSegment();
  return segment &&
      isInZone(segment->start, segment->end, segment->width, outline);
}

/**
 * @brief How many pieces of one preview polyline reach into a zone
 */
static int piecesInZone(const BoardPnsPreviewItem& item,
                        const Path& outline) noexcept {
  int count = 0;
  for (int i = 1; i < item.path.count(); ++i) {
    if (isInZone(item.path.at(i - 1), item.path.at(i), item.width, outline)) {
      ++count;
    }
  }
  return count;
}

/**
 * @brief Find a point on the fixture's own copper a route cannot be fixed on
 *
 * The position of a top layer pad of another net, which is copper the route
 * has to end inside of. Which pad that is depends on the fixture's geometry,
 * which is exactly what the tests must not depend on, so every candidate is
 * offered and the first one whose route mark obstacles mode refuses to fix is
 * picked. Each attempt gets its own session because a fix ends one.
 */
static std::optional<Point> findCollidingTarget(const Board& board,
                                                const RouteStart& start) {
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    foreach (const BI_Pad* pad, device->getPads()) {
      if (pad->getNetSignal() == start.pad->getNetSignal()) {
        continue;  // Same net, so its copper is no obstacle to this route.
      }
      if (pad->getGeometries().value(&Layer::topCopper()).isEmpty()) {
        continue;
      }
      BoardPnsRouter probe(board,
                           makeSettings(BoardPnsRouter::Mode::MarkObstacles));
      if (probe.startRouting(start.pos, start.hostId, Layer::topCopper()) !=
          BoardPnsRouter::StartResult::Ok) {
        continue;
      }
      probe.moveTo(pad->getPosition(), 0);
      if (probe.getPreview().violations.isEmpty()) {
        continue;  // Nothing in the way, so there is nothing to allow.
      }
      if (probe.fixRoute(pad->getPosition(), 0, true) !=
          BoardPnsRouter::FixOutcome::Continue) {
        continue;
      }
      return pad->getPosition();
    }
  }
  return std::nullopt;
}

/**
 * @brief Route once from a start to a target and hand back what it committed
 */
static QVector<BoardPnsNewItem> routeOnce(const Board& board,
                                          const RouteStart& start,
                                          const Point& target,
                                          BoardPnsRouter::Mode mode) {
  BoardPnsRouter router(board, makeSettings(mode));
  if (router.startRouting(start.pos, start.hostId, Layer::topCopper()) !=
      BoardPnsRouter::StartResult::Ok) {
    return QVector<BoardPnsNewItem>();
  }
  router.moveTo(target, 0);
  if (router.fixRoute(target, 0, true) !=
      BoardPnsRouter::FixOutcome::Finished) {
    return QVector<BoardPnsNewItem>();
  }
  return router.getCommit().added;
}

/**
 * @brief The middle of the longest piece of the head a preview holds
 *
 * Where a zone has to sit to be in the way of a route, whatever the fixture's
 * geometry made the router do.
 */
static std::optional<Point> longestHeadPieceMiddle(
    const BoardPnsPreview& preview) noexcept {
  std::optional<Point> middle;
  Length longest(0);
  foreach (const BoardPnsPreviewItem& item, preview.items) {
    if (item.style != BoardPnsPreviewStyle::Head) {
      continue;
    }
    for (int i = 1; i < item.path.count(); ++i) {
      const Point& p1 = item.path.at(i - 1);
      const Point& p2 = item.path.at(i);
      const Length length = *(p2 - p1).getLength();
      if (length > longest) {
        longest = length;
        middle = Point(Length((p1.getX().toNm() + p2.getX().toNm()) / 2),
                       Length((p1.getY().toNm() + p2.getY().toNm()) / 2));
      }
    }
  }
  return middle;
}

/**
 * @brief The middle of the longest segment of a commit
 *
 * The same thing for a session which has already committed.
 */
static std::optional<Point> longestSegmentMiddle(
    const QVector<BoardPnsNewItem>& items) noexcept {
  std::optional<Point> middle;
  Length longest(0);
  foreach (const BoardPnsNewItem& item, items) {
    const BoardPnsNewSegment* segment = item.getSegment();
    if (!segment) {
      continue;
    }
    const Point& start = segment->start;
    const Point& end = segment->end;
    const Length length = *(end - start).getLength();
    if (length > longest) {
      longest = length;
      middle = Point(Length((start.getX().toNm() + end.getX().toNm()) / 2),
                     Length((start.getY().toNm() + end.getY().toNm()) / 2));
    }
  }
  return middle;
}

/*******************************************************************************
 *  Differential pair helpers
 ******************************************************************************/

/**
 * @brief The settings a differential pair session runs with
 *
 * The pair gap is above the fixture's 0.2 mm minimum copper clearance,
 * which the router's own default of 0.18 mm is not: a gap below that
 * clearance is refused before anything else is even looked at.
 */
static BoardPnsRouter::Settings makeDiffPairSettings() noexcept {
  BoardPnsRouter::Settings settings = makeSettings();
  settings.diffPairWidth = PositiveLength(Length(250000));  // 0.25 mm.
  settings.diffPairGap = PositiveLength(Length(300000));  // 0.3 mm.
  return settings;
}

/**
 * @brief One pad a differential pair could be started from
 */
struct DiffPairPad {
  NetSignal* net = nullptr;
  quint64 hostId = 0;
  Point pos;
};

/**
 * @brief A differential pair made out of the fixture, and a route on it
 *
 * The two net signals are already renamed when this is answered, so the
 * board really does carry a pair and a new session over it finds one too.
 */
struct DiffPairCase {
  NetSignal* positive = nullptr;
  NetSignal* negative = nullptr;

  /// A pad of the positive net, which is what the route starts on.
  quint64 hostId = 0;
  Point pos;

  /// A point in free space the pair reaches and commits on.
  Point target;
};

/**
 * @brief Every pad with a net and copper on the top layer
 */
static QVector<DiffPairPad> diffPairPadCandidates(
    const BoardPnsRouter& router, const Board& board) noexcept {
  QVector<DiffPairPad> pads;
  foreach (const BI_Device* device, board.getDeviceInstances()) {
    foreach (BI_Pad* pad, device->getPads()) {
      NetSignal* net = pad->getNetSignal();
      if (!net) {
        continue;
      }
      if (pad->getGeometries().value(&Layer::topCopper()).isEmpty()) {
        continue;
      }
      const quint64 hostId = router.getSnapshot().getHostId(*pad);
      if (hostId == 0) {
        continue;
      }
      pads.append(DiffPairPad{net, hostId, pad->getPosition()});
    }
  }
  return pads;
}

/**
 * @brief Check whether a commit put segments on both nets of a pair
 */
static bool commitsBothNets(const BoardPnsCommit& commit,
                            const NetSignal* positive,
                            const NetSignal* negative) noexcept {
  bool onPositive = false;
  bool onNegative = false;
  foreach (const BoardPnsNewItem& item, commit.added) {
    if (!item.getSegment()) {
      continue;
    }
    onPositive = onPositive || (item.net == positive);
    onNegative = onNegative || (item.net == negative);
  }
  return onPositive && onNegative;
}

/**
 * @brief Try to route a pair out of two nets of the fixture
 *
 * Renames the two nets into `T_P` and `T_N`, which is all it takes to make
 * a pair, LibrePCB derives them from the names. On success the renaming
 * stays, so the caller's own session sees the same pair; on failure the
 * two names are put back.
 */
static std::optional<DiffPairCase> tryDiffPairCase(
    Circuit& circuit, Board& board, const QVector<DiffPairPad>& pads,
    NetSignal& positive, NetSignal& negative) {
  const CircuitIdentifier nameP = positive.getName();
  const CircuitIdentifier nameN = negative.getName();
  circuit.setNetSignalName(positive, CircuitIdentifier("T_P"), false);
  circuit.setNetSignalName(negative, CircuitIdentifier("T_N"), false);

  BoardPnsRouter probe(board, makeDiffPairSettings());
  foreach (const DiffPairPad& pad, pads) {
    if (pad.net != &positive) {
      continue;
    }
    if (probe.isStartingPointRoutableDiffPair(pad.pos, pad.hostId,
                                              Layer::topCopper()) !=
        BoardPnsRouter::StartResult::Ok) {
      continue;
    }
    // Which way out of the pads is free depends on the fixture's geometry,
    // which is exactly what the test must not depend on, so the first
    // direction that commits is taken. Each attempt gets its own session
    // because a successful one commits.
    foreach (const Point& target, freeSpaceCandidates(pad.pos)) {
      BoardPnsRouter attempt(board, makeDiffPairSettings());
      if (attempt.startRoutingDiffPair(pad.pos, pad.hostId,
                                       Layer::topCopper()) !=
          BoardPnsRouter::StartResult::Ok) {
        continue;
      }
      attempt.moveTo(target, 0);
      if (attempt.fixRoute(target, 0, true) !=
          BoardPnsRouter::FixOutcome::Finished) {
        continue;
      }
      if (!commitsBothNets(attempt.getCommit(), &positive, &negative)) {
        continue;
      }
      return DiffPairCase{&positive, &negative, pad.hostId, pad.pos, target};
    }
  }

  circuit.setNetSignalName(positive, nameP, false);
  circuit.setNetSignalName(negative, nameN, false);
  return std::nullopt;
}

/**
 * @brief Make a differential pair out of the fixture and route it once
 *
 * The Gerber Test project holds no pair, so one is made by renaming two of
 * its nets. Which two can actually be coupled depends on the board: the
 * router pairs the start object with the *nearest* matching object of the
 * partner net, and that object has to be a pad too, has to have a free end
 * and has to span the same layers. The net pairs are therefore tried
 * closest first, which finds one in a handful of attempts instead of
 * walking the whole product.
 */
static std::optional<DiffPairCase> findDiffPairCase(Project& project,
                                                    Board& board) {
  BoardPnsRouter probeRouter(board, makeDiffPairSettings());
  const QVector<DiffPairPad> pads = diffPairPadCandidates(probeRouter, board);

  // The closest pad of each ordered net pair, which is the pad the router
  // would couple with anyway.
  QMap<QPair<NetSignal*, NetSignal*>, qint64> distances;
  foreach (const DiffPairPad& a, pads) {
    foreach (const DiffPairPad& b, pads) {
      if (a.net == b.net) {
        continue;
      }
      const qint64 distance = (*(b.pos - a.pos).getLength()).toNm();
      const QPair<NetSignal*, NetSignal*> key(a.net, b.net);
      if ((!distances.contains(key)) || (distance < distances.value(key))) {
        distances.insert(key, distance);
      }
    }
  }

  // Closest first, and the net names break a tie so that a failure is
  // reproducible: the keys are pointers, whose order is not.
  QVector<QPair<NetSignal*, NetSignal*>> ordered = distances.keys().toVector();
  std::sort(ordered.begin(), ordered.end(),
            [&distances](const QPair<NetSignal*, NetSignal*>& a,
                         const QPair<NetSignal*, NetSignal*>& b) {
              if (distances.value(a) != distances.value(b)) {
                return distances.value(a) < distances.value(b);
              }
              if (a.first != b.first) {
                return *a.first->getName() < *b.first->getName();
              }
              return *a.second->getName() < *b.second->getName();
            });

  foreach (const auto& candidate, ordered) {
    if (auto found = tryDiffPairCase(project.getCircuit(), board, pads,
                                     *candidate.first, *candidate.second)) {
      return found;
    }
  }
  return std::nullopt;
}

/*******************************************************************************
 *  Length tuning helpers
 ******************************************************************************/

/**
 * @brief A trace to tune, and a target it really reaches
 */
struct TuningCase {
  const BI_NetLine* netLine = nullptr;
  quint64 hostId = 0;

  /// Where the session starts, which is one end of the trace.
  Point start;

  /// Where the cursor goes, which is the other end: everything between
  /// the two meanders.
  Point end;

  /// The length the tuned run has before any meander is placed.
  Length baseline;

  /// A target above #baseline which the router really reaches.
  Length target;
};

/// How far either side of a target still counts as tuned in these tests.
static const Length sTuningTolerance(100000);  // 0.1 mm, the router's own.

/**
 * @brief Measure the tuned run of a trace before any meander is placed
 *
 * The readout's delta is measured against exactly that length, so one
 * session with no target at all hands the baseline back as the difference
 * between the two numbers, and no target has to be guessed first.
 *
 * @return `std::nullopt` if the router refuses to tune this trace at all,
 *         or if it measured nothing, which is what an absent delta means.
 */
static std::optional<Length> tuningBaseline(const Board& board,
                                            quint64 hostId, const Point& start,
                                            const Point& end) {
  BoardPnsRouter probe(board, makeSettings());
  if (probe.startTuning(start, hostId, BoardPnsTuningMode::Single,
                        BoardPnsRouter::TuningSettings{}) !=
      BoardPnsRouter::StartResult::Ok) {
    return std::nullopt;
  }
  probe.moveTo(end, 0);

  const std::optional<BoardPnsTuningInfo>& tuning = probe.getPreview().tuning;
  if ((!tuning) || (!tuning->delta)) {
    return std::nullopt;
  }
  return tuning->result - *tuning->delta;
}

/**
 * @brief The total length of the traces a commit takes off the board
 */
static Length removedCopperLength(const BoardPnsCommit& commit) noexcept {
  Length total;
  foreach (const BoardPnsHostRef& ref, commit.removed) {
    if (ref.netLine) {
      total += *ref.netLine->getLength();
    }
  }
  for (const auto& pair : commit.updated) {
    if (pair.first.netLine) {
      total += *pair.first.netLine->getLength();
    }
  }
  return total;
}

/**
 * @brief The total length of the traces a commit puts on the board
 */
static Length addedCopperLength(const BoardPnsCommit& commit) noexcept {
  Length total;
  auto add = [&total](const BoardPnsNewItem& item) {
    if (const BoardPnsNewSegment* segment = item.getSegment()) {
      total += *(segment->end - segment->start).getLength();
    }
  };
  foreach (const BoardPnsNewItem& item, commit.added) {
    add(item);
  }
  for (const auto& pair : commit.updated) {
    add(pair.second);
  }
  return total;
}

/**
 * @brief Find a trace of the fixture the router lengthens to a target
 *
 * Which trace has room for meanders depends on the fixture's geometry,
 * which is exactly what the tests must not depend on, so every trace is
 * offered with a few targets above its own length and the first
 * combination the router reports as tuned is picked. The targets are
 * tried largest first, so that the case which is found leaves the most
 * copper to assert on.
 */
static std::optional<TuningCase> findTuningCase(const Board& board) {
  BoardPnsRouter probeRouter(board, makeSettings());
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      const quint64 hostId = probeRouter.getSnapshot().getHostId(*netLine);
      if (hostId == 0) {
        continue;
      }
      const Point start = netLine->getP1().getPosition();
      const Point end = netLine->getP2().getPosition();
      const std::optional<Length> baseline =
          tuningBaseline(board, hostId, start, end);
      if (!baseline) {
        continue;
      }

      for (qint64 extra : {2000000LL, 1000000LL, 500000LL}) {  // 2, 1, 0.5 mm.
        const Length target = *baseline + Length(extra);
        BoardPnsRouter::TuningSettings settings;
        settings.target = target;
        settings.tolerance = sTuningTolerance;

        BoardPnsRouter probe(board, makeSettings());
        if (probe.startTuning(start, hostId, BoardPnsTuningMode::Single,
                              settings) != BoardPnsRouter::StartResult::Ok) {
          continue;
        }
        probe.moveTo(end, 0);
        const std::optional<BoardPnsTuningInfo>& tuning =
            probe.getPreview().tuning;
        if ((!tuning) || (tuning->status != BoardPnsTuningStatus::Tuned)) {
          continue;
        }
        return TuningCase{netLine, hostId, start, end, *baseline, target};
      }
    }
  }
  return std::nullopt;
}

/**
 * @brief Find any trace of the fixture the router opens a session on
 *
 * For the tests which only need a running session, not a tuned one.
 */
static std::optional<TuningCase> findTunableTrace(const Board& board) {
  BoardPnsRouter probeRouter(board, makeSettings());
  foreach (const BI_NetSegment* segment, board.getNetSegments()) {
    foreach (const BI_NetLine* netLine, segment->getNetLines()) {
      const quint64 hostId = probeRouter.getSnapshot().getHostId(*netLine);
      if (hostId == 0) {
        continue;
      }
      const Point start = netLine->getP1().getPosition();
      const Point end = netLine->getP2().getPosition();
      const std::optional<Length> baseline =
          tuningBaseline(board, hostId, start, end);
      if (!baseline) {
        continue;
      }
      return TuningCase{netLine, hostId, start, end, *baseline, *baseline};
    }
  }
  return std::nullopt;
}

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class BoardPnsRouterTest : public ::testing::Test {
protected:
  std::unique_ptr<Project> mProject;
  Board* mBoard = nullptr;

  void SetUp() override {
    mProject = openGerberTestProject();
    ASSERT_FALSE(mProject->getBoards().isEmpty());
    mBoard = mProject->getBoards().first();
  }
};

/*******************************************************************************
 *  Step 4: driving a session
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testSessionOverBoard) {
  BoardPnsRouter router(*mBoard, makeSettings());
  EXPECT_EQ(router.getCopperLayerCount(), mBoard->getInnerLayerCount() + 2);
  EXPECT_FALSE(router.isRoutingInProgress());
  EXPECT_EQ(router.getCurrentLayer(), nullptr);
  EXPECT_FALSE(router.isPlacingVia());
  EXPECT_TRUE(router.getPreview().items.isEmpty());
  EXPECT_TRUE(router.getCommit().added.isEmpty());
  EXPECT_GT(router.getHostRefs().count(), 1);
  EXPECT_TRUE(isNull(router.getHostRef(0)));
}

TEST_F(BoardPnsRouterTest, testRouteFromPadIntoFreeSpace) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isRoutingInProgress());
  EXPECT_EQ(router.getCurrentLayer(), &Layer::topCopper());

  router.moveTo(*target, 0);
  int headItems = 0;
  foreach (const BoardPnsPreviewItem& item, router.getPreview().items) {
    if (item.style == BoardPnsPreviewStyle::Head) {
      ++headItems;
      EXPECT_EQ(item.layer, &Layer::topCopper());
      EXPECT_GE(item.path.count(), 2);
    }
  }
  EXPECT_GT(headItems, 0);

  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isRoutingInProgress());

  // The commit is the whole point: the route becomes traces on the layer it
  // was placed on, on the pad's net, at the width the session was given.
  const BoardPnsCommit commit = router.getCommit();
  EXPECT_TRUE(commit.removed.isEmpty());
  int segments = 0;
  foreach (const BoardPnsNewItem& item, commit.added) {
    const BoardPnsNewSegment* segment = item.getSegment();
    ASSERT_NE(segment, nullptr);
    ++segments;
    EXPECT_EQ(segment->layer, &Layer::topCopper());
    EXPECT_EQ(item.net, start->pad->getNetSignal());
    EXPECT_EQ((*segment->width).toNm(), 250000);
    EXPECT_TRUE(isNull(item.source));
  }
  EXPECT_GT(segments, 0);

  // A committed session has nothing left to draw.
  EXPECT_TRUE(router.getPreview().items.isEmpty());
  EXPECT_FALSE(router.getPreview().via.has_value());
}

TEST_F(BoardPnsRouterTest, testAbortRoutingCommitsNothing) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  EXPECT_TRUE(router.isRoutingInProgress());

  router.abortRouting();
  EXPECT_FALSE(router.isRoutingInProgress());
  EXPECT_TRUE(router.getPreview().items.isEmpty());
  EXPECT_TRUE(router.getCommit().added.isEmpty());
  EXPECT_TRUE(router.getCommit().removed.isEmpty());
  EXPECT_TRUE(router.getCommit().updated.isEmpty());

  // Stopping an aborted session must not resurrect what it threw away.
  const BoardPnsCommit commit = router.stopRouting();
  EXPECT_TRUE(commit.added.isEmpty());
  EXPECT_TRUE(commit.removed.isEmpty());
  EXPECT_TRUE(commit.updated.isEmpty());
  EXPECT_FALSE(router.isRoutingInProgress());
}

TEST_F(BoardPnsRouterTest, testFixWithoutFinishThenUndo) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);

  // Without the force flag a fix in free space only pins one more corner.
  ASSERT_EQ(router.fixRoute(*target, 0, false),
            BoardPnsRouter::FixOutcome::Continue);
  EXPECT_TRUE(router.isRoutingInProgress());
  EXPECT_TRUE(router.getCommit().added.isEmpty());

  // A fix clears the head and restarts the placement where it ended, so
  // the router's hosts follow every fix with a move. Moving back towards
  // the pad stays on the net that was just routed, so nothing is in the
  // way wherever the fixture put the pad.
  const Point midway(
      Length((start->pos.getX().toNm() + target->getX().toNm()) / 2),
      Length((start->pos.getY().toNm() + target->getY().toNm()) / 2));
  router.moveTo(midway, 0);

  const std::optional<Point> undone = router.undoLastSegment();
  EXPECT_TRUE(undone.has_value());
  EXPECT_TRUE(router.isRoutingInProgress());

  router.abortRouting();
  EXPECT_FALSE(router.isRoutingInProgress());
  EXPECT_TRUE(router.getCommit().added.isEmpty());
}

TEST_F(BoardPnsRouterTest, testStartingPointRoutable) {
  BoardPnsRouter router(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(router, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  EXPECT_EQ(router.isStartingPointRoutable(start->pos, start->hostId,
                                           Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);

  // A board level drill is not copper, so no route may start on it.
  ASSERT_FALSE(mBoard->getHoles().isEmpty());
  int holesTested = 0;
  foreach (const BI_Hole* hole, mBoard->getHoles()) {
    const quint64 hostId = router.getSnapshot().getHostId(*hole);
    ASSERT_GT(hostId, 0U);
    EXPECT_EQ(router.getHostRef(hostId).hole, hole);

    const Point pos = hole->getData().getPath()->getVertices().first().getPos();
    EXPECT_NE(router.isStartingPointRoutable(pos, hostId, Layer::topCopper()),
              BoardPnsRouter::StartResult::Ok)
        << "hole " << hole->getData().getUuid().toStr().toStdString();
    ++holesTested;
  }
  EXPECT_GT(holesTested, 0);

  // An ID the snapshot never handed out is not a start point either.
  const quint64 unknown =
      static_cast<quint64>(router.getHostRefs().count()) + 100;
  EXPECT_EQ(
      router.isStartingPointRoutable(start->pos, unknown, Layer::topCopper()),
      BoardPnsRouter::StartResult::UnknownStartItem);
}

/*******************************************************************************
 *  Dragging
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testDragNetLineAside) {
  const std::optional<DragCase> drag = findDragCase(*mBoard);
  ASSERT_TRUE(drag.has_value()) << "no net line the router moves 1 mm aside";
  const Layer& layer = drag->netLine->getLayer();
  const NetSignal* net = drag->netLine->getNetSegment().getNetSignal();

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startDragging(drag->pos, drag->hostId, false),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isDragging());
  EXPECT_TRUE(router.isRoutingInProgress());

  // A drag which has not moved yet holds the untouched board, so the
  // geometry only appears once the cursor has gone somewhere.
  EXPECT_TRUE(router.getPreview().items.isEmpty());
  router.moveTo(drag->target, 0);
  EXPECT_FALSE(router.getPreview().items.isEmpty());

  ASSERT_EQ(router.fixRoute(drag->target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isDragging());
  EXPECT_FALSE(router.isRoutingInProgress());

  // The dragged trace is gone and what replaces it is on its layer and on
  // its net, which is what makes this a move rather than a new route.
  const BoardPnsCommit commit = router.getCommit();
  EXPECT_TRUE(dropsNetLine(commit, *drag->netLine));
  int segments = 0;
  foreach (const BoardPnsNewItem& item, commit.added) {
    const BoardPnsNewSegment* segment = item.getSegment();
    ASSERT_NE(segment, nullptr);
    ++segments;
    EXPECT_EQ(segment->layer, &layer);
    EXPECT_EQ(item.net, net);
  }
  for (const auto& pair : commit.updated) {
    const BoardPnsNewSegment* segment = pair.second.getSegment();
    ASSERT_NE(segment, nullptr);
    ++segments;
    EXPECT_EQ(segment->layer, &layer);
    EXPECT_EQ(pair.second.net, net);
  }
  EXPECT_GT(segments, 0);
}

TEST_F(BoardPnsRouterTest, testDragTwoNetLinesTogether) {
  const std::optional<MultiDragCase> drag = findMultiDragCase(*mBoard);
  ASSERT_TRUE(drag.has_value()) << "no pair of net lines the router drags";

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startDragging(drag->pos, drag->hostIds, false),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isDragging());

  router.moveTo(drag->target, 0);
  EXPECT_FALSE(router.getPreview().items.isEmpty());

  ASSERT_EQ(router.fixRoute(drag->target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);

  // Both net lines are replaced, which is what makes this one gesture
  // rather than two drags: the router moved the set it was given.
  const BoardPnsCommit commit = router.getCommit();
  EXPECT_TRUE(dropsNetLine(commit, *drag->primary));
  EXPECT_TRUE(dropsNetLine(commit, *drag->other));
  EXPECT_TRUE(commit.movedDevices.isEmpty());
  EXPECT_FALSE(commit.added.isEmpty() && commit.updated.isEmpty());
}

TEST_F(BoardPnsRouterTest, testDragDeviceByItsPads) {
  const std::optional<FootprintDragCase> drag = findFootprintDragCase(*mBoard);
  ASSERT_TRUE(drag.has_value()) << "no device the router moves 1 mm aside";
  const Point offset = drag->target - drag->pos;

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startDragging(drag->pos, drag->pads, false),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isDragging());
  EXPECT_TRUE(router.isRoutingInProgress());

  // The preview names the device rather than drawing its copper, because
  // the router has no geometry for a pad and the host owns it already.
  router.moveTo(drag->target, 0);
  ASSERT_EQ(router.getPreview().movedDevices.count(), 1);
  EXPECT_EQ(router.getPreview().movedDevices.first().device, drag->device);
  EXPECT_EQ(router.getPreview().movedDevices.first().offset, offset);

  ASSERT_EQ(router.fixRoute(drag->target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isDragging());

  // One entry per device and not one per pad, which is what the host has
  // to apply: the pads themselves are never removed or added.
  const BoardPnsCommit commit = router.getCommit();
  ASSERT_EQ(commit.movedDevices.count(), 1);
  EXPECT_EQ(commit.movedDevices.first().device, drag->device);
  EXPECT_EQ(commit.movedDevices.first().offset, offset);
  foreach (const BoardPnsHostRef& ref, commit.removed) {
    EXPECT_EQ(ref.pad, nullptr);
  }
  foreach (const BoardPnsNewItem& item, commit.added) {
    EXPECT_EQ(item.source.pad, nullptr);
  }
}

TEST_F(BoardPnsRouterTest, testDragOnOnePadOfADeviceIsRefused) {
  BoardPnsRouter router(*mBoard, makeSettings());

  // A device is dragged by all of its pads or not at all: the router moves
  // the pads it is given while the host moves the device as a whole, so a
  // pad left out would keep its traces where the copper no longer is.
  QVector<quint64> pads;
  Point pos;
  foreach (const BI_Device* device, mBoard->getDeviceInstances()) {
    const QVector<quint64> candidate = syncedPadsOf(router, *device);
    if (candidate.count() >= 2) {
      pads = candidate;
      pos = router.getHostRef(candidate.first()).pad->getPosition();
      break;
    }
  }
  ASSERT_GE(pads.count(), 2) << "no device with two pads the router knows";

  EXPECT_EQ(router.startDragging(pos, pads.first(), false),
            BoardPnsRouter::StartResult::IncompleteDeviceDrag);
  EXPECT_FALSE(router.isDragging());
  EXPECT_FALSE(router.isRoutingInProgress());

  // A pad mixed with a trace is not a drag this host can apply either.
  const std::optional<DragCase> netLineDrag = findDragCase(*mBoard);
  ASSERT_TRUE(netLineDrag.has_value()) << "no net line the router drags";
  QVector<quint64> mixed = pads;
  mixed.append(netLineDrag->hostId);
  EXPECT_EQ(router.startDragging(pos, mixed, false),
            BoardPnsRouter::StartResult::IncompleteDeviceDrag);

  // And nothing at all is not something to drag either.
  EXPECT_EQ(router.startDragging(pos, 0, false),
            BoardPnsRouter::StartResult::NothingToDrag);
}

TEST_F(BoardPnsRouterTest, testAbortDraggingCommitsNothing) {
  const std::optional<DragCase> drag = findDragCase(*mBoard);
  ASSERT_TRUE(drag.has_value()) << "no net line the router moves 1 mm aside";

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startDragging(drag->pos, drag->hostId, false),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(drag->target, 0);

  router.abortRouting();
  EXPECT_FALSE(router.isDragging());
  EXPECT_FALSE(router.isRoutingInProgress());
  EXPECT_TRUE(router.getPreview().items.isEmpty());
  EXPECT_TRUE(router.getCommit().removed.isEmpty());
  EXPECT_TRUE(router.getCommit().added.isEmpty());
  EXPECT_TRUE(router.getCommit().updated.isEmpty());

  // A drag is committed by its fix and by nothing else, so stopping one
  // which was never fixed must not move anything either.
  BoardPnsRouter stopped(*mBoard, makeSettings());
  ASSERT_EQ(stopped.startDragging(drag->pos, drag->hostId, false),
            BoardPnsRouter::StartResult::Ok);
  stopped.moveTo(drag->target, 0);
  const BoardPnsCommit commit = stopped.stopRouting();
  EXPECT_TRUE(commit.removed.isEmpty());
  EXPECT_TRUE(commit.added.isEmpty());
  EXPECT_TRUE(commit.updated.isEmpty());
  EXPECT_FALSE(stopped.isRoutingInProgress());
}

/*******************************************************************************
 *  Session recording
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testRecordedSessionIsInTheRouterFixtureFormat) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter::Settings settings = makeSettings();
  settings.recordSession = true;

  BoardPnsRouter router(*mBoard, settings);
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);

  const QString recording = router.takeRecording();
  ASSERT_FALSE(recording.isEmpty());

  // The shape of the router crate's recorded session format, as in its
  // tests/fixtures/sessions/*.txt: a comment header, the version record,
  // the board, the events and the commit the session answered with. A file
  // holding this is a fixture the crate replays without any conversion.
  EXPECT_TRUE(recording.startsWith("# A pnsrouter session recording."))
      << recording.left(80).toStdString();
  EXPECT_TRUE(recording.contains("\npnsrouter-session 1\n"));
  EXPECT_TRUE(recording.contains("\nsnapshot "));
  EXPECT_TRUE(recording.contains("\nsettings 0 mode "));
  EXPECT_TRUE(recording.contains("\nsizes 0 track-width 250000\n"));
  EXPECT_TRUE(recording.contains("\nevent start-routing "));
  EXPECT_TRUE(recording.contains("\nevent move-to "));
  EXPECT_TRUE(recording.contains("\nevent fix-route "));
  EXPECT_TRUE(recording.contains("\ncommit\n"));
  EXPECT_TRUE(recording.contains("\nadded segment "));
  EXPECT_TRUE(recording.endsWith("\n"));

  // The board is really in there, not just the session's own geometry.
  EXPECT_GT(recording.count("\nitem "), 1);

  // Taking the recording ends it, which is the crate's own semantics.
  EXPECT_TRUE(router.takeRecording().isEmpty());
}

TEST_F(BoardPnsRouterTest, testNothingIsRecordedWithoutTheSetting) {
  EXPECT_FALSE(makeSettings().recordSession);

  BoardPnsRouter router(*mBoard, makeSettings());
  EXPECT_TRUE(router.takeRecording().isEmpty());
}

/*******************************************************************************
 *  Shove iteration limit
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testShoveIterationLimitReachesTheEngine) {
  EXPECT_EQ(makeSettings().shoveIterationLimit, 250U);  // KiCad's value.

  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter::Settings settings = makeSettings();
  settings.shoveIterationLimit = 50;
  settings.recordSession = true;  // The only read back of the limit.

  BoardPnsRouter router(*mBoard, settings);
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.getCommit().added.isEmpty());

  const QString recording = router.takeRecording();
  EXPECT_TRUE(recording.contains("\nsettings 0 shove-iteration-limit 50\n"))
      << recording.left(400).toStdString();
}

/*******************************************************************************
 *  Corner mode
 ******************************************************************************/

/**
 * @brief The corner mode is part of the settings, not a command
 *
 * Which is what lets a session rebuilt after a commit start on the mode the
 * user left, without the tool state re-toggling it. The session recording is
 * the only read back of the engine's settings.
 */
TEST_F(BoardPnsRouterTest, testCornerModeReachesTheEngine) {
  EXPECT_FALSE(makeSettings().cornerMode90);  // 45 degrees by default.

  BoardPnsRouter::Settings settings = makeSettings();
  settings.recordSession = true;
  {
    BoardPnsRouter router(*mBoard, settings);
    EXPECT_TRUE(router.takeRecording().contains(
        "\nsettings 0 corner-mode mitered-45\n"));
  }

  settings.cornerMode90 = true;
  BoardPnsRouter router(*mBoard, settings);
  const QString recording = router.takeRecording();
  EXPECT_TRUE(recording.contains("\nsettings 0 corner-mode mitered-90\n"))
      << recording.left(400).toStdString();
}

/*******************************************************************************
 *  Keepout zones
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testRouteWalksAroundAKeepoutZone) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  // Where the route goes while nothing is in the way, so that the zone can be
  // put right on top of it. Committing changes nothing on the board, so the
  // second session below starts from the same place.
  const QVector<BoardPnsNewItem> direct =
      routeOnce(*mBoard, *start, *target, BoardPnsRouter::Mode::Walkaround);
  ASSERT_FALSE(direct.isEmpty());
  const std::optional<Point> center = longestSegmentMiddle(direct);
  ASSERT_TRUE(center.has_value());

  const BI_Zone* zone = addKeepoutZone(*mBoard, *center, Length(1000000));
  const Path& outline = zone->getData().getOutline();

  // The control: the route this one has to avoid ran straight through where
  // the zone now is.
  int through = 0;
  foreach (const BoardPnsNewItem& item, direct) {
    if (isCopperInZone(item, outline)) {
      ++through;
    }
  }
  EXPECT_GT(through, 0);

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);

  const BoardPnsCommit commit = router.getCommit();
  ASSERT_FALSE(commit.added.isEmpty());
  foreach (const BoardPnsNewItem& item, commit.added) {
    const BoardPnsNewSegment* segment = item.getSegment();
    if (!segment) {
      continue;  // isCopperInZone() answers false for a via anyway.
    }
    EXPECT_FALSE(isCopperInZone(item, outline))
        << "a segment ending at " << segment->end.getX().toNm() << ", "
        << segment->end.getY().toNm() << " nm is in the zone";
  }
}

/**
 * @brief A pad which a keepout zone stands on is not a place to route from
 *
 * The pad itself is routable, so the gate's per object half is happy; what
 * refuses the start is the probe trace it puts down there, which the zone
 * excludes.
 */
TEST_F(BoardPnsRouterTest, testStartInsideAKeepoutZoneIsRefused) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  addKeepoutZone(*mBoard, start->pos, Length(2000000));  // 2 mm.

  BoardPnsRouter router(*mBoard, makeSettings());
  EXPECT_EQ(router.isStartingPointRoutable(start->pos, start->hostId,
                                           Layer::topCopper()),
            BoardPnsRouter::StartResult::StartPointViolatesRules);
  EXPECT_NE(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_FALSE(router.isRoutingInProgress());
  EXPECT_TRUE(router.getPreview().items.isEmpty());
}

/**
 * @brief Mark obstacles mode draws a keepout instead of avoiding it
 *
 * The mode puts the route where the user pointed and marks what it runs into,
 * so a zone has to reach the preview as a violation naming the zone rather
 * than bending the route the way ::testRouteWalksAroundAKeepoutZone expects
 * of the walkaround, and rather than stopping the session.
 *
 * Whether a route which breaks a rule may then be committed is
 * ::librepcb::BoardPnsRouter::Settings::allowDrcViolations, which is off
 * here, so a colliding fix is refused whatever it collided with; the keepout
 * is no different from the copper the fixture already has in the way. The
 * switch itself is ::testAllowDrcViolationsCommitsACollidingRoute.
 */
TEST_F(BoardPnsRouterTest, testMarkObstaclesReportsAKeepoutZone) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  // Where this mode puts the head, so that the zone can be put on top of it.
  std::optional<Point> center;
  {
    BoardPnsRouter probe(*mBoard,
                         makeSettings(BoardPnsRouter::Mode::MarkObstacles));
    ASSERT_EQ(probe.startRouting(start->pos, start->hostId, Layer::topCopper()),
              BoardPnsRouter::StartResult::Ok);
    probe.moveTo(*target, 0);
    center = longestHeadPieceMiddle(probe.getPreview());
  }
  ASSERT_TRUE(center.has_value());

  const BI_Zone* zone = addKeepoutZone(*mBoard, *center, Length(1000000));

  BoardPnsRouter router(*mBoard,
                        makeSettings(BoardPnsRouter::Mode::MarkObstacles));
  // A keepout under the route does not stop a session whose job is to show
  // what the route breaks.
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);

  int reported = 0;
  foreach (const BoardPnsViolation& violation, router.getPreview().violations) {
    if (violation.host.zone == zone) {
      ++reported;
      // A keepout excludes at its exact boundary, so there is no distance to
      // draw around it.
      EXPECT_EQ(violation.clearance.toNm(), 0);
      // Every triangle is a compound primitive, so the zone keeps being drawn
      // and the violation is shown on top of it rather than in its place.
      EXPECT_FALSE(violation.hideOriginal);
    }
  }
  EXPECT_GT(reported, 0);

  // And the head was not bent around it, which is what separates this mode
  // from the walkaround.
  int through = 0;
  foreach (const BoardPnsPreviewItem& item, router.getPreview().items) {
    if (item.style == BoardPnsPreviewStyle::Head) {
      through += piecesInZone(item, zone->getData().getOutline());
    }
  }
  EXPECT_GT(through, 0);
}

/*******************************************************************************
 *  Allow DRC violations
 ******************************************************************************/

/**
 * @brief The DRC violation switch decides whether a colliding route commits
 *
 * KiCad's "Allow DRC violations", which only mark obstacles mode acts on: the
 * mode puts the trace where the user pointed whatever is there, and this is
 * what says whether the user may then keep it. LibrePCB's own design rule
 * check reports the collision afterwards either way.
 */
TEST_F(BoardPnsRouterTest, testAllowDrcViolationsCommitsACollidingRoute) {
  BoardPnsRouter probeRouter(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findCollidingTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no copper of another net to route into";

  // The control: with the switch off the fix is refused and the session
  // carries on placing, so nothing reaches the board.
  {
    BoardPnsRouter router(*mBoard,
                          makeSettings(BoardPnsRouter::Mode::MarkObstacles));
    ASSERT_EQ(
        router.startRouting(start->pos, start->hostId, Layer::topCopper()),
        BoardPnsRouter::StartResult::Ok);
    router.moveTo(*target, 0);
    EXPECT_FALSE(router.getPreview().violations.isEmpty());
    EXPECT_EQ(router.fixRoute(*target, 0, true),
              BoardPnsRouter::FixOutcome::Continue);
    EXPECT_TRUE(router.isRoutingInProgress());
    EXPECT_TRUE(router.getCommit().isEmpty());
  }

  // The same route with the switch on is committed, collision and all.
  BoardPnsRouter router(
      *mBoard, makeSettings(BoardPnsRouter::Mode::MarkObstacles, true));
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  EXPECT_FALSE(router.getPreview().violations.isEmpty());
  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isRoutingInProgress());
  const BoardPnsCommit commit = router.getCommit();
  EXPECT_FALSE(commit.isEmpty());
  EXPECT_FALSE(commit.added.isEmpty());
}

/*******************************************************************************
 *  Differential pairs
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testRouteDiffPairFromPadPairIntoFreeSpace) {
  const std::optional<DiffPairCase> pair = findDiffPairCase(*mProject, *mBoard);
  ASSERT_TRUE(pair.has_value())
      << "no two pads of the fixture can be made into a routable pair";

  BoardPnsRouter router(*mBoard, makeDiffPairSettings());
  ASSERT_EQ(router.isStartingPointRoutableDiffPair(pair->pos, pair->hostId,
                                                   Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  ASSERT_EQ(router.startRoutingDiffPair(pair->pos, pair->hostId,
                                        Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isRoutingInProgress());

  // Both nets, positive first, which is what the net filter of the tool
  // takes its two entries from.
  const QVector<NetSignal*> nets = router.getCurrentNets();
  ASSERT_EQ(nets.count(), 2);
  EXPECT_EQ(nets.at(0), pair->positive);
  EXPECT_EQ(nets.at(1), pair->negative);

  // Two lanes are previewed, one per net, both on the layer being routed.
  router.moveTo(pair->target, 0);
  QSet<const NetSignal*> headNets;
  foreach (const BoardPnsPreviewItem& item, router.getPreview().items) {
    if (item.style == BoardPnsPreviewStyle::Head) {
      EXPECT_EQ(item.layer, &Layer::topCopper());
      EXPECT_GE(item.path.count(), 2);
      headNets.insert(item.net);
    }
  }
  EXPECT_TRUE(headNets.contains(pair->positive));
  EXPECT_TRUE(headNets.contains(pair->negative));

  ASSERT_EQ(router.fixRoute(pair->target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isRoutingInProgress());

  // One commit carries the segments of both nets, at the pair width, which
  // is what lets the commit applier stitch them into two net segments.
  const BoardPnsCommit commit = router.getCommit();
  int positiveSegments = 0;
  int negativeSegments = 0;
  foreach (const BoardPnsNewItem& item, commit.added) {
    const BoardPnsNewSegment* segment = item.getSegment();
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment->layer, &Layer::topCopper());
    EXPECT_EQ((*segment->width).toNm(), 250000);
    if (item.net == pair->positive) {
      ++positiveSegments;
    } else if (item.net == pair->negative) {
      ++negativeSegments;
    } else {
      ADD_FAILURE() << "a segment on a net which is not part of the pair";
    }
  }
  EXPECT_GT(positiveSegments, 0);
  EXPECT_GT(negativeSegments, 0);
}

TEST_F(BoardPnsRouterTest, testDiffPairOnAnUnpairedNetIsRefused) {
  BoardPnsRouter router(*mBoard, makeDiffPairSettings());
  const std::optional<RouteStart> start = findStartPad(router, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  // No net of the fixture is half of a pair, so the router cannot name a
  // second net to route and says so rather than routing one trace.
  EXPECT_EQ(router.isStartingPointRoutableDiffPair(start->pos, start->hostId,
                                                   Layer::topCopper()),
            BoardPnsRouter::StartResult::NotADiffPair);
  EXPECT_EQ(
      router.startRoutingDiffPair(start->pos, start->hostId,
                                  Layer::topCopper()),
      BoardPnsRouter::StartResult::NotADiffPair);
  EXPECT_FALSE(router.isRoutingInProgress());

  // A single trace from the very same point is fine, so the refusal is
  // about the pair and not about the start point.
  EXPECT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
}

TEST_F(BoardPnsRouterTest, testDiffPairInFreeSpaceIsRefused) {
  BoardPnsRouter probeRouter(*mBoard, makeDiffPairSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  // A single trace may start in free space, a pair may not: it has no other
  // way to learn which two nets it is routing.
  BoardPnsRouter router(*mBoard, makeDiffPairSettings());
  EXPECT_EQ(router.isStartingPointRoutableDiffPair(*target, 0,
                                                   Layer::topCopper()),
            BoardPnsRouter::StartResult::PairNeedsStartItem);
  EXPECT_EQ(router.startRoutingDiffPair(*target, 0, Layer::topCopper()),
            BoardPnsRouter::StartResult::PairNeedsStartItem);
  EXPECT_FALSE(router.isRoutingInProgress());
}

TEST_F(BoardPnsRouterTest, testDiffPairGapBelowMinClearanceIsRefused) {
  const Length minClearance =
      *mBoard->getDrcSettings().getMinCopperCopperClearance();
  ASSERT_GT(minClearance.toNm(), 1);

  BoardPnsRouter::Settings settings = makeDiffPairSettings();
  settings.diffPairGap = PositiveLength(Length(minClearance.toNm() - 1));

  BoardPnsRouter router(*mBoard, settings);
  const std::optional<RouteStart> start = findStartPad(router, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  // The one consistency check the router makes between the pair geometry
  // and the clearance rules, and it runs before anything else, so it wins
  // over the missing pair of this net.
  EXPECT_EQ(router.isStartingPointRoutableDiffPair(start->pos, start->hostId,
                                                   Layer::topCopper()),
            BoardPnsRouter::StartResult::PairGapBelowMinClearance);
}

/*******************************************************************************
 *  Length tuning
 ******************************************************************************/

TEST_F(BoardPnsRouterTest, testTuneTraceToALongerTarget) {
  const std::optional<TuningCase> tuning = findTuningCase(*mBoard);
  ASSERT_TRUE(tuning.has_value())
      << "no trace of the fixture has room for meanders";
  const Layer& layer = tuning->netLine->getLayer();
  const NetSignal* net = tuning->netLine->getNetSegment().getNetSignal();

  BoardPnsRouter::TuningSettings settings;
  settings.target = tuning->target;
  settings.tolerance = sTuningTolerance;

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startTuning(tuning->start, tuning->hostId,
                               BoardPnsTuningMode::Single, settings),
            BoardPnsRouter::StartResult::Ok);
  EXPECT_TRUE(router.isTuning());
  EXPECT_TRUE(router.isRoutingInProgress());
  EXPECT_FALSE(router.isDragging());

  // The cursor decides how much of the trace meanders, so the readout only
  // exists once the session has been moved.
  router.moveTo(tuning->end, 0);
  const std::optional<BoardPnsTuningInfo> readout = router.getPreview().tuning;
  ASSERT_TRUE(readout.has_value());
  EXPECT_EQ(readout->status, BoardPnsTuningStatus::Tuned);
  EXPECT_EQ(readout->mode, BoardPnsTuningMode::Single);
  EXPECT_GE(readout->result, tuning->target - sTuningTolerance);
  EXPECT_LE(readout->result, tuning->target + sTuningTolerance);
  EXPECT_EQ(readout->target.opt, tuning->target);
  // A length session measures no skew and has no coupled trace.
  EXPECT_FALSE(readout->skew.has_value());
  EXPECT_FALSE(readout->skewTarget.has_value());
  EXPECT_FALSE(readout->coupledLength.has_value());
  EXPECT_EQ((*readout->amplitude).toNm(), 1000000);
  EXPECT_EQ((*readout->spacing).toNm(), 600000);

  // The meandered trace is drawn like any other head, so a caller which
  // draws a route needs nothing new for a tuning session.
  int headItems = 0;
  foreach (const BoardPnsPreviewItem& item, router.getPreview().items) {
    if (item.style == BoardPnsPreviewStyle::Head) {
      ++headItems;
      EXPECT_EQ(item.layer, &layer);
      EXPECT_EQ(item.net, net);
    }
  }
  EXPECT_GT(headItems, 0);

  // A tuning fix is always terminal: there is no second leg to tune.
  ASSERT_EQ(router.fixRoute(tuning->end, 0, false),
            BoardPnsRouter::FixOutcome::Finished);
  EXPECT_FALSE(router.isTuning());
  EXPECT_FALSE(router.isRoutingInProgress());

  // The commit replaces the tuned copper with the meandered chain, on the
  // same net and the same layer, and there is more of it than there was.
  const BoardPnsCommit commit = router.getCommit();
  int segments = 0;
  auto check = [&](const BoardPnsNewItem& item) {
    const BoardPnsNewSegment* segment = item.getSegment();
    ASSERT_NE(segment, nullptr);
    ++segments;
    EXPECT_EQ(segment->layer, &layer);
    EXPECT_EQ(item.net, net);
  };
  foreach (const BoardPnsNewItem& item, commit.added) {
    check(item);
  }
  for (const auto& pair : commit.updated) {
    check(pair.second);
  }
  EXPECT_GT(segments, 1);
  EXPECT_GT(addedCopperLength(commit), removedCopperLength(commit));

  // The commit applier needs nothing new for a tuning session: the tuned
  // trace goes away exactly as a dragged one does, either as a removal or
  // as an update which kept its identity, and no device moves.
  EXPECT_TRUE(dropsNetLine(commit, *tuning->netLine));
  EXPECT_TRUE(commit.movedDevices.isEmpty());
}

TEST_F(BoardPnsRouterTest, testTuningOnAPadIsRefused) {
  BoardPnsRouter router(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(router, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  // Only a trace can be lengthened; a pad, a via or a hole is refused by
  // its own name rather than by a catch-all.
  EXPECT_EQ(router.startTuning(start->pos, start->hostId,
                               BoardPnsTuningMode::Single,
                               BoardPnsRouter::TuningSettings{}),
            BoardPnsRouter::StartResult::NotATrack);
  EXPECT_FALSE(router.isTuning());
  EXPECT_FALSE(router.isRoutingInProgress());

  // And a session needs something to lengthen, so free space is refused
  // too, where a single trace placement may start there.
  EXPECT_EQ(router.startTuning(start->pos, 0, BoardPnsTuningMode::Single,
                               BoardPnsRouter::TuningSettings{}),
            BoardPnsRouter::StartResult::TuningNeedsStartItem);
  EXPECT_FALSE(router.isTuning());
}

TEST_F(BoardPnsRouterTest, testTuningOnAnUnpairedNetIsRefused) {
  const std::optional<TuningCase> tuning = findTunableTrace(*mBoard);
  ASSERT_TRUE(tuning.has_value()) << "no trace of the fixture can be tuned";

  // No net of the fixture is half of a pair, so both pair modes refuse the
  // very trace the single mode tunes, and each names its own mode.
  BoardPnsRouter router(*mBoard, makeSettings());
  EXPECT_EQ(router.startTuning(tuning->start, tuning->hostId,
                               BoardPnsTuningMode::DiffPair,
                               BoardPnsRouter::TuningSettings{}),
            BoardPnsRouter::StartResult::NotADiffPairForTuning);
  EXPECT_EQ(router.startTuning(tuning->start, tuning->hostId,
                               BoardPnsTuningMode::Skew,
                               BoardPnsRouter::TuningSettings{}),
            BoardPnsRouter::StartResult::NotADiffPairForSkew);
  EXPECT_EQ(router.startTuning(tuning->start, tuning->hostId,
                               BoardPnsTuningMode::Single,
                               BoardPnsRouter::TuningSettings{}),
            BoardPnsRouter::StartResult::Ok);
}

TEST_F(BoardPnsRouterTest, testAmplitudeAndSpacingStepsChangeTheReadout) {
  const std::optional<TuningCase> tuning = findTunableTrace(*mBoard);
  ASSERT_TRUE(tuning.has_value()) << "no trace of the fixture can be tuned";

  BoardPnsRouter::TuningSettings settings;
  settings.target = tuning->baseline + Length(2000000);  // 2 mm more.
  settings.tolerance = sTuningTolerance;

  BoardPnsRouter router(*mBoard, makeSettings());
  ASSERT_EQ(router.startTuning(tuning->start, tuning->hostId,
                               BoardPnsTuningMode::Single, settings),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  const Length amplitude = *router.getPreview().tuning->amplitude;
  const Length spacing = *router.getPreview().tuning->spacing;
  EXPECT_EQ(amplitude.toNm(), (*settings.maxAmplitude).toNm());
  EXPECT_EQ(spacing.toNm(), (*settings.spacing).toNm());

  // Neither step produces a frame of its own, so the readout only catches
  // up on the next move, which is how the router's other hosts drive them.
  EXPECT_TRUE(router.amplitudeStep(1));
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  EXPECT_EQ(*router.getPreview().tuning->amplitude,
            amplitude + *settings.step);

  // Down again, and the amplitude is back where it started.
  EXPECT_TRUE(router.amplitudeStep(-1));
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  EXPECT_EQ(*router.getPreview().tuning->amplitude, amplitude);

  // The spacing is floored by the tuned trace's width plus its clearance,
  // and the fixture's traces are wide enough for that floor to swallow the
  // first few steps, so it is taken well clear of the floor before the
  // step size itself is asserted on.
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(router.spacingStep(1));
  }
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  const Length wideSpacing = *router.getPreview().tuning->spacing;
  EXPECT_GT(wideSpacing, spacing);

  EXPECT_TRUE(router.spacingStep(1));
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  EXPECT_EQ(*router.getPreview().tuning->spacing,
            wideSpacing + *settings.step);

  EXPECT_TRUE(router.spacingStep(-1));
  router.moveTo(tuning->end, 0);
  ASSERT_TRUE(router.getPreview().tuning.has_value());
  EXPECT_EQ(*router.getPreview().tuning->spacing, wideSpacing);

  // Both refuse a session which is not tuning, which is every route and
  // every drag.
  router.abortRouting();
  EXPECT_FALSE(router.isTuning());
  EXPECT_FALSE(router.amplitudeStep(1));
  EXPECT_FALSE(router.spacingStep(1));
  EXPECT_FALSE(router.getPreview().tuning.has_value());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
