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
#include <librepcb/core/fileio/transactionalfilesystem.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/drc/boarddesignrulecheck.h>
#include <librepcb/core/project/board/drc/boarddesignrulecheckmessages.h>
#include <librepcb/core/project/project.h>
#include <librepcb/core/project/projectloader.h>
#include <librepcb/core/serialization/sexpression.h>
#include <librepcb/core/utils/toolbox.h>

#include <QtCore>

#include <memory>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace tests {

/*******************************************************************************
 *  Test Class
 ******************************************************************************/

class BoardDesignRuleCheckTest : public ::testing::Test {};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST(BoardDesignRuleCheckTest, testMessages) {
  // Ignore certain messages in all boards, except on whitelisted ones.
  QHash<QString, QStringList> whitelist = {
      {"missing_device", {"checkForUnplacedComponents"}},
      {"missing_connection", {"checkForMissingConnections"}},
      {"unused_layer", {"checkUsedLayers"}},
      {"useless_via", {"checkVias", "checkVias2", "checkVias3"}},
      {"plated_cutouts", {"checkBoardCutouts"}},
  };

  // Open project from test data directory.
  FilePath projectFp(TEST_DATA_DIR "/projects/DRC/project.lpp");
  std::shared_ptr<TransactionalFileSystem> projectFs =
      TransactionalFileSystem::openRO(projectFp.getParentDir());
  ProjectLoader loader;
  std::unique_ptr<Project> project =
      loader.open(std::make_unique<TransactionalDirectory>(projectFs),
                  projectFp.getFilename());  // can throw

  // Run DRC for each board.
  QStringList summary;
  foreach (Board* board, project->getBoards()) {
    std::cout << "- Run DRC for board '" << board->getName()->toStdString()
              << "':\n";

    BoardDesignRuleCheck drc;
    drc.start(*board, board->getDrcSettings(), false);
    const BoardDesignRuleCheck::Result result = drc.waitForFinished();

    // Filter messages, get approvals and check uniqueness of their approval.
    QSet<SExpression> approvals;
    for (const auto& msg : result.messages) {
      // Skip some messages.
      const QString msgType = msg->getApproval().getChild("@0").getValue();
      if (whitelist.contains(msgType)) {
        if (!whitelist[msgType].contains(*board->getName())) {
          continue;
        }
      }

      // Check for ambiguous approvals.
      if (approvals.contains(msg->getApproval())) {
        std::cout << "  * Ambiguous approval for message '"
                  << msg->getMessage().toStdString() << "':\n"
                  << msg->getApproval().toByteArray().toStdString() << "\n";
        ADD_FAILURE();
      }

      approvals.insert(msg->getApproval());
    }

    // Build actual approvals.
    std::unique_ptr<SExpression> actual = SExpression::createList("node");
    foreach (const SExpression& node, Toolbox::sortedQSet(approvals)) {
      actual->ensureLineBreak();
      actual->appendChild(node);
    }
    actual->ensureLineBreak();

    // Build expected approvals.
    std::unique_ptr<SExpression> expected = SExpression::createList("node");
    foreach (const SExpression& node,
             Toolbox::sortedQSet(board->getDrcMessageApprovals())) {
      expected->ensureLineBreak();
      expected->appendChild(node);
    }
    expected->ensureLineBreak();

    // Compare.
    const QString msg = QString("Emitted %1 messages, %2 approved")
                            .arg(approvals.count())
                            .arg(board->getDrcMessageApprovals().count());
    std::cout << "  * " << qPrintable(msg) << "\n";
    summary.append(QString(" * %1: %2").arg(*board->getName(), msg));
    EXPECT_EQ(expected->toByteArray().toStdString(),
              actual->toByteArray().toStdString());
    EXPECT_EQ(board->getDrcMessageApprovals().count(), approvals.count());
  }

  // The output in case of failures can be very verbose, so we print a more
  // readable summary at the end.
  if (::testing::Test::HasFailure()) {
    std::cout << "Summary:\n";
    for (const QString& s : summary) {
      std::cout << qPrintable(s) << "\n";
    }
  }
}

TEST(BoardDesignRuleCheckTest, testCopperHoleClearanceMessageTexts) {
  typedef DrcMsgCopperHoleClearanceViolation Msg;
  typedef Msg::CopperType Type;
  const BoardDesignRuleCheckData::Hole hole{
      Uuid::createRandom(),
      PositiveLength(1000000),
      makeNonEmptyPath(Point(0, 0)),
      std::nullopt,
  };
  auto text = [&hole](const QList<Type>& types) {
    return Msg(hole, nullptr, types, UnsignedLength(200000), {})
        .getMessage()
        .toStdString();
  };
  EXPECT_EQ("Clearance copper ↔ hole < 0.2 mm", text({}));
  EXPECT_EQ("Clearance copper (pad) ↔ hole < 0.2 mm", text({Type::Pad}));
  EXPECT_EQ("Clearance copper (trace) ↔ hole < 0.2 mm", text({Type::Trace}));
  EXPECT_EQ("Clearance copper (via) ↔ hole < 0.2 mm", text({Type::Via}));
  EXPECT_EQ("Clearance copper (plane) ↔ hole < 0.2 mm", text({Type::Plane}));
  EXPECT_EQ("Clearance copper (polygon) ↔ hole < 0.2 mm",
            text({Type::Polygon}));
  EXPECT_EQ("Clearance copper (circle) ↔ hole < 0.2 mm",
            text({Type::Circle}));
  EXPECT_EQ("Clearance copper (text) ↔ hole < 0.2 mm", text({Type::Text}));
  EXPECT_EQ("Clearance copper (pad, trace, via) ↔ hole < 0.2 mm",
            text({Type::Pad, Type::Trace, Type::Via}));
}

