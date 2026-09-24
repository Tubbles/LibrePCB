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
#include "pnssessionrecorder.h"

#include <librepcb/core/fileio/fileutils.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

PnsSessionRecorder::PnsSessionRecorder(QObject* parent) noexcept
  : QObject(parent), mDirectory(), mSessionCount(0) {
}

PnsSessionRecorder::~PnsSessionRecorder() noexcept {
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

void PnsSessionRecorder::start(const FilePath& directory) noexcept {
  stop();  // Hand a running recording over before the directory changes.
  if (!directory.isExistingDir()) {
    return;
  }

  mDirectory = directory;
  mSessionCount = 0;
  emit sessionCountChanged(mSessionCount);
  emit recordingStarted();
}

void PnsSessionRecorder::stop() noexcept {
  if (!isRecording()) {
    return;
  }

  // While the directory is still set, so that a session which is recording
  // right now can still write what it has.
  emit recordingAboutToStop();

  mDirectory = FilePath();
  emit recordingStopped();
}

FilePath PnsSessionRecorder::writeSession(const QString& text,
                                          const QString& boardName) {
  if ((!isRecording()) || text.isEmpty()) {
    return FilePath();
  }

  const QString timestamp =
      QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
  const QString name = FilePath::cleanFileName(
      boardName, FilePath::ReplaceSpaces | FilePath::KeepCase);
  FilePath fp = mDirectory.getPathTo(timestamp % "-" % name % ".txt");
  for (int i = 2; fp.isExistingFile(); ++i) {
    fp = mDirectory.getPathTo(timestamp % "-" % name % "-" %
                              QString::number(i) % ".txt");
  }

  FileUtils::writeFile(fp, text.toUtf8());  // can throw

  ++mSessionCount;
  emit sessionCountChanged(mSessionCount);
  return fp;
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
