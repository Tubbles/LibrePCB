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
#include <librepcb/core/application.h>
#include <librepcb/core/fileio/transactionaldirectory.h>
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/project/circuit/circuit.h>
#include <librepcb/core/project/circuit/differentialpairs.h>
#include <librepcb/core/project/circuit/netclass.h>
#include <librepcb/core/project/circuit/netsignal.h>
#include <librepcb/core/project/project.h>

#include <QtCore>

#include <memory>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace tests {

/*******************************************************************************
 *  Test Data Types
 ******************************************************************************/

typedef struct {
  QString name;  ///< The net signal name to classify.
  int polarity;  ///< Expected result of polarityOf().
  QString complement;  ///< Expected complement name, empty for none.
} DifferentialPairsNameTestData;

/*******************************************************************************
 *  Test Classes
 ******************************************************************************/

class DifferentialPairsNameTest
  : public ::testing::TestWithParam<DifferentialPairsNameTestData> {};

class DifferentialPairsTest : public ::testing::Test {
protected:
  std::unique_ptr<Project> mProject;

  DifferentialPairsTest() {
    mProject = Project::create(
        std::make_unique<TransactionalDirectory>(
            TransactionalFileSystem::openRW(Application::getRandomTempPath())),
        "project.lpp");
  }

  Circuit& circuit() const noexcept { return mProject->getCircuit(); }

