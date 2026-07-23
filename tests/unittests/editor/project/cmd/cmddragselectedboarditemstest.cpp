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
#include <librepcb/core/exceptions.h>
#include <librepcb/core/fileio/filepath.h>
#include <librepcb/core/fileio/transactionaldirectory.h>
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_hole.h>
#include <librepcb/core/project/board/items/bi_stroketext.h>
#include <librepcb/core/project/board/items/bi_via.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/editor/graphics/graphicslayerlist.h>
#include <librepcb/editor/project/board/boardgraphicsscene.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_device.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_hole.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_via.h>
#include <librepcb/editor/project/cmd/cmddragselectedboarditems.h>
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
 *  Test Class
 ******************************************************************************/

class CmdDragSelectedBoardItemsTest : public ::testing::Test {
protected:
  std::unique_ptr<Project> mProject;
  Board* mBoard = nullptr;
  std::unique_ptr<GraphicsLayerList> mLayers;
  std::unique_ptr<BoardGraphicsScene> mScene;

  void SetUp() override {
    FilePath projectFp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
    std::shared_ptr<TransactionalFileSystem> projectFs =
        TransactionalFileSystem::openRO(projectFp.getParentDir());
    ProjectLoader loader;
    mProject = loader.open(std::make_unique<TransactionalDirectory>(projectFs),
                           projectFp.getFilename());  // can throw
    mBoard = mProject->getBoards().first();
    mLayers = GraphicsLayerList::boardLayers(nullptr);
    auto context = std::make_shared<BoardGraphicsScene::Context>();
    context->crossProbe = std::make_shared<ProjectCrossProbe>();
    mScene.reset(new BoardGraphicsScene(*mBoard, *mLayers, context));
  }

  void selectAllDevices() {
    for (auto it = mScene->getDevices().begin();
         it != mScene->getDevices().end(); it++) {
      it.value()->setSelected(true);
    }
  }

  void selectAllVias() {
    for (auto it = mScene->getVias().begin(); it != mScene->getVias().end();
         it++) {
      it.value()->setSelected(true);
    }
  }

  void selectAllHoles() {
    for (auto it = mScene->getHoles().begin(); it != mScene->getHoles().end();
         it++) {
      it.value()->setSelected(true);
    }
  }

