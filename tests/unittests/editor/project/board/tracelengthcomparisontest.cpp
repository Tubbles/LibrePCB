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
#include <librepcb/editor/project/board/tracelengthcomparison.h>

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

class TraceLengthComparisonTest : public ::testing::Test {
protected:
  /// Delay of a 1mm trace at an effective dielectric constant of 4.0, in ps
  static constexpr qreal sDelayPerMm = 6.6712819;

  static TraceLengthSegment segment(const QString& netName,
                                    TraceLengthAnchorId startAnchor,
                                    TraceLengthAnchorId endAnchor,
                                    qreal lengthMm) {
    return TraceLengthSegment{netName, startAnchor, endAnchor,
                              UnsignedLength(Length::fromMm(lengthMm))};
  }

  /// 1 Gbit/s, 10% of the unit interval allowed skew, FR4 stripline
  static TraceLengthComparisonSettings settings(qreal allowedSkewPs = 100) {
    TraceLengthComparisonSettings s;
    s.unitIntervalPs = unitIntervalPsOfDataRate(1.0);
    s.allowedSkewPs = allowedSkewPs;
    s.effectiveDielectricConstant = 4.0;
    return s;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(TraceLengthComparisonTest, testFormulas) {
  // A 1 Gbit/s bit lasts 1ns, a 100MHz period lasts 10ns.
  EXPECT_NEAR(1000, unitIntervalPsOfDataRate(1.0), 1e-9);
  EXPECT_NEAR(400, unitIntervalPsOfDataRate(2.5), 1e-9);
  EXPECT_NEAR(10000, unitIntervalPsOfFrequency(100.0), 1e-9);

  // In vacuum the velocity is the speed of light, at Dk=4 it is halved.
  EXPECT_NEAR(sSpeedOfLightMmPerPs, traceVelocityMmPerPs(1.0), 1e-12);
  EXPECT_NEAR(sSpeedOfLightMmPerPs / 2, traceVelocityMmPerPs(4.0), 1e-12);

  // The well known ~6.7ps/mm of an FR4 stripline.
  EXPECT_NEAR(sDelayPerMm, traceDelayPs(Length::fromMm(1.0), 4.0), 1e-6);
  EXPECT_NEAR(10 * sDelayPerMm, traceDelayPs(Length::fromMm(10.0), 4.0), 1e-6);
}

TEST_F(TraceLengthComparisonTest, testTwoNets) {
  // NET1 is a chain of 10mm + 5mm, NET2 is a single 10mm segment.
  const QVector<TraceLengthSegment> segments{
      segment("NET1", 1, 2, 10.0),
      segment("NET1", 2, 3, 5.0),
      segment("NET2", 11, 12, 10.0),
  };

  const QVector<TraceLengthComparisonRow> rows =
      compareTraceLengths(segments, settings());
  ASSERT_EQ(2, rows.count());

  EXPECT_EQ("NET1", rows.at(0).netName);
  EXPECT_EQ(TraceLengthWarning::None, rows.at(0).warning);
  EXPECT_EQ(Length::fromMm(15.0), *rows.at(0).length);
  EXPECT_NEAR(15 * sDelayPerMm, rows.at(0).delayPs, 1e-6);
  EXPECT_EQ(Length(0), rows.at(0).differenceToLongest);
  EXPECT_NEAR(0, rows.at(0).differenceToLongestPs, 1e-9);
  EXPECT_TRUE(rows.at(0).inSpec);

  EXPECT_EQ("NET2", rows.at(1).netName);
  EXPECT_EQ(TraceLengthWarning::None, rows.at(1).warning);
  EXPECT_EQ(Length::fromMm(10.0), *rows.at(1).length);
  EXPECT_NEAR(10 * sDelayPerMm, rows.at(1).delayPs, 1e-6);
  EXPECT_EQ(Length::fromMm(-5.0), rows.at(1).differenceToLongest);
  EXPECT_NEAR(-5 * sDelayPerMm, rows.at(1).differenceToLongestPs, 1e-6);
  EXPECT_TRUE(rows.at(1).inSpec);

  // 5mm are about 33.4ps, so a 20ps skew budget is not met anymore.
  const QVector<TraceLengthComparisonRow> tightRows =
      compareTraceLengths(segments, settings(20));
  ASSERT_EQ(2, tightRows.count());
  EXPECT_TRUE(tightRows.at(0).inSpec);
  EXPECT_FALSE(tightRows.at(1).inSpec);
}

TEST_F(TraceLengthComparisonTest, testBranchedSelection) {
  // Three segments of NET1 meet at anchor 2, NET2 stays measurable.
  const QVector<TraceLengthSegment> segments{
      segment("NET1", 1, 2, 10.0),
      segment("NET1", 2, 3, 5.0),
      segment("NET1", 2, 4, 5.0),
      segment("NET2", 11, 12, 10.0),
  };

  const QVector<TraceLengthComparisonRow> rows =
      compareTraceLengths(segments, settings());
  ASSERT_EQ(2, rows.count());
  EXPECT_EQ("NET1", rows.at(0).netName);
  EXPECT_EQ(TraceLengthWarning::Branched, rows.at(0).warning);
  EXPECT_EQ("NET2", rows.at(1).netName);
  EXPECT_EQ(TraceLengthWarning::None, rows.at(1).warning);

  // The branched net must not be taken as the longest one.
  EXPECT_EQ(Length(0), rows.at(1).differenceToLongest);
}

TEST_F(TraceLengthComparisonTest, testDisconnectedSelection) {
  // The two segments of NET1 don't share an anchor.
  const QVector<TraceLengthSegment> segments{
      segment("NET1", 1, 2, 10.0),
      segment("NET1", 3, 4, 5.0),
  };

  const TraceLengthChain chain = measureTraceLengthChain(segments);
  EXPECT_EQ(TraceLengthWarning::Disconnected, chain.warning);
}

TEST_F(TraceLengthComparisonTest, testSingleSegmentChain) {
  const TraceLengthChain chain =
      measureTraceLengthChain({segment("NET1", 1, 2, 10.0)});
  EXPECT_EQ(TraceLengthWarning::None, chain.warning);
  EXPECT_EQ(Length::fromMm(10.0), *chain.length);
}

TEST_F(TraceLengthComparisonTest, testCommonBus) {
  const QVector<TraceLengthBus> buses{
      TraceLengthBus{"DATA",
                     {"NET1", "NET2", "NET3"},
                     UnsignedLength(Length::fromMm(1.0))},
      TraceLengthBus{"CLK", {"NET4"}, std::nullopt},
      TraceLengthBus{"ADDR", {"NET1", "NET2", "NET5"}, std::nullopt},
  };

  // The bus containing all compared nets provides the skew.
  const std::optional<TraceLengthBus> bus =
      findCommonTraceLengthBus(buses, {"NET1", "NET2"});
  ASSERT_TRUE(bus.has_value());
  EXPECT_EQ("DATA", bus->name);
  ASSERT_TRUE(bus->maxTraceLengthDifference.has_value());
  EXPECT_EQ(Length::fromMm(1.0), **bus->maxTraceLengthDifference);

  // No bus contains all compared nets.
  EXPECT_FALSE(
      findCommonTraceLengthBus(buses, {"NET1", "NET4"}).has_value());

  // Only a bus without a maximum trace length difference would qualify.
  EXPECT_FALSE(
      findCommonTraceLengthBus(buses, {"NET1", "NET5"}).has_value());
}

TEST_F(TraceLengthComparisonTest, testAmbiguousBus) {
  const QVector<TraceLengthBus> buses{
      TraceLengthBus{"DATA0",
                     {"NET1", "NET2"},
                     UnsignedLength(Length::fromMm(1.0))},
      TraceLengthBus{"DATA1",
                     {"NET1", "NET2"},
                     UnsignedLength(Length::fromMm(2.0))},
  };
  EXPECT_FALSE(
      findCommonTraceLengthBus(buses, {"NET1", "NET2"}).has_value());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
