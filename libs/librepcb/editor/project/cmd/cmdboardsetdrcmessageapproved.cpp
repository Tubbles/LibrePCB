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
#include "cmdboardsetdrcmessageapproved.h"

#include <librepcb/core/project/board/board.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

CmdBoardSetDrcMessageApproved::CmdBoardSetDrcMessageApproved(
    Board& board, const SExpression& approval, bool approved) noexcept
  : UndoCommand(approved ? tr("Approve DRC message")
                         : tr("Remove DRC message approval")),
    mBoard(board),
    mApproval(approval),
    mApproved(approved),
    mWasApproved(false) {
}

CmdBoardSetDrcMessageApproved::~CmdBoardSetDrcMessageApproved() noexcept {
}

/*******************************************************************************
 *  Inherited from UndoCommand
 ******************************************************************************/

bool CmdBoardSetDrcMessageApproved::performExecute() {
  mWasApproved = mBoard.getDrcMessageApprovals().contains(mApproval);
  if (mWasApproved == mApproved) {
    return false;
  }

  performRedo();  // can throw
  return true;
}

void CmdBoardSetDrcMessageApproved::performUndo() {
  mBoard.setDrcMessageApproved(mApproval, mWasApproved);
}

void CmdBoardSetDrcMessageApproved::performRedo() {
  mBoard.setDrcMessageApproved(mApproval, mApproved);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
