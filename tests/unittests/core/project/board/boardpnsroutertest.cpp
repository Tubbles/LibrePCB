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
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_hole.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_zone.h>
#include <librepcb/core/project/circuit/netsignal.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/types/uuid.h>

#include <QtCore>

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
    BoardPnsRouter::Mode mode = BoardPnsRouter::Mode::Walkaround) noexcept {
  return BoardPnsRouter::Settings{
      mode,
      PositiveLength(Length(250000)),  // 0.25 mm trace, above the minimum.
      PositiveLength(Length(700000)),  // 0.7 mm via.
      PositiveLength(Length(300000)),  // 0.3 mm via drill.
  };
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
  return (item.kind == BoardPnsNewItem::Kind::Segment) &&
      isInZone(item.start, item.end, item.width, outline);
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
    if (item.kind != BoardPnsNewItem::Kind::Segment) {
      continue;
    }
    const Length length = *(item.end - item.start).getLength();
    if (length > longest) {
      longest = length;
      middle = Point(
          Length((item.start.getX().toNm() + item.end.getX().toNm()) / 2),
          Length((item.start.getY().toNm() + item.end.getY().toNm()) / 2));
    }
  }
  return middle;
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
  const BoardPnsCommit& commit = router.getCommit();
  EXPECT_TRUE(commit.removed.isEmpty());
  int segments = 0;
  foreach (const BoardPnsNewItem& item, commit.added) {
    ASSERT_EQ(item.kind, BoardPnsNewItem::Kind::Segment);
    ++segments;
    EXPECT_EQ(item.layer, &Layer::topCopper());
    EXPECT_EQ(item.net, start->pad->getNetSignal());
    EXPECT_EQ((*item.width).toNm(), 250000);
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
  const BoardPnsCommit& commit = router.getCommit();
  EXPECT_TRUE(dropsNetLine(commit, *drag->netLine));
  int segments = 0;
  foreach (const BoardPnsNewItem& item, commit.added) {
    ASSERT_EQ(item.kind, BoardPnsNewItem::Kind::Segment);
    ++segments;
    EXPECT_EQ(item.layer, &layer);
    EXPECT_EQ(item.net, net);
  }
  for (const auto& pair : commit.updated) {
    ASSERT_EQ(pair.second.kind, BoardPnsNewItem::Kind::Segment);
    ++segments;
    EXPECT_EQ(pair.second.layer, &layer);
    EXPECT_EQ(pair.second.net, net);
  }
  EXPECT_GT(segments, 0);
}

TEST_F(BoardPnsRouterTest, testDragOnAPadIsRefused) {
  BoardPnsRouter router(*mBoard, makeSettings());
  const std::optional<RouteStart> start = findStartPad(router, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";

  // A selection of nothing but pads is a component drag to the router,
  // which this host does not offer yet.
  EXPECT_EQ(router.startDragging(start->pos, start->hostId, false),
            BoardPnsRouter::StartResult::ComponentDragUnsupported);
  EXPECT_FALSE(router.isDragging());
  EXPECT_FALSE(router.isRoutingInProgress());

  // And nothing at all is not something to drag either.
  EXPECT_EQ(router.startDragging(start->pos, 0, false),
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

  const BoardPnsCommit& commit = router.getCommit();
  ASSERT_FALSE(commit.added.isEmpty());
  foreach (const BoardPnsNewItem& item, commit.added) {
    EXPECT_FALSE(isCopperInZone(item, outline))
        << "a segment ending at " << item.end.getX().toNm() << ", "
        << item.end.getY().toNm() << " nm is in the zone";
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
 * Whether a route which breaks a rule may then be committed is the router's
 * own `allow_drc_violations` setting, KiCad's "Allow DRC violations" switch.
 * This host does not expose it, so a colliding fix is refused in this mode
 * whatever it collided with; the keepout is no different from the copper the
 * fixture already has in the way.
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
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
