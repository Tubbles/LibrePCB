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
#include <librepcb/core/fileio/transactionaldirectory.h>
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/boardpnsrouter.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netpoint.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_via.h>
#include <librepcb/core/project/circuit/circuit.h>
#include <librepcb/core/project/circuit/netsignal.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/types/uuid.h>
#include <librepcb/editor/project/cmd/cmdboardapplypnscommit.h>
#include <librepcb/editor/undostack.h>

#include <QtCore>

#include <memory>
#include <optional>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

/// The trace width every hand built commit places with.
static PositiveLength traceWidth() noexcept {
  return PositiveLength(250000);  // 0.25 mm.
}

/// A point in millimetres, well outside the fixture's own copper
///
/// The Gerber Test board lives between roughly -90 mm and 120 mm in x and
/// -30 mm and 130 mm in y, so the topology cases are built out at 300 mm and
/// beyond where nothing of the fixture can interfere with an anchor lookup.
static Point mm(qreal x, qreal y) noexcept {
  return Point::fromMm(x, y);
}

static BoardPnsNewItem makeSegment(const Point& start, const Point& end,
                                   const Layer& layer,
                                   NetSignal* net) noexcept {
  BoardPnsNewSegment segment;
  segment.start = start;
  segment.end = end;
  segment.width = traceWidth();
  segment.layer = &layer;

  BoardPnsNewItem item;
  item.net = net;
  item.geometry = segment;
  return item;
}

static BoardPnsNewItem makeVia(const Point& pos, NetSignal* net) noexcept {
  BoardPnsNewVia via;
  via.position = pos;
  via.diameter = PositiveLength(700000);  // 0.7 mm.
  via.drill = PositiveLength(300000);  // 0.3 mm.
  via.startLayer = &Layer::topCopper();
  via.endLayer = &Layer::botCopper();

  BoardPnsNewItem item;
  item.net = net;
  item.geometry = via;
  return item;
}

static BI_NetPoint* netPointAt(const Board& board, const Point& pos) noexcept {
  foreach (BI_NetSegment* segment, board.getNetSegments()) {
    foreach (BI_NetPoint* netPoint, segment->getNetPoints()) {
      if (netPoint->getPosition() == pos) {
        return netPoint;
      }
    }
  }
  return nullptr;
}

static BI_Via* viaAt(const Board& board, const Point& pos) noexcept {
  foreach (BI_NetSegment* segment, board.getNetSegments()) {
    foreach (BI_Via* via, segment->getVias()) {
      if (via->getPosition() == pos) {
        return via;
      }
    }
  }
  return nullptr;
}

static BI_NetLine* netLineBetween(const Board& board, const Point& a,
                                  const Point& b) noexcept {
  foreach (BI_NetSegment* segment, board.getNetSegments()) {
    foreach (BI_NetLine* netLine, segment->getNetLines()) {
      const Point& p1 = netLine->getP1().getPosition();
      const Point& p2 = netLine->getP2().getPosition();
      if (((p1 == a) && (p2 == b)) || ((p1 == b) && (p2 == a))) {
        return netLine;
      }
    }
  }
  return nullptr;
}

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

/**
 * @brief Applies hand built commits to the fixture the router tests use
 *
 * The commits are built by hand rather than routed, because the topology
 * cases have to be deterministic and a routed commit follows whatever the
 * fixture's copper allows. The one exception is the end to end test, whose
 * whole point is that a real session's commit goes onto the board.
 *
 * The project is `tests/data/projects/Gerber Test`, the same one
 * `BoardPnsRouterTest` loads: it is the smallest fixture that has devices
 * with pads on nets, traces and vias, which the end to end test needs.
 */
class CmdBoardApplyPnsCommitTest : public ::testing::Test {
protected:
  std::unique_ptr<Project> mProject;
  Board* mBoard = nullptr;
  NetSignal* mNet = nullptr;
  std::unique_ptr<UndoStack> mUndoStack;

