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
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/editor/graphics/graphicslayerlist.h>
#include <librepcb/editor/project/board/boardgraphicsscene.h>
#include <librepcb/editor/project/board/boardpnspreview.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_netline.h>
#include <librepcb/editor/project/projectcrossprobe.h>

#include <QtCore>

#include <memory>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static BoardPnsPreviewItem previewItem(BoardPnsPreviewStyle style,
                                       const Layer& layer) noexcept {
  BoardPnsPreviewItem item;
  item.path.append(Point(0, 0));
  item.path.append(Point(1000000, 0));
  item.width = PositiveLength(200000);
  item.layer = &layer;
  item.style = style;
  return item;
}

static BoardPnsPreviewVia previewVia() noexcept {
  BoardPnsPreviewVia via;
  via.position = Point(1000000, 0);
  via.diameter = PositiveLength(700000);
  via.drill = PositiveLength(300000);
  via.startLayer = &Layer::topCopper();
  via.endLayer = &Layer::botCopper();
  return via;
}

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

/**
 * @brief Renders hand built frames into a real board graphics scene
 *
 * The frames are hand built rather than routed, because what is under test is
 * the renderer and not the router. The project is `tests/data/projects/Gerber
 * Test`, the same one the other router tests load, for its traces.
 */
class BoardPnsPreviewTest : public ::testing::Test {
protected:
  std::unique_ptr<Project> mProject;
  Board* mBoard = nullptr;
  std::unique_ptr<GraphicsLayerList> mLayers;
  std::shared_ptr<ProjectCrossProbe> mCrossProbe;
  std::shared_ptr<BoardGraphicsScene::Context> mSceneContext;
  std::unique_ptr<BoardGraphicsScene> mScene;

  void SetUp() override {
    const FilePath fp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
    std::shared_ptr<TransactionalFileSystem> fs =
        TransactionalFileSystem::openRO(fp.getParentDir());
    ProjectLoader loader;
    mProject = loader.open(std::make_unique<TransactionalDirectory>(fs),
                           fp.getFilename());  // can throw
    ASSERT_FALSE(mProject->getBoards().isEmpty());
    mBoard = mProject->getBoards().first();

    mLayers = GraphicsLayerList::boardLayers(nullptr);
    mCrossProbe = std::make_shared<ProjectCrossProbe>();
    mSceneContext = std::make_shared<BoardGraphicsScene::Context>(
        BoardGraphicsScene::Context{nullptr, mCrossProbe,
                                    GraphicsLayer::State::Enabled, false});
    mScene.reset(new BoardGraphicsScene(*mBoard, *mLayers, mSceneContext));
  }

  void TearDown() override {
    mScene.reset();
    mSceneContext.reset();
    mCrossProbe.reset();
    mLayers.reset();
    mProject.reset();
  }