  /**
   * @brief Add a net signal to the circuit, owned by the circuit
   */
  NetSignal& addNetSignal(const QString& name) {
    NetClass* netclass = circuit().getNetClasses().first();
    NetSignal* netsignal =
        new NetSignal(circuit(), Uuid::createRandom(), *netclass,
                      CircuitIdentifier(name), false);
    circuit().addNetSignal(*netsignal);
    return *netsignal;
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_P(DifferentialPairsNameTest, testPolarityOf) {
  const DifferentialPairsNameTestData& data = GetParam();
  EXPECT_EQ(data.polarity, DifferentialPairs::polarityOf(data.name))
      << "Name: " << data.name.toStdString();
}

TEST_P(DifferentialPairsNameTest, testComplementNameOf) {
  const DifferentialPairsNameTestData& data = GetParam();
  const std::optional<QString> actual =
      DifferentialPairs::complementNameOf(data.name);
  if (data.complement.isEmpty()) {
    EXPECT_FALSE(actual.has_value()) << "Name: " << data.name.toStdString();
  } else {
    ASSERT_TRUE(actual.has_value()) << "Name: " << data.name.toStdString();
    EXPECT_EQ(data.complement.toStdString(), actual->toStdString());
  }
}

TEST_P(DifferentialPairsNameTest, testComplementIsSymmetric) {
  const DifferentialPairsNameTestData& data = GetParam();
  if (!data.complement.isEmpty()) {
    const std::optional<QString> back =
        DifferentialPairs::complementNameOf(data.complement);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(data.name.toStdString(), back->toStdString());
    EXPECT_EQ(-data.polarity, DifferentialPairs::polarityOf(data.complement));
  }
}

TEST_F(DifferentialPairsTest, testPartnerOfBothHalvesPresent) {
  NetSignal& positive = addNetSignal("USB_DP");
  NetSignal& negative = addNetSignal("USB_DN");

  EXPECT_EQ(&negative, DifferentialPairs::partnerOf(positive));
  EXPECT_EQ(&positive, DifferentialPairs::partnerOf(negative));
}

TEST_F(DifferentialPairsTest, testPartnerOfOnlyOneHalfPresent) {
  NetSignal& positive = addNetSignal("USB_DP");

  EXPECT_EQ(nullptr, DifferentialPairs::partnerOf(positive));
  EXPECT_FALSE(DifferentialPairs::pairOf(positive).has_value());
}

TEST_F(DifferentialPairsTest, testPartnerOfUnpairableName) {
  NetSignal& netsignal = addNetSignal("VCC");
  addNetSignal("GND");

  EXPECT_EQ(nullptr, DifferentialPairs::partnerOf(netsignal));
  EXPECT_FALSE(DifferentialPairs::pairOf(netsignal).has_value());
}

TEST_F(DifferentialPairsTest, testPairOfIsPolaritySorted) {
  NetSignal& positive = addNetSignal("CLK+");
  NetSignal& negative = addNetSignal("CLK-");

  const std::optional<DifferentialPairs::Pair> fromPositive =
      DifferentialPairs::pairOf(positive);
  ASSERT_TRUE(fromPositive.has_value());
  EXPECT_EQ(&positive, fromPositive->positive);
  EXPECT_EQ(&negative, fromPositive->negative);

  // Asking the other half must yield the very same pair.
  const std::optional<DifferentialPairs::Pair> fromNegative =
      DifferentialPairs::pairOf(negative);
  ASSERT_TRUE(fromNegative.has_value());
  EXPECT_EQ(&positive, fromNegative->positive);
  EXPECT_EQ(&negative, fromNegative->negative);
}

TEST_F(DifferentialPairsTest, testAllPairsEmptyCircuit) {
  EXPECT_TRUE(DifferentialPairs::allPairs(circuit()).isEmpty());
}

TEST_F(DifferentialPairsTest, testAllPairsOrderedAndListedOnce) {
  // Deliberately added out of order, and mixed with nets which form no pair.
  addNetSignal("LVDS_N0");
  addNetSignal("USB_DP");
  addNetSignal("VCC");
  addNetSignal("CLK-");
  addNetSignal("LVDS_P0");
  addNetSignal("TX+_1");
  addNetSignal("USB_DN");
  addNetSignal("GND");
  addNetSignal("CLK+");
  addNetSignal("TX-_1");
  addNetSignal("RX_P");  // Partner "RX_N" is missing.

  const QVector<DifferentialPairs::Pair> pairs =
      DifferentialPairs::allPairs(circuit());

  QStringList actual;
  foreach (const DifferentialPairs::Pair& pair, pairs) {
    actual.append(*pair.positive->getName() + "/" + *pair.negative->getName());
  }
  const QStringList expected = {
      "CLK+/CLK-",
      "LVDS_P0/LVDS_N0",
      "TX+_1/TX-_1",
      "USB_DP/USB_DN",
  };
  EXPECT_EQ(expected.join(",").toStdString(), actual.join(",").toStdString());
}

/*******************************************************************************
 *  Test Data
 ******************************************************************************/

// clang-format off
INSTANTIATE_TEST_SUITE_P(DifferentialPairsNameTest, DifferentialPairsNameTest, ::testing::Values(
  // Positive halves.
  DifferentialPairsNameTestData({"USB_DP", 1, "USB_DN"}),
  DifferentialPairsNameTestData({"CLK+", 1, "CLK-"}),
  DifferentialPairsNameTestData({"LVDS_P0", 1, "LVDS_N0"}),
  DifferentialPairsNameTestData({"TX+_1", 1, "TX-_1"}),
  DifferentialPairsNameTestData({"P", 1, "N"}),
  DifferentialPairsNameTestData({"+", 1, "-"}),
  DifferentialPairsNameTestData({"HDMI_D2P_12", 1, "HDMI_D2N_12"}),

  // Negative halves.
  DifferentialPairsNameTestData({"USB_DN", -1, "USB_DP"}),
  DifferentialPairsNameTestData({"CLK-", -1, "CLK+"}),
  DifferentialPairsNameTestData({"LVDS_N0", -1, "LVDS_P0"}),
  DifferentialPairsNameTestData({"TX-_1", -1, "TX+_1"}),

  // No polarity character at all.
  DifferentialPairsNameTestData({"clk_p", 0, ""}),
  DifferentialPairsNameTestData({"clk_n", 0, ""}),
  DifferentialPairsNameTestData({"DATA0", 0, ""}),
  DifferentialPairsNameTestData({"VCC", 0, ""}),
  DifferentialPairsNameTestData({"GND", 0, ""}),
  DifferentialPairsNameTestData({"NET_42", 0, ""}),
  DifferentialPairsNameTestData({"_", 0, ""}),
  DifferentialPairsNameTestData({"123", 0, ""}),
  DifferentialPairsNameTestData({"", 0, ""})
));
// clang-format on

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
