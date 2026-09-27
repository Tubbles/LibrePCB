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
#include <librepcb/editor/3d/slintopenglview.h>

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

// The view is never rendered, so it works on a 1x1 normalized view and the
// OpenGL context is irrelevant for these tests.
class SlintOpenGlViewTest : public ::testing::Test {
protected:
  void move(const QPointF& pos, bool alt, bool control, bool shift) noexcept {
    mView.pointerEvent(pos,
                       PointerEvent{PointerEventButton::Other,
                                    PointerEventKind::Move,
                                    KeyboardModifiers{alt, control, shift,
                                                      false},
                                    0});
  }

  SlintOpenGlView mView;
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(SlintOpenGlViewTest, testTouchpadShiftMotionPans) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  const OpenGlProjection initial = mView.getProjection();
  move(QPointF(0.1, 0.1), false, false, true);  // Only records.
  EXPECT_EQ(initial, mView.getProjection());
  move(QPointF(0.3, 0.2), false, false, true);
  const OpenGlProjection panned = mView.getProjection();
  EXPECT_NE(initial.center, panned.center);
  EXPECT_EQ(initial.fov, panned.fov);
  EXPECT_EQ(initial.transform, panned.transform);
}

TEST_F(SlintOpenGlViewTest, testTouchpadCtrlShiftMotionZooms) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  const OpenGlProjection initial = mView.getProjection();
  move(QPointF(0.5, 200), false, true, true);  // Only records.
  EXPECT_EQ(initial, mView.getProjection());
  move(QPointF(0.5, 100), false, true, true);  // 100 px up doubles the zoom.
  EXPECT_NEAR(initial.fov / 2, mView.getProjection().fov, 1e-9);
  EXPECT_EQ(initial.transform, mView.getProjection().transform);
}

TEST_F(SlintOpenGlViewTest, testTouchpadAltMotionRotates) {
  mView.setNavigationStyle(NavigationStyle::Touchpad);
  const OpenGlProjection initial = mView.getProjection();
  move(QPointF(0.1, 0.1), true, false, false);  // Only records.
  EXPECT_EQ(initial, mView.getProjection());
  move(QPointF(0.2, 0.1), true, false, false);
  EXPECT_EQ(initial.center, mView.getProjection().center);
  EXPECT_EQ(initial.fov, mView.getProjection().fov);
  EXPECT_NE(initial.transform, mView.getProjection().transform);
}

// Two small rotation steps equal one left button drag over the same path.
TEST_F(SlintOpenGlViewTest, testTouchpadRotationMatchesLeftButtonDrag) {
  SlintOpenGlView dragged;
  dragged.pointerEvent(
      QPointF(0.1, 0.1),
      PointerEvent{PointerEventButton::Left, PointerEventKind::Down,
                   KeyboardModifiers{}, 0});
  dragged.pointerEvent(
      QPointF(0.4, 0.1),
      PointerEvent{PointerEventButton::Other, PointerEventKind::Move,
                   KeyboardModifiers{}, 0});

  mView.setNavigationStyle(NavigationStyle::Touchpad);
  move(QPointF(0.1, 0.1), true, false, false);
  move(QPointF(0.2, 0.1), true, false, false);
  move(QPointF(0.4, 0.1), true, false, false);

  const QMatrix4x4 expected = dragged.getProjection().transform;
  const QMatrix4x4 actual = mView.getProjection().transform;
  for (int i = 0; i < 16; ++i) {
    EXPECT_NEAR(expected.constData()[i], actual.constData()[i], 1e-5) << i;
  }
}

TEST_F(SlintOpenGlViewTest, testDefaultStyleIgnoresModifiers) {
  const OpenGlProjection initial = mView.getProjection();
  move(QPointF(0.1, 0.1), true, false, true);
  move(QPointF(0.3, 0.2), true, false, true);
  move(QPointF(0.3, 0.2), false, true, true);
  move(QPointF(0.3, 0.1), false, true, true);
  EXPECT_EQ(initial, mView.getProjection());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