TEST(BoardDesignRuleCheckTest, testCopperHoleClearanceApprovalIsStable) {
  typedef DrcMsgCopperHoleClearanceViolation Msg;
  typedef Msg::CopperType Type;
  const BoardDesignRuleCheckData::Hole hole{
      Uuid::fromString("2bfeb64d-bade-4ca6-98e4-ff0bc443aa41"),
      PositiveLength(1000000),
      makeNonEmptyPath(Point(0, 0)),
      std::nullopt,
  };
  const Msg withoutType(hole, nullptr, {}, UnsignedLength(200000), {});
  const Msg withTypes(hole, nullptr, {Type::Pad, Type::Plane},
                      UnsignedLength(200000), {});
  EXPECT_EQ(withoutType.getApproval(), withTypes.getApproval());
  EXPECT_EQ(
      "(approved copper_hole_clearance_violation\n"
      " (hole 2bfeb64d-bade-4ca6-98e4-ff0bc443aa41)\n"
      ")\n",
      withTypes.getApproval().toByteArray().toStdString());
}

TEST(BoardDesignRuleCheckTest, testCopperHoleClearanceCopperTypes) {
  // Open project from test data directory.
  FilePath projectFp(TEST_DATA_DIR "/projects/DRC/project.lpp");
  std::shared_ptr<TransactionalFileSystem> projectFs =
      TransactionalFileSystem::openRO(projectFp.getParentDir());
  ProjectLoader loader;
  std::unique_ptr<Project> project =
      loader.open(std::make_unique<TransactionalDirectory>(projectFs),
                  projectFp.getFilename());  // can throw
  Board* board = nullptr;
  foreach (Board* b, project->getBoards()) {
    if (*b->getName() == "checkCopperHoleClearances") {
      board = b;
    }
  }
  ASSERT_TRUE(board);

  // Run DRC and collect the messages per hole.
  BoardDesignRuleCheck drc;
  drc.start(*board, board->getDrcSettings(), false);
  const BoardDesignRuleCheck::Result result = drc.waitForFinished();
  QMap<QString, QString> actual;
  for (const auto& msg : result.messages) {
    const SExpression& approval = msg->getApproval();
    if (approval.getChild("@0").getValue() ==
        "copper_hole_clearance_violation") {
      actual.insert(approval.getChild("hole/@0").getValue(),
                    msg->getMessage());
    }
  }
  const QString suffix = " ↔ hole < 0.25 mm";
  const QMap<QString, QString> expected = {
      {"2bfeb64d-bade-4ca6-98e4-ff0bc443aa41", "Clearance copper (via)"},
      {"3a1afd1b-2826-4b47-9645-9083ac0366b5", "Clearance copper (polygon)"},
      {"8c745ec6-187f-4a59-83eb-d86c3646106d", "Clearance copper (pad)"},
      {"cbe52bcf-b196-469f-9bcd-9fef6c84551d", "Clearance copper (pad)"},
      {"cc2557b8-1535-4e06-92e0-303c34651ed9", "Clearance copper (trace)"},
      {"d2f6c593-a1cb-483c-9d3c-cd618965e1e1", "Clearance copper (text)"},
      {"fa425f13-29e4-4621-92bf-9928ad93bb5f", "Clearance copper (plane)"},
  };
  ASSERT_EQ(expected.count(), actual.count());
  for (auto it = expected.begin(); it != expected.end(); ++it) {
    EXPECT_EQ((it.value() + suffix).toStdString(),
              actual.value(it.key()).toStdString());
  }
}

TEST(BoardDesignRuleCheckTest, testMultithreading) {
  // open project from test data directory
  FilePath projectFp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
  std::shared_ptr<TransactionalFileSystem> projectFs =
      TransactionalFileSystem::openRO(projectFp.getParentDir());
  ProjectLoader loader;
  std::unique_ptr<Project> project =
      loader.open(std::make_unique<TransactionalDirectory>(projectFs),
                  projectFp.getFilename());  // can throw
  Board* board = project->getBoards().first();

  // Run several times to heavily test multithreading.
  const int runs = qBound(10, QThread::idealThreadCount() * 8, 50);
  qreal totalTimeMs = 0;
  BoardDesignRuleCheck drc;
  for (int i = 0; i < runs; ++i) {
    std::chrono::time_point<std::chrono::high_resolution_clock> start =
        std::chrono::high_resolution_clock::now();
    drc.start(*board, board->getDrcSettings(), false);
    const BoardDesignRuleCheck::Result result = drc.waitForFinished();
    std::chrono::duration<double> elapsed =
        std::chrono::high_resolution_clock::now() - start;
    totalTimeMs += elapsed.count() * 1000;

    // Check result.
    EXPECT_EQ(0, result.errors.count());
  }
  std::cout << "Average time over " << runs << " runs: " << (totalTimeMs / runs)
            << " ms\n";
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace librepcb