  void SetUp() override {
    const FilePath fp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
    std::shared_ptr<TransactionalFileSystem> fs =
        TransactionalFileSystem::openRO(fp.getParentDir());
    ProjectLoader loader;
    mProject = loader.open(std::make_unique<TransactionalDirectory>(fs),
                           fp.getFilename());  // can throw
    ASSERT_FALSE(mProject->getBoards().isEmpty());
    mBoard = mProject->getBoards().first();
    ASSERT_FALSE(mProject->getCircuit().getNetSignals().isEmpty());
    mNet = mProject->getCircuit().getNetSignals().first();
    mUndoStack.reset(new UndoStack());
  }

  void TearDown() override {
    mUndoStack.reset();
    mProject.reset();
  }

  int netSegmentCount() const noexcept {
    return mBoard->getNetSegments().count();
  }

  int netPointCount() const noexcept {
    int count = 0;
    foreach (const BI_NetSegment* segment, mBoard->getNetSegments()) {
      count += segment->getNetPoints().count();
    }
    return count;
  }

  int netLineCount() const noexcept {
    int count = 0;
    foreach (const BI_NetSegment* segment, mBoard->getNetSegments()) {
      count += segment->getNetLines().count();
    }
    return count;
  }

  int viaCount() const noexcept {
    int count = 0;
    foreach (const BI_NetSegment* segment, mBoard->getNetSegments()) {
      count += segment->getVias().count();
    }
    return count;
  }

  /**
   * @brief Put a chain of net points and net lines on the board directly
   *
   * Not through the undo stack, so that undoing the applied commit does not
   * undo the topology the test set up for it.
   */
  BI_NetSegment* addTrace(const QVector<Point>& positions,
                          const Layer& layer) {
    BI_NetSegment* segment =
        new BI_NetSegment(*mBoard, Uuid::createRandom(), mNet);
    QList<BI_NetPoint*> netPoints;
    QList<BI_NetLine*> netLines;
    foreach (const Point& pos, positions) {
      netPoints.append(new BI_NetPoint(*segment, Uuid::createRandom(), pos));
    }
    for (int i = 1; i < netPoints.count(); ++i) {
      netLines.append(new BI_NetLine(*segment, Uuid::createRandom(),
                                     *netPoints.at(i - 1), *netPoints.at(i),
                                     layer, traceWidth()));
    }
    segment->addElements({}, {}, netPoints, netLines);  // can throw
    mBoard->addNetSegment(*segment);  // can throw
    return segment;
  }

