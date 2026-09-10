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
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/boardpnsrouter.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_hole.h>
#include <librepcb/core/project/board/items/bi_pad.h>
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

static BoardPnsRouter::Settings makeSettings() noexcept {
  return BoardPnsRouter::Settings{
      BoardPnsRouter::Mode::Walkaround,
      PositiveLength(Length(250000)),  // 0.25 mm trace, above the minimum.
      PositiveLength(Length(700000)),  // 0.7 mm via.
      PositiveLength(Length(300000)),  // 0.3 mm via drill.
  };
}

static bool isNull(const BoardPnsHostRef& ref) noexcept {
  return (!ref.netLine) && (!ref.via) && (!ref.pad) && (!ref.hole) &&
      (!ref.polygon);
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
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
