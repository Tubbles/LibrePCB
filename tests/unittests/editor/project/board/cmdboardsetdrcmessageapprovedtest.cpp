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
#include <librepcb/editor/project/cmd/cmdboardsetdrcmessageapproved.h>

#include <QtCore>
#include <QtTest>

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

class CmdBoardSetDrcMessageApprovedTest : public ::testing::Test {
protected:
  void SetUp() override {
    const FilePath projectFp(TEST_DATA_DIR "/projects/Gerber Test/project.lpp");
    std::shared_ptr<TransactionalFileSystem> projectFs =
        TransactionalFileSystem::openRO(projectFp.getParentDir());
    ProjectLoader loader;
    mProject = loader.open(std::make_unique<TransactionalDirectory>(projectFs),
                           projectFp.getFilename());  // can throw
    mBoard = mProject->getBoards().first();
  }

  static SExpression approval(const QString& name) {
    std::unique_ptr<SExpression> node = SExpression::createList("approved");
    node->appendChild(SExpression::createToken(name));
    return *node;
  }

  bool isApproved(const SExpression& approval) const {
    return mBoard->getDrcMessageApprovals().contains(approval);
  }

  std::unique_ptr<Project> mProject;
  Board* mBoard = nullptr;
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(CmdBoardSetDrcMessageApprovedTest, testApproveUndoRedo) {
  const SExpression msg = approval("test_message");
  const QSet<SExpression> approvalsBefore = mBoard->getDrcMessageApprovals();
  ASSERT_FALSE(isApproved(msg));
  QSignalSpy spy(mBoard, &Board::drcMessageApprovalChanged);

  CmdBoardSetDrcMessageApproved cmd(*mBoard, msg, true);
  EXPECT_TRUE(cmd.execute());
  EXPECT_TRUE(isApproved(msg));
  EXPECT_EQ(approvalsBefore.count() + 1,
            mBoard->getDrcMessageApprovals().count());
  EXPECT_EQ(1, spy.count());

  cmd.undo();
  EXPECT_FALSE(isApproved(msg));
  EXPECT_EQ(approvalsBefore, mBoard->getDrcMessageApprovals());
  EXPECT_EQ(2, spy.count());

  cmd.redo();
  EXPECT_TRUE(isApproved(msg));
  EXPECT_EQ(approvalsBefore.count() + 1,
            mBoard->getDrcMessageApprovals().count());
  EXPECT_EQ(3, spy.count());
}

TEST_F(CmdBoardSetDrcMessageApprovedTest, testRemoveApprovalUndoRedo) {
  const SExpression msg = approval("test_message");
  mBoard->setDrcMessageApproved(msg, true);
  const QSet<SExpression> approvalsBefore = mBoard->getDrcMessageApprovals();

  CmdBoardSetDrcMessageApproved cmd(*mBoard, msg, false);
  EXPECT_TRUE(cmd.execute());
  EXPECT_FALSE(isApproved(msg));

  cmd.undo();
  EXPECT_TRUE(isApproved(msg));
  EXPECT_EQ(approvalsBefore, mBoard->getDrcMessageApprovals());

  cmd.redo();
  EXPECT_FALSE(isApproved(msg));
}

TEST_F(CmdBoardSetDrcMessageApprovedTest, testNoChangeDoesNothing) {
  const SExpression msg = approval("test_message");
  const QSet<SExpression> approvalsBefore = mBoard->getDrcMessageApprovals();
  QSignalSpy spy(mBoard, &Board::drcMessageApprovalChanged);

  CmdBoardSetDrcMessageApproved cmd(*mBoard, msg, false);
  EXPECT_FALSE(cmd.execute());
  EXPECT_EQ(approvalsBefore, mBoard->getDrcMessageApprovals());
  EXPECT_EQ(0, spy.count());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
