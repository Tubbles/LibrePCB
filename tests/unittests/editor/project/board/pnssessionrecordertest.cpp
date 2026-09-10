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
#include <librepcb/core/fileio/fileutils.h>
#include <librepcb/editor/project/board/pnssessionrecorder.h>

#include <QSignalSpy>
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

/**
 * @brief Drives the recorder the way the router tool state does
 *
 * The recorder holds no router, so the whole start, write, stop cycle runs
 * headlessly: what a session hands over is just a string.
 */
class PnsSessionRecorderTest : public ::testing::Test {
protected:
  FilePath mDir{Application::getRandomTempPath()};
  PnsSessionRecorder mRecorder;

  PnsSessionRecorderTest() { QDir().mkpath(mDir.toNative()); }

  ~PnsSessionRecorderTest() { QDir(mDir.toNative()).removeRecursively(); }

  QStringList writtenFiles() const {
    return QDir(mDir.toNative())
        .entryList(QDir::Files, QDir::Name);  // sorted by name
  }
};

/*******************************************************************************
 *  Test Methods
 ******************************************************************************/

TEST_F(PnsSessionRecorderTest, testNotRecordingByDefault) {
  EXPECT_FALSE(mRecorder.isRecording());
  EXPECT_FALSE(mRecorder.getDirectory().isValid());
  EXPECT_EQ(0, mRecorder.getSessionCount());

  // A session which hands its recording over while nothing is being
  // recorded must not leave a file behind.
  EXPECT_FALSE(mRecorder.writeSession("recording", "Board").isValid());
  EXPECT_EQ(0, mRecorder.getSessionCount());
}

TEST_F(PnsSessionRecorderTest, testStartWriteStop) {
  QSignalSpy startedSpy(&mRecorder, &PnsSessionRecorder::recordingStarted);
  QSignalSpy aboutToStopSpy(&mRecorder,
                            &PnsSessionRecorder::recordingAboutToStop);
  QSignalSpy stoppedSpy(&mRecorder, &PnsSessionRecorder::recordingStopped);
  QSignalSpy countSpy(&mRecorder, &PnsSessionRecorder::sessionCountChanged);

  mRecorder.start(mDir);
  EXPECT_TRUE(mRecorder.isRecording());
  EXPECT_EQ(mDir, mRecorder.getDirectory());
  EXPECT_EQ(0, mRecorder.getSessionCount());
  EXPECT_EQ(1, startedSpy.count());

  const FilePath first = mRecorder.writeSession("first recording", "My Board");
  ASSERT_TRUE(first.isExistingFile());
  EXPECT_EQ("first recording", FileUtils::readFile(first).toStdString());
  EXPECT_EQ(1, mRecorder.getSessionCount());

  const FilePath second =
      mRecorder.writeSession("second recording", "My Board");
  ASSERT_TRUE(second.isExistingFile());
  EXPECT_NE(first, second);  // Even within the same second.
  EXPECT_EQ(2, mRecorder.getSessionCount());
  EXPECT_EQ(2, writtenFiles().count());

  // The board name is part of every file name, so that recordings of
  // different boards do not have to be told apart by their content.
  for (const QString& name : writtenFiles()) {
    EXPECT_TRUE(name.contains("My_Board")) << name.toStdString();
    EXPECT_TRUE(name.endsWith(".txt")) << name.toStdString();
  }

  // Three counter changes: the reset by start() and one per file.
  EXPECT_EQ(3, countSpy.count());

  mRecorder.stop();
  EXPECT_FALSE(mRecorder.isRecording());
  EXPECT_FALSE(mRecorder.getDirectory().isValid());
  EXPECT_EQ(1, aboutToStopSpy.count());
  EXPECT_EQ(1, stoppedSpy.count());

  // Stopping twice is what closing the window after pressing its stop
  // button does, and must be silent.
  mRecorder.stop();
  EXPECT_EQ(1, stoppedSpy.count());

  // The counter of the finished run stays readable, but nothing is written
  // any more.
  EXPECT_EQ(2, mRecorder.getSessionCount());
  EXPECT_FALSE(mRecorder.writeSession("late recording", "My Board").isValid());
  EXPECT_EQ(2, writtenFiles().count());
}

TEST_F(PnsSessionRecorderTest, testEmptyRecordingIsNotWritten) {
  mRecorder.start(mDir);
  EXPECT_FALSE(mRecorder.writeSession(QString(), "My Board").isValid());
  EXPECT_EQ(0, mRecorder.getSessionCount());
  EXPECT_EQ(0, writtenFiles().count());
}

TEST_F(PnsSessionRecorderTest, testStartOnMissingDirectoryDoesNotRecord) {
  mRecorder.start(mDir.getPathTo("nonexistent"));
  EXPECT_FALSE(mRecorder.isRecording());
}

TEST_F(PnsSessionRecorderTest, testRestartResetsTheCounter) {
  mRecorder.start(mDir);
  mRecorder.writeSession("first recording", "My Board");
  EXPECT_EQ(1, mRecorder.getSessionCount());

  mRecorder.start(mDir);
  EXPECT_TRUE(mRecorder.isRecording());
  EXPECT_EQ(0, mRecorder.getSessionCount());
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace tests
}  // namespace editor
}  // namespace librepcb
