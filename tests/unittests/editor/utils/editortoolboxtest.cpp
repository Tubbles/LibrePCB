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
#include <librepcb/editor/utils/editortoolbox.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class EditorToolboxTest : public ::testing::Test {
protected:
  ~EditorToolboxTest() override {
    // The style is process wide, do not leak it into other tests.
    EditorToolbox::setNavigationStyle(NavigationStyle::Default);
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(EditorToolboxTest, testSnapOverrideFollowsNavigationStyle) {
  EditorToolbox::setNavigationStyle(NavigationStyle::Default);
  EXPECT_EQ(Qt::ShiftModifier, EditorToolbox::snapOverrideModifier());
  EXPECT_EQ(Qt::Key_Shift, EditorToolbox::snapOverrideKey());

  // The touchpad style pans with Shift, so Alt disables snapping there.
  EditorToolbox::setNavigationStyle(NavigationStyle::Touchpad);
  EXPECT_EQ(Qt::AltModifier, EditorToolbox::snapOverrideModifier());
  EXPECT_EQ(Qt::Key_Alt, EditorToolbox::snapOverrideKey());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
