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
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/workspace/colorrole.h>
#include <librepcb/editor/graphics/graphicslayerlist.h>
#include <librepcb/editor/project/board/boardgraphicsscene.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_device.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_hole.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_netline.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_pad.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_polygon.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_stroketext.h>
#include <librepcb/editor/project/board/graphicsitems/bgi_zone.h>
#include <librepcb/editor/project/projectcrossprobe.h>

#include <QtCore>
#include <QtWidgets>

#include <memory>
#include <optional>
#include <typeinfo>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static QPainterPath sceneShape(const QGraphicsItem& item) noexcept {
  return item.mapToScene(item.shape());
}

static QRectF sceneShapeRect(const QGraphicsItem& item) noexcept {
  return sceneShape(item).controlPointRect();
}

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

/**
 * @brief Rectangle selection in a real board graphics scene
 *
 * Devices, pads and stroke texts are item groups whose children are not
 * added through `QGraphicsItemGroup::addToGroup()`, so their bounding rect
 * is empty although their shape is not. These tests make sure the rectangle
 * selection still finds them.
 */
class BoardGraphicsSceneTest : public ::testing::Test {
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

  void selectRect(const QRectF& rect) noexcept {
    mScene->selectItemsInRect(Point::fromPx(rect.topLeft()),
                              Point::fromPx(rect.bottomRight()));
  }

  std::shared_ptr<BGI_Device> firstDevice() const noexcept {
    for (auto device : mScene->getDevices()) {
      return device;
    }
    return nullptr;
  }

  QList<std::shared_ptr<BGI_Pad>> padsOf(
      const std::shared_ptr<BGI_Device>& device) const noexcept {
    QList<std::shared_ptr<BGI_Pad>> pads;
    for (auto pad : mScene->getPads()) {
      if (pad->getDeviceGraphicsItem().lock() == device) {
        pads.append(pad);
      }
    }
    return pads;
  }

  /// A net line whose shape stays clear of every device and pad.
  std::shared_ptr<BGI_NetLine> netLineAwayFromDevices() const noexcept {
    for (auto netLine : mScene->getNetLines()) {
      const QRectF rect = sceneShapeRect(*netLine);
      bool clear = true;
      for (auto device : mScene->getDevices()) {
        clear = clear && (!sceneShape(*device).intersects(rect));
      }
      for (auto pad : mScene->getPads()) {
        clear = clear && (!sceneShape(*pad).intersects(rect));
      }
      if (clear) {
        return netLine;
      }
    }
    return nullptr;
  }

  /// A small rect around a point inside `inside` and outside all of `outside`.
  static std::optional<QRectF> probeRect(
      const QPainterPath& inside,
      const QList<QPainterPath>& outside) noexcept {
    const QRectF bounds = inside.boundingRect();
    const qreal step = Length(50000).toPx();
    const qreal half = Length(10000).toPx();
    for (qreal x = bounds.left(); x <= bounds.right(); x += step) {
      for (qreal y = bounds.top(); y <= bounds.bottom(); y += step) {
        const QRectF rect(x - half, y - half, 2 * half, 2 * half);
        bool clear = inside.contains(rect);
        for (const QPainterPath& path : outside) {
          clear = clear && (!path.intersects(rect));
        }
        if (clear) {
          return rect;
        }
      }
    }
    return std::nullopt;
  }

