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

#ifndef LIBREPCB_EDITOR_PNSRECORDINGWINDOW_H
#define LIBREPCB_EDITOR_PNSRECORDINGWINDOW_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include <QtCore>
#include <QtWidgets>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {
namespace editor {

class PnsSessionRecorder;

/*******************************************************************************
 *  Class PnsRecordingWindow
 ******************************************************************************/

/**
 * @brief Shows what ::librepcb::editor::PnsSessionRecorder is doing
 *
 * A tool window rather than a message box, because the whole point of
 * recording is to keep routing while it runs, and rather than a dock or a
 * panel because it belongs to no particular board.
 *
 * Stopping is left to the owner: the window only closes itself, and
 * ::librepcb::editor::GuiApplication stops the recorder when the window is
 * destroyed. That makes closing and being closed with the main window the
 * same thing, and leaves no way to dereference a recorder which is already
 * gone.
 */
class PnsRecordingWindow final : public QDialog {
  Q_OBJECT

public:
  // Constructors / Destructor
  PnsRecordingWindow() = delete;
  PnsRecordingWindow(const PnsRecordingWindow& other) = delete;

  /**
   * @brief Show what a recorder is recording
   *
   * @param recorder  The recorder to follow. Only read here, and only
   *                  through signals afterwards.
   * @param parent    The window this one stays on top of.
   */
  explicit PnsRecordingWindow(const PnsSessionRecorder& recorder,
                              QWidget* parent = nullptr) noexcept;
  ~PnsRecordingWindow() noexcept;

  // Operator Overloadings
  PnsRecordingWindow& operator=(const PnsRecordingWindow& rhs) = delete;

private:  // Methods
  void setSessionCount(int count) noexcept;

private:  // Data
  QLabel* mSessionCountLabel;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
