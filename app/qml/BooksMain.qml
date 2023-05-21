/*
  Copyright (C) 2015-2021 Jolla Ltd.
  Copyright (C) 2015-2023 Slava Monich <slava@monich.com>

  You may use this file under the terms of BSD license as follows:

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer
       in the documentation and/or other materials provided with the
       distribution.
    3. Neither the names of the copyright holders nor the names of its
       contributors may be used to endorse or promote products derived
       from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
*/

import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.books 1.0

ApplicationWindow {
    id: window
    allowedOrientations: {
        switch (Settings.orientation) {
        default:
        case BooksSettings.OrientationAny: return Orientation.All
        case BooksSettings.OrientationPortrait: return Orientation.Portrait
        case BooksSettings.OrientationLandscape: return Orientation.Landscape
        }
    }

    // Application title
    //% "Books"
    readonly property string title: qsTrId("harbour-books-app-name")

    property variant currentShelf: mainPage.currentShelf

    HarbourDisplayBlanking {
        pauseRequested: Qt.application.active &&
            Settings.currentBook && Settings.keepDisplayOn &&
            (HarbourBattery.batteryState === HarbourBattery.BatteryStateCharging ||
             HarbourBattery.batteryLevel === 0 || // Zero if unknown (not reported by mce)
             HarbourBattery.batteryLevel >= 20)
    }

    Binding {
        target: Settings
        property: "theme"
        value: Theme
    }

    initialPage: BooksMainPage { id: mainPage }

    cover: Component {
        BooksCoverPage {
            book: Settings.currentBook
            shelf: currentShelf
        }
    }
}