  bool anyNetLineSelected() const noexcept {
    for (auto netLine : mScene->getNetLines()) {
      if (netLine->isSelected()) {
        return true;
      }
    }
    return false;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(BoardGraphicsSceneTest, testRectOverDeviceAndNetLineSelectsBoth) {
  std::shared_ptr<BGI_Device> device = firstDevice();
  ASSERT_TRUE(device);
  std::shared_ptr<BGI_NetLine> netLine = netLineAwayFromDevices();
  ASSERT_TRUE(netLine);

  QRectF rect = sceneShapeRect(*device) | sceneShapeRect(*netLine);
  for (auto pad : padsOf(device)) {
    rect |= sceneShapeRect(*pad);
  }
  selectRect(rect);

  EXPECT_TRUE(device->isSelected());
  EXPECT_TRUE(netLine->isSelected());
}

TEST_F(BoardGraphicsSceneTest, testRectOverDeviceBodySelectsDevice) {
  // The grab area is hidden by default, which leaves only the origin cross.
  mLayers->get(ColorRole::boardGrabAreasTop())->setVisible(true);
  mLayers->get(ColorRole::boardGrabAreasBot())->setVisible(true);
  std::shared_ptr<BGI_Device> device = firstDevice();
  ASSERT_TRUE(device);
  QList<QPainterPath> outside;
  for (auto pad : padsOf(device)) {
    outside.append(sceneShape(*pad));
  }
  // Keep clear of the origin cross, so the grab area alone is hit.
  QPainterPath originCross;
  const qreal radius = Length(1000000).toPx();
  originCross.addEllipse(device->scenePos(), radius, radius);
  outside.append(originCross);
  const std::optional<QRectF> rect = probeRect(sceneShape(*device), outside);
  ASSERT_TRUE(rect.has_value());

  selectRect(*rect);

  EXPECT_TRUE(device->isSelected());
  EXPECT_FALSE(anyNetLineSelected());
}

TEST_F(BoardGraphicsSceneTest, testRectOverPadSelectsDevice) {
  std::shared_ptr<BGI_Device> device = firstDevice();
  ASSERT_TRUE(device);
  QList<std::shared_ptr<BGI_Pad>> pads = padsOf(device);
  ASSERT_FALSE(pads.isEmpty());
  QList<QPainterPath> outside;
  outside.append(sceneShape(*device));
  for (auto netLine : mScene->getNetLines()) {
    outside.append(sceneShape(*netLine));
  }
  const std::optional<QRectF> rect = probeRect(sceneShape(*pads.first()),
                                               outside);
  ASSERT_TRUE(rect.has_value());

  selectRect(*rect);

  EXPECT_TRUE(device->isSelected());
  EXPECT_FALSE(anyNetLineSelected());
}

TEST_F(BoardGraphicsSceneTest, testRectOverStrokeTextSelectsIt) {
  QList<QPainterPath> outside;
  for (auto device : mScene->getDevices()) {
    outside.append(sceneShape(*device));
  }
  for (auto pad : mScene->getPads()) {
    outside.append(sceneShape(*pad));
  }
  for (auto text : mScene->getStrokeTexts()) {
    if (const std::optional<QRectF> rect =
            probeRect(sceneShape(*text), outside)) {
      selectRect(*rect);
      EXPECT_TRUE(text->isSelected());
      return;
    }
  }
  FAIL() << "No stroke text clear of devices and pads.";
}

TEST_F(BoardGraphicsSceneTest, testRectOverBoardSelectsEveryItemGroup) {
  QList<QGraphicsItem*> items;
  for (auto item : mScene->getDevices()) {
    items.append(item.get());
  }
  for (auto item : mScene->getPolygons()) {
    items.append(item.get());
  }
  for (auto item : mScene->getStrokeTexts()) {
    items.append(item.get());
  }
  for (auto item : mScene->getHoles()) {
    items.append(item.get());
  }
  for (auto item : mScene->getZones()) {
    items.append(item.get());
  }
  QRectF rect;
  for (QGraphicsItem* item : items) {
    rect |= sceneShapeRect(*item);
  }
  selectRect(rect);

  int checked = 0;
  for (QGraphicsItem* item : items) {
    if (item->isVisible() && (!item->shape().isEmpty())) {
      EXPECT_TRUE(item->isSelected()) << typeid(*item).name();
      ++checked;
    }
  }
  EXPECT_GT(checked, 0);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