  static QList<Point> shiftedBy(const QList<Point>& positions,
                                const Point& delta) {
    QList<Point> result;
    foreach (const Point& position, positions) {
      result.append(position + delta);
    }
    return result;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(CmdDragSelectedBoardItemsTest, testGetPositionsOfSelectedDevices) {
  selectAllDevices();
  CmdDragSelectedBoardItems cmd(*mScene, true);

  QSet<Point> expected;
  foreach (BI_Device* device, mBoard->getDeviceInstances()) {
    expected.insert(device->getPosition());
  }
  ASSERT_FALSE(expected.isEmpty());
  EXPECT_EQ(mBoard->getDeviceInstances().count(), cmd.getPositions().count());
  foreach (const Point& position, cmd.getPositions()) {
    EXPECT_TRUE(expected.contains(position));
  }
}

TEST_F(CmdDragSelectedBoardItemsTest, testSetNewPositionsOfDevices) {
  selectAllDevices();
  const Point delta(1000000, -2000000);  // 1mm / -2mm

  // Memorize original positions of all devices and their stroke texts.
  QHash<BI_Device*, Point> oldDevicePositions;
  QHash<BI_StrokeText*, Point> oldTextPositions;
  foreach (BI_Device* device, mBoard->getDeviceInstances()) {
    oldDevicePositions.insert(device, device->getPosition());
    foreach (BI_StrokeText* text, device->getStrokeTexts()) {
      oldTextPositions.insert(text, text->getData().getPosition());
    }
  }
  ASSERT_FALSE(oldDevicePositions.isEmpty());
  ASSERT_FALSE(oldTextPositions.isEmpty());

  std::unique_ptr<CmdDragSelectedBoardItems> cmd(
      new CmdDragSelectedBoardItems(*mScene, true));
  cmd->setNewPositions(shiftedBy(cmd->getPositions(), delta));
  EXPECT_TRUE(cmd->execute());

  // Devices are moved to the new positions and their texts are moved by the
  // same delta to keep their relative position to the device.
  for (auto it = oldDevicePositions.begin(); it != oldDevicePositions.end();
       it++) {
    EXPECT_EQ(it.value() + delta, it.key()->getPosition());
  }
  for (auto it = oldTextPositions.begin(); it != oldTextPositions.end(); it++) {
    EXPECT_EQ(it.value() + delta, it.key()->getData().getPosition());
  }

  // Undo restores the original positions.
  cmd->undo();
  for (auto it = oldDevicePositions.begin(); it != oldDevicePositions.end();
       it++) {
    EXPECT_EQ(it.value(), it.key()->getPosition());
  }
  for (auto it = oldTextPositions.begin(); it != oldTextPositions.end(); it++) {
    EXPECT_EQ(it.value(), it.key()->getData().getPosition());
  }
}

TEST_F(CmdDragSelectedBoardItemsTest, testSetNewPositionsOfVias) {
  selectAllVias();
  const Point delta(-500000, 250000);

  QHash<BI_Via*, Point> oldPositions;
  for (auto it = mScene->getVias().begin(); it != mScene->getVias().end();
       it++) {
    oldPositions.insert(it.key(), it.key()->getPosition());
  }
  ASSERT_FALSE(oldPositions.isEmpty());

  std::unique_ptr<CmdDragSelectedBoardItems> cmd(
      new CmdDragSelectedBoardItems(*mScene, true));
  EXPECT_EQ(oldPositions.count(), cmd->getPositions().count());
  cmd->setNewPositions(shiftedBy(cmd->getPositions(), delta));
  EXPECT_TRUE(cmd->execute());

  for (auto it = oldPositions.begin(); it != oldPositions.end(); it++) {
    EXPECT_EQ(it.value() + delta, it.key()->getPosition());
  }
}

TEST_F(CmdDragSelectedBoardItemsTest, testSetNewPositionsOfHoles) {
  selectAllHoles();
  const Point delta(750000, 1250000);

  QHash<BI_Hole*, Point> oldPositions;
  for (auto it = mScene->getHoles().begin(); it != mScene->getHoles().end();
       it++) {
    oldPositions.insert(
        it.key(),
        it.key()->getData().getPath()->getVertices().first().getPos());
  }
  ASSERT_FALSE(oldPositions.isEmpty());

  std::unique_ptr<CmdDragSelectedBoardItems> cmd(
      new CmdDragSelectedBoardItems(*mScene, true));
  EXPECT_EQ(oldPositions.count(), cmd->getPositions().count());
  cmd->setNewPositions(shiftedBy(cmd->getPositions(), delta));
  EXPECT_TRUE(cmd->execute());

  for (auto it = oldPositions.begin(); it != oldPositions.end(); it++) {
    EXPECT_EQ(it.value() + delta,
              it.key()->getData().getPath()->getVertices().first().getPos());
  }
}

TEST_F(CmdDragSelectedBoardItemsTest, testDestroyWithoutExecuteRevertsPreview) {
  selectAllDevices();
  const Point delta(1000000, 1000000);

  QHash<BI_Device*, Point> oldPositions;
  foreach (BI_Device* device, mBoard->getDeviceInstances()) {
    oldPositions.insert(device, device->getPosition());
  }
  ASSERT_FALSE(oldPositions.isEmpty());

  {
    CmdDragSelectedBoardItems cmd(*mScene, true);
    cmd.setNewPositions(shiftedBy(cmd.getPositions(), delta));

    // The new positions are applied immediately (live preview).
    for (auto it = oldPositions.begin(); it != oldPositions.end(); it++) {
      EXPECT_EQ(it.value() + delta, it.key()->getPosition());
    }
  }

  // Destroying the unexecuted command reverts the preview.
  for (auto it = oldPositions.begin(); it != oldPositions.end(); it++) {
    EXPECT_EQ(it.value(), it.key()->getPosition());
  }
}

TEST_F(CmdDragSelectedBoardItemsTest, testSetNewPositionsCountMismatchThrows) {
  selectAllDevices();
  CmdDragSelectedBoardItems cmd(*mScene, true);
  ASSERT_FALSE(cmd.getPositions().isEmpty());

  EXPECT_THROW(cmd.setNewPositions(QList<Point>()), LogicError);

  QList<Point> tooMany = cmd.getPositions();
  tooMany.append(Point(0, 0));
  EXPECT_THROW(cmd.setNewPositions(tooMany), LogicError);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
