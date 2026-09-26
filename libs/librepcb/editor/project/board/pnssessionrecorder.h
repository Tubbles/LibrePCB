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

#ifndef LIBREPCB_EDITOR_PNSSESSIONRECORDER_H
#define LIBREPCB_EDITOR_PNSSESSIONRECORDER_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include <librepcb/core/fileio/filepath.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Class PnsSessionRecorder
 ******************************************************************************/

/**
 * @brief Collects push and shove router session recordings in a directory
 *
 * Recording is application wide rather than per board, so that it can be
 * switched on before the board which shows the bug is even open. One object
 * lives in ::librepcb::editor::GuiApplication and every
 * ::librepcb::editor::BoardEditorState_RouteTrace reaches it through
 * ::librepcb::editor::BoardEditorFsm::Context.
 *
 * A router session decides at construction whether it records, so a tool
 * state has to build a new session to react to #start() and #stop(). That is
 * what the signals are for: #recordingAboutToStop() is the last moment at
 * which a session can still hand its recording over, and the two toggle
 * signals ask the tool states to rebuild.
 */
class PnsSessionRecorder final : public QObject {
  Q_OBJECT

public:
  // Constructors / Destructor
  PnsSessionRecorder(const PnsSessionRecorder& other) = delete;
  explicit PnsSessionRecorder(QObject* parent = nullptr) noexcept;
  ~PnsSessionRecorder() noexcept;

  // Getters

  /// Whether sessions are being recorded at the moment
  bool isRecording() const noexcept { return mDirectory.isValid(); }

  /// The directory recordings are written to, invalid while not recording
  const FilePath& getDirectory() const noexcept { return mDirectory; }

  /// How many recordings were written since the current #start()
  int getSessionCount() const noexcept { return mSessionCount; }

  // General Methods

  /**
   * @brief Start writing recordings into a directory
   *
   * Restarts the session counter at zero. Starting while already recording
   * switches the directory, which stops the previous run first.
   *
   * @param directory   An existing directory. An invalid one stops instead,
   *                    so that a caller cannot record into nowhere.
   */
  void start(const FilePath& directory) noexcept;

  /**
   * @brief Stop writing recordings
   *
   * Emits #recordingAboutToStop() while the directory is still set, so that
   * a session which is recording at this moment can still be written out,
   * then #recordingStopped(). Does nothing when not recording.
   */
  void stop() noexcept;

  /**
   * @brief Write one session recording into the recording directory
   *
   * The file is named `<yyyyMMdd-HHmmss>-<board name>.txt`, with `-<n>`
   * appended if that name is taken, because a user can end two sessions
   * within the same second and the session counter must not promise files
   * which were overwritten.
   *
   * @param text        The recording, in the router crate's own format.
   * @param boardName   The name of the board it was routed on.
   *
   * @return The file which was written, or an invalid
   *         ::librepcb::FilePath if not recording or if the text is empty.
   *
   * @throws ::librepcb::Exception if the file cannot be written.
   */
  FilePath writeSession(const QString& text, const QString& boardName);

  // Operator Overloadings
  PnsSessionRecorder& operator=(const PnsSessionRecorder& rhs) = delete;

signals:
  /// Recording has just been switched on
  void recordingStarted();

  /// Recording is about to be switched off, the directory is still set
  void recordingAboutToStop();

  /// Recording has just been switched off
  void recordingStopped();

  /// Another recording was written, or the counter was reset by #start()
  void sessionCountChanged(int count);

private:  // Data
  /// The directory to write into. Invalid means "not recording", which is
  /// the one place the recording state is kept.
  FilePath mDirectory;

  /// How many recordings were written since the current #start()
  int mSessionCount;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
