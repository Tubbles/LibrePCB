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
#include "../../../testhelpers.h"

#include <gtest/gtest.h>
#include <librepcb/core/types/lengthunit.h>
#include <librepcb/editor/project/board/tracelengthcomparisondialog.h>
#include <librepcb/editor/widgets/unsignedlengthedit.h>

#include <QtWidgets>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {
namespace tests {

using librepcb::tests::TestHelpers;

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class TraceLengthComparisonDialogTest : public ::testing::Test {
protected:
  /// NET1 is 15mm long, NET2 only 10mm, i.e. about 33ps shorter on FR4
  QVector<TraceLengthSegment> mSegments;

  TraceLengthComparisonDialogTest() {
    mSegments.append(TraceLengthSegment{
        "NET1", 1, 2, UnsignedLength(Length::fromMm(15.0))});
    mSegments.append(TraceLengthSegment{
        "NET2", 11, 12, UnsignedLength(Length::fromMm(10.0))});
    QSettings().clear();
  }

  static QString cellText(const QTableWidget& table, int row, int column) {
    const QTableWidgetItem* item = table.item(row, column);
    return item ? item->text() : QString();
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(TraceLengthComparisonDialogTest, testTableAndLiveUpdate) {
  TraceLengthComparisonDialog dialog(mSegments, {}, LengthUnit::millimeters(),
                                     "test", nullptr);
  QTableWidget& table =
      TestHelpers::getChild<QTableWidget>(dialog, "tableWidget");
  EXPECT_EQ(2, table.rowCount());
  EXPECT_EQ(7, table.columnCount());
  EXPECT_EQ("NET1", cellText(table, 0, 0).toStdString());
  EXPECT_EQ("15.0", cellText(table, 0, 1).toStdString());
  EXPECT_EQ("NET2", cellText(table, 1, 0).toStdString());
  EXPECT_EQ("10.0", cellText(table, 1, 1).toStdString());
  EXPECT_EQ("-5.0", cellText(table, 1, 4).toStdString());

  // Without a bus, the default skew is 10% of the unit interval, which is
  // 100ps at the default rate of 1Gbps and therefore met.
  QComboBox& cbxSkewMode =
      TestHelpers::getChild<QComboBox>(dialog, "cbxSkewMode");
  EXPECT_EQ(0, cbxSkewMode.currentIndex());
  EXPECT_TRUE(
      TestHelpers::getChild<QLabel>(dialog, "lblSkewSource").isHidden());
  EXPECT_EQ("In Spec", cellText(table, 0, 6).toStdString());
  EXPECT_EQ("In Spec", cellText(table, 1, 6).toStdString());

  // Tightening the budget to 1% (10ps) must update the table right away.
  TestHelpers::getChild<QDoubleSpinBox>(dialog, "spbxSkewPercent")
      .setValue(1.0);
  EXPECT_EQ("In Spec", cellText(table, 0, 6).toStdString());
  EXPECT_EQ("Out of Spec", cellText(table, 1, 6).toStdString());
}

TEST_F(TraceLengthComparisonDialogTest, testBusDefault) {
  const QVector<TraceLengthBus> buses{TraceLengthBus{
      "DATA", {"NET1", "NET2"}, UnsignedLength(Length::fromMm(1.27))}};
  TraceLengthComparisonDialog dialog(mSegments, buses,
                                     LengthUnit::millimeters(), "test",
                                     nullptr);

  // The bus switches the input to a length and says where it comes from.
  QComboBox& cbxSkewMode =
      TestHelpers::getChild<QComboBox>(dialog, "cbxSkewMode");
  EXPECT_EQ(2, cbxSkewMode.currentIndex());
  UnsignedLengthEdit& edtSkewLength =
      TestHelpers::getChild<UnsignedLengthEdit>(dialog, "edtSkewLength");
  EXPECT_EQ("1.27", edtSkewLength.getValue()->toMmString().toStdString());
  QLabel& lblSkewSource =
      TestHelpers::getChild<QLabel>(dialog, "lblSkewSource");
  EXPECT_FALSE(lblSkewSource.isHidden());
  EXPECT_TRUE(lblSkewSource.text().contains("DATA"));

  // 1.27mm are about 8.5ps, so the 5mm difference is out of spec.
  QTableWidget& table =
      TestHelpers::getChild<QTableWidget>(dialog, "tableWidget");
  EXPECT_EQ("In Spec", cellText(table, 0, 6).toStdString());
  EXPECT_EQ("Out of Spec", cellText(table, 1, 6).toStdString());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