  BI_NetLine* firstNetLine() const noexcept {
    foreach (BI_NetSegment* segment, mBoard->getNetSegments()) {
      foreach (BI_NetLine* netLine, segment->getNetLines()) {
        return netLine;
      }
    }
    return nullptr;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(BoardPnsPreviewTest, testFrameIsDrawnFromPooledItems) {
  BoardPnsPreviewItems preview(*mScene, *mLayers);
  EXPECT_EQ(0, preview.getPathItemPoolSize());
  EXPECT_EQ(0, preview.getCircleItemPoolSize());

  BoardPnsPreview frame;
  frame.items.append(
      previewItem(BoardPnsPreviewStyle::Head, Layer::topCopper()));
  frame.items.append(
      previewItem(BoardPnsPreviewStyle::Collision, Layer::topCopper()));
  frame.via = previewVia();
  preview.update(frame);
  EXPECT_EQ(2, preview.getUsedPathItemCount());
  EXPECT_EQ(1, preview.getUsedCircleItemCount());
  EXPECT_EQ(2, preview.getPathItemPoolSize());
  EXPECT_EQ(1, preview.getCircleItemPoolSize());

  // A smaller frame parks the surplus items instead of deleting them.
  BoardPnsPreview smallerFrame;
  smallerFrame.items.append(
      previewItem(BoardPnsPreviewStyle::Tail, Layer::botCopper()));
  preview.update(smallerFrame);
  EXPECT_EQ(1, preview.getUsedPathItemCount());
  EXPECT_EQ(0, preview.getUsedCircleItemCount());
  EXPECT_EQ(2, preview.getPathItemPoolSize());
  EXPECT_EQ(1, preview.getCircleItemPoolSize());

  // And so does an empty frame.
  preview.update(BoardPnsPreview());
  EXPECT_EQ(0, preview.getUsedPathItemCount());
  EXPECT_EQ(2, preview.getPathItemPoolSize());
}

TEST_F(BoardPnsPreviewTest, testRatlineTakesOnePathItem) {
  BoardPnsPreviewItems preview(*mScene, *mLayers);

  BoardPnsPreview frame;
  frame.items.append(
      previewItem(BoardPnsPreviewStyle::Head, Layer::topCopper()));
  frame.ratline.append(Point(1000000, 0));
  frame.ratline.append(Point(3000000, 2000000));
  preview.update(frame);
  EXPECT_EQ(2, preview.getUsedPathItemCount());

  // A rat line of a single point cannot be stroked and takes no item.
  frame.ratline.removeLast();
  preview.update(frame);
  EXPECT_EQ(1, preview.getUsedPathItemCount());
}

TEST_F(BoardPnsPreviewTest, testHiddenBoardItemsAreRestored) {
  BI_NetLine* netLine = firstNetLine();
  ASSERT_NE(nullptr, netLine);
  std::shared_ptr<BGI_NetLine> graphicsItem =
      mScene->getNetLines().value(netLine);
  ASSERT_NE(nullptr, graphicsItem.get());
  EXPECT_DOUBLE_EQ(1.0, graphicsItem->opacity());

  BoardPnsHostRef ref;
  ref.netLine = netLine;
  BoardPnsPreview frame;
  frame.hidden.append(ref);

  BoardPnsPreviewItems preview(*mScene, *mLayers);
  preview.update(frame);
  EXPECT_EQ(1, preview.getHiddenBoardItemCount());
  EXPECT_DOUBLE_EQ(0.0, graphicsItem->opacity());

  // The first frame which does not name it draws it again.
  preview.update(BoardPnsPreview());
  EXPECT_EQ(0, preview.getHiddenBoardItemCount());
  EXPECT_DOUBLE_EQ(1.0, graphicsItem->opacity());

  // A violation which hides the original is the same case.
  BoardPnsViolation violation;
  violation.host = ref;
  violation.hideOriginal = true;
  BoardPnsPreview violationFrame;
  violationFrame.violations.append(violation);
  preview.update(violationFrame);
  EXPECT_EQ(1, preview.getHiddenBoardItemCount());
  EXPECT_DOUBLE_EQ(0.0, graphicsItem->opacity());

  // A violation which does not hide the original leaves it alone.
  violationFrame.violations.first().hideOriginal = false;
  preview.update(violationFrame);
  EXPECT_EQ(0, preview.getHiddenBoardItemCount());
  EXPECT_DOUBLE_EQ(1.0, graphicsItem->opacity());

  // Ending the session must not leave anything hidden.
  preview.update(frame);
  EXPECT_DOUBLE_EQ(0.0, graphicsItem->opacity());
  preview.clear();
  EXPECT_EQ(0, preview.getHiddenBoardItemCount());
  EXPECT_DOUBLE_EQ(1.0, graphicsItem->opacity());
}

TEST_F(BoardPnsPreviewTest, testDestructorRestoresHiddenBoardItems) {
  BI_NetLine* netLine = firstNetLine();
  ASSERT_NE(nullptr, netLine);
  std::shared_ptr<BGI_NetLine> graphicsItem =
      mScene->getNetLines().value(netLine);
  ASSERT_NE(nullptr, graphicsItem.get());

  BoardPnsHostRef ref;
  ref.netLine = netLine;
  BoardPnsPreview frame;
  frame.hidden.append(ref);

  {
    BoardPnsPreviewItems preview(*mScene, *mLayers);
    preview.update(frame);
    EXPECT_DOUBLE_EQ(0.0, graphicsItem->opacity());
  }
  EXPECT_DOUBLE_EQ(1.0, graphicsItem->opacity());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
