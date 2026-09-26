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
#include <librepcb/editor/widgets/if_graphicsvieweventhandler.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

using slint::private_api::KeyboardModifiers;
using slint::private_api::PointerEvent;
using slint::private_api::PointerEventButton;
using slint::private_api::PointerEventKind;

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class SlintGraphicsViewTest : public ::testing::Test,
                              public IF_GraphicsViewEventHandler {
protected:
  SlintGraphicsViewTest()
    : mView(QRectF(-100, -100, 200, 200), QMarginsF()), mMovesForwarded(0) {
    mView.setEventHandler(this);
    // Rendering sets the view size and fits the empty scene's default rect,
    // giving a scale of 1 with the scene origin in the view center.
    mView.render(mScene, 200, 200);
  }

  bool graphicsSceneMouseMoved(
      const GraphicsSceneMouseEvent& e) noexcept override {
    Q_UNUSED(e);
    ++mMovesForwarded;
    return true;
  }

  void move(const QPointF& pos, bool alt, bool control, bool shift) noexcept {
    mView.pointerEvent(pos,
                       PointerEvent{PointerEventButton::Other,
                                    PointerEventKind::Move,
                                    KeyboardModifiers{alt, control, shift,
                                                      false},
                                    0});
  }

  QPointF sceneAt(const QPointF& pos) const noexcept {
    return mView.mapToScenePosPx(pos, 1);
  }

  // Screen pixels per scene pixel.
  qreal scale() const noexcept {
    return 100 / (sceneAt(QPointF(100, 0)).x() - sceneAt(QPointF(0, 0)).x());
  }

  GraphicsScene mScene;
  SlintGraphicsView mView;
  int mMovesForwarded;
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(SlintGraphicsViewTest, testInitialProjection) {
  EXPECT_EQ(QPointF(0, 0), sceneAt(QPointF(100, 100)));
  EXPECT_NEAR(1, scale(), 1e-9);
}

TEST_F(SlintGraphicsViewTest, testTouchpadShiftMotionPans) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  move(QPointF(10, 10), false, false, false);
  EXPECT_EQ(1, mMovesForwarded);

  // The first move with Shift held only records the position.
  const QPointF grabbed = sceneAt(QPointF(50, 50));
  move(QPointF(50, 50), false, false, true);
  EXPECT_EQ(grabbed, sceneAt(QPointF(50, 50)));

  // Further moves drag the scene along, and the tool sees none of them.
  move(QPointF(80, 70), false, false, true);
  EXPECT_EQ(grabbed, sceneAt(QPointF(80, 70)));
  move(QPointF(120, 60), true, false, true);  // Alt does not matter.
  EXPECT_EQ(grabbed, sceneAt(QPointF(120, 60)));
  EXPECT_NEAR(1, scale(), 1e-9);
  EXPECT_EQ(1, mMovesForwarded);

  // Auto fit is off, so a resize keeps the projection.
  mView.render(mScene, 300, 300);
  EXPECT_EQ(grabbed, sceneAt(QPointF(120, 60)));

  // Releasing Shift ends the gesture, moves reach the tool again.
  move(QPointF(130, 60), false, false, false);
  EXPECT_EQ(grabbed, sceneAt(QPointF(120, 60)));
  EXPECT_EQ(2, mMovesForwarded);
}

TEST_F(SlintGraphicsViewTest, testTouchpadCtrlShiftMotionZooms) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  move(QPointF(100, 150), false, true, true);
  EXPECT_NEAR(1, scale(), 1e-9);

  // 100 px upwards doubles the scale, about the pointer.
  const QPointF anchor = sceneAt(QPointF(100, 50));
  move(QPointF(100, 50), false, true, true);
  EXPECT_NEAR(2, scale(), 1e-9);
  EXPECT_NEAR(anchor.x(), sceneAt(QPointF(100, 50)).x(), 1e-9);
  EXPECT_NEAR(anchor.y(), sceneAt(QPointF(100, 50)).y(), 1e-9);

  // 50 px downwards zooms out by the square root of two.
  move(QPointF(100, 100), false, true, true);
  EXPECT_NEAR(std::sqrt(2), scale(), 1e-9);
  EXPECT_EQ(0, mMovesForwarded);

  // Auto fit is off, so a resize keeps the scale.
  mView.render(mScene, 300, 300);
  EXPECT_NEAR(std::sqrt(2), scale(), 1e-9);
}

TEST_F(SlintGraphicsViewTest, testTouchpadSwitchingGestureRestarts) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  move(QPointF(50, 50), false, false, true);
  move(QPointF(60, 50), false, false, true);
  const QPointF before = sceneAt(QPointF(0, 0));

  // Adding Ctrl starts a zoom, whose first move only records the position.
  move(QPointF(60, 10), false, true, true);
  EXPECT_EQ(before, sceneAt(QPointF(0, 0)));
  EXPECT_NEAR(1, scale(), 1e-9);
}

// Alt is the snap override of the tools in the touchpad style.
TEST_F(SlintGraphicsViewTest, testTouchpadAltMotionReachesTool) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  const QPointF before = sceneAt(QPointF(0, 0));
  move(QPointF(50, 50), true, false, false);
  move(QPointF(80, 70), true, false, false);
  move(QPointF(80, 20), true, true, false);
  EXPECT_EQ(before, sceneAt(QPointF(0, 0)));
  EXPECT_NEAR(1, scale(), 1e-9);
  EXPECT_EQ(3, mMovesForwarded);
}

TEST_F(SlintGraphicsViewTest, testTouchpadGesturesNeedNoButton) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  mView.pointerEvent(
      QPointF(50, 50),
      PointerEvent{PointerEventButton::Left, PointerEventKind::Down,
                   KeyboardModifiers{false, false, true, false}, 0});
  const QPointF before = sceneAt(QPointF(0, 0));
  move(QPointF(50, 50), false, false, true);
  move(QPointF(80, 70), false, false, true);
  EXPECT_EQ(before, sceneAt(QPointF(0, 0)));
  EXPECT_EQ(2, mMovesForwarded);
}

TEST_F(SlintGraphicsViewTest, testDefaultStyleIgnoresModifiers) {
  const QPointF before = sceneAt(QPointF(0, 0));
  move(QPointF(50, 50), false, false, true);
  move(QPointF(80, 70), false, false, true);
  move(QPointF(80, 70), false, true, true);
  move(QPointF(80, 20), false, true, true);
  EXPECT_EQ(before, sceneAt(QPointF(0, 0)));
  EXPECT_NEAR(1, scale(), 1e-9);
  EXPECT_EQ(4, mMovesForwarded);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
