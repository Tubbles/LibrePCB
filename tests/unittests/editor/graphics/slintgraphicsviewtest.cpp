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
#include <librepcb/editor/graphics/graphicsscene.h>
#include <librepcb/editor/graphics/slintgraphicsview.h>

#include <QtCore>
#include <QtTest>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class SlintGraphicsViewTest : public ::testing::Test {
protected:
  static constexpr qreal sWidth = 400;
  static constexpr qreal sHeight = 300;

  static QPointF viewCenterInScene(const SlintGraphicsView& view) {
    return view.mapToScenePosPx(QPointF(sWidth / 2, sHeight / 2), 1);
  }

  static qreal scale(const SlintGraphicsView& view) {
    const QPointF p0 = view.mapToScenePosPx(QPointF(0, 0), 1);
    const QPointF p1 = view.mapToScenePosPx(QPointF(100, 0), 1);
    return 100 / (p1.x() - p0.x());
  }

  static void waitUntilCenteredOn(const SlintGraphicsView& view,
                                  const QPointF& pos) {
    QDeadlineTimer deadline(3000);
    while ((QLineF(viewCenterInScene(view), pos).length() > 0.001) &&
           (!deadline.hasExpired())) {
      QTest::qWait(10);
    }
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(SlintGraphicsViewTest, testPanToScenePointKeepsScale) {
  SlintGraphicsView view(SlintGraphicsView::defaultBoardSceneRect(),
                         SlintGraphicsView::defaultMargins());
  GraphicsScene scene;
  view.render(scene, sWidth, sHeight);  // Fits the default scene rect.
  const qreal scaleBefore = scale(view);

  // A point far outside of the current view.
  const QPointF target(12345, -6789);
  view.panToScenePoint(target);
  waitUntilCenteredOn(view, target);

  EXPECT_NEAR(target.x(), viewCenterInScene(view).x(), 0.001);
  EXPECT_NEAR(target.y(), viewCenterInScene(view).y(), 0.001);
  EXPECT_NEAR(scaleBefore, scale(view), 1e-9);
}

TEST_F(SlintGraphicsViewTest, testPanToScenePointDuringZoomKeepsTargetScale) {
  const QRectF zoomRect(-50, -50, 20, 10);

  // Reference: The scale reached by zooming only.
  SlintGraphicsView reference(SlintGraphicsView::defaultBoardSceneRect(),
                              SlintGraphicsView::defaultMargins());
  GraphicsScene scene;
  reference.render(scene, sWidth, sHeight);
  reference.zoomToSceneRect(zoomRect, false);
  waitUntilCenteredOn(reference, zoomRect.center());
  const qreal zoomScale = scale(reference);

  // Panning while the zoom animation is running.
  SlintGraphicsView view(SlintGraphicsView::defaultBoardSceneRect(),
                         SlintGraphicsView::defaultMargins());
  view.render(scene, sWidth, sHeight);
  view.zoomToSceneRect(zoomRect, false);
  const QPointF target(100, 200);
  view.panToScenePoint(target);
  waitUntilCenteredOn(view, target);

  EXPECT_NEAR(target.x(), viewCenterInScene(view).x(), 0.001);
  EXPECT_NEAR(target.y(), viewCenterInScene(view).y(), 0.001);
  EXPECT_NEAR(zoomScale, scale(view), 1e-9);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