  /**
   * @brief Find a footprint pad with a net which no trace is attached to yet
   */
  BI_Pad* findFreePadWithNet() const noexcept {
    foreach (const BI_Device* device, mBoard->getDeviceInstances()) {
      foreach (BI_Pad* pad, device->getPads()) {
        if (pad->getNetSignal() && pad->isOnLayer(Layer::topCopper()) &&
            (!pad->getNetSegmentOfLines())) {
          return pad;
        }
      }
    }
    return nullptr;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(CmdBoardApplyPnsCommitTest, testEndpointOnPad) {
  BI_Pad* pad = findFreePadWithNet();
  ASSERT_NE(pad, nullptr) << "no unconnected top layer pad with a net";
  const Point start = pad->getPosition();
  const Point end = start + mm(300, 300);

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();

  BoardPnsCommit commit;
  commit.added.append(
      makeSegment(start, end, Layer::topCopper(), pad->getNetSignal()));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // The pad had no net segment, so the route opened one on the pad's net and
  // anchored itself on the pad rather than on a net point of its own.
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  BI_NetLine* netLine = netLineBetween(*mBoard, start, end);
  ASSERT_NE(netLine, nullptr);
  EXPECT_EQ(netLine->getNetSegment().getNetSignal(), pad->getNetSignal());
  EXPECT_EQ(&netLine->getLayer(), &Layer::topCopper());
  EXPECT_EQ(netLine->getWidth(), traceWidth());
  EXPECT_TRUE((&netLine->getP1() == pad) || (&netLine->getP2() == pad));

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_EQ(netLineBetween(*mBoard, start, end), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  netLine = netLineBetween(*mBoard, start, end);
  ASSERT_NE(netLine, nullptr);
  EXPECT_TRUE((&netLine->getP1() == pad) || (&netLine->getP2() == pad));
}

TEST_F(CmdBoardApplyPnsCommitTest, testEndpointOnNetPoint) {
  const Point p0 = mm(300, 200);
  const Point p1 = mm(305, 200);
  const Point p2 = mm(305, 205);
  addTrace({p0, p1}, Layer::topCopper());

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();
  BI_NetPoint* anchor = netPointAt(*mBoard, p1);
  ASSERT_NE(anchor, nullptr);

  BoardPnsCommit commit;
  commit.added.append(makeSegment(p1, p2, Layer::topCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // The existing net point is the anchor, so no new net segment is opened.
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  BI_NetLine* netLine = netLineBetween(*mBoard, p1, p2);
  ASSERT_NE(netLine, nullptr);
  EXPECT_TRUE((&netLine->getP1() == anchor) || (&netLine->getP2() == anchor));

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_EQ(netLineBetween(*mBoard, p1, p2), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  ASSERT_NE(netLineBetween(*mBoard, p1, p2), nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testEndpointOnExistingVia) {
  const Point viaPos = mm(320, 200);
  const Point top = mm(315, 200);
  const Point bot = mm(320, 205);

  // One commit places the via, a second one, and therefore a second undo
  // entry, ends a trace on it from the other side of the board.
  BoardPnsCommit first;
  first.added.append(makeVia(viaPos, mNet));
  first.added.append(makeSegment(top, viaPos, Layer::topCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, first)));
  BI_Via* placedVia = viaAt(*mBoard, viaPos);
  ASSERT_NE(placedVia, nullptr);

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();
  const int vias = viaCount();

  BoardPnsCommit second;
  second.added.append(makeSegment(viaPos, bot, Layer::botCopper(), mNet));
  ASSERT_TRUE(
      mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, second)));

  // The via is the anchor, so the trace joined the via's net segment instead
  // of opening one, and no net point was created at the via's position.
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  EXPECT_EQ(viaCount(), vias);
  BI_NetLine* netLine = netLineBetween(*mBoard, viaPos, bot);
  ASSERT_NE(netLine, nullptr);
  EXPECT_EQ(&netLine->getLayer(), &Layer::botCopper());
  EXPECT_TRUE((&netLine->getP1() == placedVia) ||
              (&netLine->getP2() == placedVia));

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_EQ(viaCount(), vias);
  EXPECT_EQ(netLineBetween(*mBoard, viaPos, bot), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netPointCount(), points + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  netLine = netLineBetween(*mBoard, viaPos, bot);
  ASSERT_NE(netLine, nullptr);
  EXPECT_TRUE((&netLine->getP1() == viaAt(*mBoard, viaPos)) ||
              (&netLine->getP2() == viaAt(*mBoard, viaPos)));
}

TEST_F(CmdBoardApplyPnsCommitTest, testViaAddedByTheSameCommit) {
  const Point start = mm(330, 200);
  const Point via = mm(335, 200);
  const Point end = mm(335, 205);

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();
  const int vias = viaCount();

  // One via and the two traces that meet on it, on the two outer layers.
  BoardPnsCommit commit;
  commit.added.append(makeVia(via, mNet));
  commit.added.append(makeSegment(start, via, Layer::topCopper(), mNet));
  commit.added.append(makeSegment(via, end, Layer::botCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 2);
  EXPECT_EQ(viaCount(), vias + 1);
  BI_Via* placedVia = viaAt(*mBoard, via);
  ASSERT_NE(placedVia, nullptr);
  EXPECT_EQ(&placedVia->getVia().getStartLayer(), &Layer::topCopper());
  EXPECT_EQ(&placedVia->getVia().getEndLayer(), &Layer::botCopper());

  // Both traces hang off the via, so the whole thing is one net segment.
  BI_NetLine* topLine = netLineBetween(*mBoard, start, via);
  BI_NetLine* botLine = netLineBetween(*mBoard, via, end);
  ASSERT_NE(topLine, nullptr);
  ASSERT_NE(botLine, nullptr);
  EXPECT_TRUE((&topLine->getP1() == placedVia) ||
              (&topLine->getP2() == placedVia));
  EXPECT_TRUE((&botLine->getP1() == placedVia) ||
              (&botLine->getP2() == placedVia));
  EXPECT_EQ(&topLine->getNetSegment(), &botLine->getNetSegment());

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_EQ(viaCount(), vias);
  EXPECT_EQ(viaAt(*mBoard, via), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 2);
  EXPECT_EQ(viaCount(), vias + 1);
  EXPECT_NE(viaAt(*mBoard, via), nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testEndpointInNetLineInterior) {
  const Point p0 = mm(340, 200);
  const Point p1 = mm(350, 200);
  const Point split = mm(345, 200);
  const Point end = mm(345, 210);
  addTrace({p0, p1}, Layer::topCopper());

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();

  BoardPnsCommit commit;
  commit.added.append(makeSegment(split, end, Layer::topCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // The existing trace was split in two and the new one hangs off the split
  // point, so there is one T junction rather than a crossing.
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 2);
  BI_NetPoint* splitPoint = netPointAt(*mBoard, split);
  ASSERT_NE(splitPoint, nullptr);
  EXPECT_EQ(splitPoint->getNetLines().count(), 3);
  EXPECT_NE(netLineBetween(*mBoard, p0, split), nullptr);
  EXPECT_NE(netLineBetween(*mBoard, split, p1), nullptr);
  BI_NetLine* netLine = netLineBetween(*mBoard, split, end);
  ASSERT_NE(netLine, nullptr);
  EXPECT_TRUE((&netLine->getP1() == splitPoint) ||
              (&netLine->getP2() == splitPoint));
  EXPECT_EQ(netLineBetween(*mBoard, p0, p1), nullptr);

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_NE(netLineBetween(*mBoard, p0, p1), nullptr);
  EXPECT_EQ(netPointAt(*mBoard, split), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 2);
  EXPECT_EQ(netLineBetween(*mBoard, p0, p1), nullptr);
  EXPECT_NE(netLineBetween(*mBoard, split, end), nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testEndpointInFreeSpace) {
  const Point start = mm(360, 200);
  const Point end = mm(365, 205);

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();

  BoardPnsCommit commit;
  commit.added.append(makeSegment(start, end, Layer::topCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // Nothing to anchor on at either end, so both ends became net points in a
  // net segment of their own.
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 1);
  BI_NetLine* netLine = netLineBetween(*mBoard, start, end);
  ASSERT_NE(netLine, nullptr);
  EXPECT_NE(dynamic_cast<BI_NetPoint*>(&netLine->getP1()), nullptr);
  EXPECT_NE(dynamic_cast<BI_NetPoint*>(&netLine->getP2()), nullptr);
  EXPECT_EQ(netLine->getNetSegment().getNetSignal(), mNet);

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netPointCount(), points + 2);
  EXPECT_EQ(netLineCount(), lines + 1);
  EXPECT_NE(netLineBetween(*mBoard, start, end), nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testRouteWithoutNet) {
  const Point start = mm(400, 200);
  const Point end = mm(405, 205);

  const int segments = netSegmentCount();
  const int lines = netLineCount();

  // A route started in free space carries no net: the router places it on an
  // internal orphan net which has no counterpart in the circuit. Both
  // BI_NetSegment and CmdBoardNetSegmentAdd take a null net signal, so this
  // needs no special case beyond passing the null through.
  BoardPnsCommit commit;
  commit.added.append(makeSegment(start, end, Layer::topCopper(), nullptr));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  BI_NetLine* netLine = netLineBetween(*mBoard, start, end);
  ASSERT_NE(netLine, nullptr);
  EXPECT_EQ(netLine->getNetSegment().getNetSignal(), nullptr);

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netLineCount(), lines);

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments + 1);
  EXPECT_EQ(netLineCount(), lines + 1);
  ASSERT_NE(netLineBetween(*mBoard, start, end), nullptr);
  EXPECT_EQ(netLineBetween(*mBoard, start, end)->getNetSegment().getNetSignal(),
            nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testMergeTwoNetSegments) {
  const Point a0 = mm(370, 200);
  const Point a1 = mm(375, 200);
  const Point b0 = mm(380, 205);
  const Point b1 = mm(385, 205);
  addTrace({a0, a1}, Layer::topCopper());
  addTrace({b0, b1}, Layer::topCopper());

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();

  BoardPnsCommit commit;
  commit.added.append(makeSegment(a1, b0, Layer::topCopper(), mNet));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // The two net segments of one net became one, so no net point was added:
  // the stand in for the far end was dissolved into the anchor it met.
  EXPECT_EQ(netSegmentCount(), segments - 1);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines + 1);
  BI_NetLine* joining = netLineBetween(*mBoard, a1, b0);
  ASSERT_NE(joining, nullptr);
  BI_NetLine* left = netLineBetween(*mBoard, a0, a1);
  BI_NetLine* right = netLineBetween(*mBoard, b0, b1);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(&left->getNetSegment(), &joining->getNetSegment());
  EXPECT_EQ(&right->getNetSegment(), &joining->getNetSegment());

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_EQ(netLineBetween(*mBoard, a1, b0), nullptr);
  EXPECT_NE(&netLineBetween(*mBoard, a0, a1)->getNetSegment(),
            &netLineBetween(*mBoard, b0, b1)->getNetSegment());

  mUndoStack->redo();
  EXPECT_EQ(netSegmentCount(), segments - 1);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines + 1);
  EXPECT_EQ(&netLineBetween(*mBoard, a0, a1)->getNetSegment(),
            &netLineBetween(*mBoard, b0, b1)->getNetSegment());
}

TEST_F(CmdBoardApplyPnsCommitTest, testRemoveNetLine) {
  const Point p0 = mm(390, 200);
  const Point p1 = mm(395, 200);
  const Point p2 = mm(395, 205);
  addTrace({p0, p1, p2}, Layer::topCopper());

  const int segments = netSegmentCount();
  const int points = netPointCount();
  const int lines = netLineCount();
  BI_NetLine* toRemove = netLineBetween(*mBoard, p1, p2);
  ASSERT_NE(toRemove, nullptr);

  BoardPnsCommit commit;
  BoardPnsHostRef ref;
  ref.netLine = toRemove;
  commit.removed.append(ref);
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // The trace's last leg is gone and so is the net point it ended on, which
  // would otherwise be left dangling.
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points - 1);
  EXPECT_EQ(netLineCount(), lines - 1);
  EXPECT_EQ(netLineBetween(*mBoard, p1, p2), nullptr);
  EXPECT_EQ(netPointAt(*mBoard, p2), nullptr);
  EXPECT_NE(netLineBetween(*mBoard, p0, p1), nullptr);

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netPointCount(), points);
  EXPECT_EQ(netLineCount(), lines);
  EXPECT_NE(netLineBetween(*mBoard, p1, p2), nullptr);

  mUndoStack->redo();
  EXPECT_EQ(netPointCount(), points - 1);
  EXPECT_EQ(netLineCount(), lines - 1);
  EXPECT_EQ(netLineBetween(*mBoard, p1, p2), nullptr);
}

/*******************************************************************************
 *  End to End
 ******************************************************************************/

/// A pad the router accepts as a start point, as in BoardPnsRouterTest
struct RouteStart {
  const BI_Pad* pad = nullptr;
  quint64 hostId = 0;
  Point pos;
};

static BoardPnsRouter::Settings routerSettings() noexcept {
  return BoardPnsRouter::Settings{
      BoardPnsRouter::Mode::Walkaround,
      PositiveLength(250000),  // 0.25 mm trace, above the minimum.
      PositiveLength(700000),  // 0.7 mm via.
      PositiveLength(300000),  // 0.3 mm via drill.
  };
}

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

static std::optional<Point> findFreeTarget(const Board& board,
                                           const RouteStart& start) {
  foreach (const Point& target, freeSpaceCandidates(start.pos)) {
    BoardPnsRouter probe(board, routerSettings());
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

TEST_F(CmdBoardApplyPnsCommitTest, testMovedDeviceWithItsTrace) {
  BI_Pad* pad = findFreePadWithNet();
  ASSERT_NE(pad, nullptr) << "no unconnected top layer pad with a net";
  BI_Device* device = pad->getDevice();
  ASSERT_NE(device, nullptr) << "the pad belongs to no device";
  const Point devicePos = device->getPosition();
  const Point padPos = pad->getPosition();
  const Point end = padPos + mm(300, 300);

  // The trace a footprint drag is going to re-shape, anchored on the pad.
  BoardPnsCommit setup;
  setup.added.append(
      makeSegment(padPos, end, Layer::topCopper(), pad->getNetSignal()));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, setup)));
  BI_NetLine* original = netLineBetween(*mBoard, padPos, end);
  ASSERT_NE(original, nullptr);

  const int segments = netSegmentCount();
  const int lines = netLineCount();

  // What a footprint drag commits: the device moves, and the traces on its
  // pads come back with the endpoints the router computed, which are the
  // pad's new anchor positions. The device therefore has to move before
  // the endpoints are resolved, which is what this test is about.
  const Point offset = mm(1, 0);
  BoardPnsHostRef removed;
  removed.netLine = original;
  BoardPnsCommit commit;
  commit.movedDevices.append(BoardPnsMovedDevice{device, offset});
  commit.removed.append(removed);
  commit.added.append(makeSegment(padPos + offset, end, Layer::topCopper(),
                                  pad->getNetSignal()));
  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  EXPECT_EQ(device->getPosition(), devicePos + offset);
  EXPECT_EQ(pad->getPosition(), padPos + offset);
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netLineCount(), lines);
  BI_NetLine* moved = netLineBetween(*mBoard, padPos + offset, end);
  ASSERT_NE(moved, nullptr) << "the trace did not follow the pad";
  EXPECT_TRUE((&moved->getP1() == pad) || (&moved->getP2() == pad));

  mUndoStack->undo();
  EXPECT_EQ(device->getPosition(), devicePos);
  EXPECT_EQ(pad->getPosition(), padPos);
  EXPECT_EQ(netLineBetween(*mBoard, padPos + offset, end), nullptr);
  BI_NetLine* restored = netLineBetween(*mBoard, padPos, end);
  ASSERT_NE(restored, nullptr);
  EXPECT_TRUE((&restored->getP1() == pad) || (&restored->getP2() == pad));

  mUndoStack->redo();
  EXPECT_EQ(device->getPosition(), devicePos + offset);
  EXPECT_EQ(netLineCount(), lines);
  ASSERT_NE(netLineBetween(*mBoard, padPos + offset, end), nullptr);
}

TEST_F(CmdBoardApplyPnsCommitTest, testRoutedCommitFromPadIntoFreeSpace) {
  BoardPnsRouter probeRouter(*mBoard, routerSettings());
  const std::optional<RouteStart> start = findStartPad(probeRouter, *mBoard);
  ASSERT_TRUE(start.has_value()) << "no routable top layer pad with a net";
  const std::optional<Point> target = findFreeTarget(*mBoard, *start);
  ASSERT_TRUE(target.has_value()) << "no free space around the start pad";

  BoardPnsRouter router(*mBoard, routerSettings());
  ASSERT_EQ(router.startRouting(start->pos, start->hostId, Layer::topCopper()),
            BoardPnsRouter::StartResult::Ok);
  router.moveTo(*target, 0);
  ASSERT_EQ(router.fixRoute(*target, 0, true),
            BoardPnsRouter::FixOutcome::Finished);
  const BoardPnsCommit commit = router.getCommit();
  ASSERT_FALSE(commit.added.isEmpty());

  const int segments = netSegmentCount();
  const int lines = netLineCount();

  ASSERT_TRUE(mUndoStack->execCmd(new CmdBoardApplyPnsCommit(*mBoard, commit)));

  // Every routed segment became a net line, all on the pad's net. The
  // optimiser may have left corners the simplifier then collapsed, so the
  // count is a lower bound, not an equality.
  EXPECT_GT(netLineCount(), lines);
  EXPECT_LE(netLineCount(), lines + commit.added.count());
  EXPECT_GE(netSegmentCount(), segments);
  BI_NetLine* atPad = nullptr;
  foreach (BI_NetSegment* segment, mBoard->getNetSegments()) {
    foreach (BI_NetLine* netLine, segment->getNetLines()) {
      if ((&netLine->getP1() == start->pad) ||
          (&netLine->getP2() == start->pad)) {
        atPad = netLine;
      }
    }
  }
  ASSERT_NE(atPad, nullptr) << "the route did not attach to the start pad";
  EXPECT_EQ(&atPad->getLayer(), &Layer::topCopper());
  EXPECT_EQ(atPad->getNetSegment().getNetSignal(), start->pad->getNetSignal());

  mUndoStack->undo();
  EXPECT_EQ(netSegmentCount(), segments);
  EXPECT_EQ(netLineCount(), lines);

  mUndoStack->redo();
  EXPECT_GT(netLineCount(), lines);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
