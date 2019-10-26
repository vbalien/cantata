/*
 * Cantata
 *
 * Copyright (c) 2011-2019 Craig Drummond <craig.p.drummond@gmail.com>
 *
 * ----
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "httpstream.h"
#include "mpdconnection.h"
#include "mpdstatus.h"
#include "gui/settings.h"
#include "support/globalstatic.h"
#include "support/configuration.h"
#if !defined(LIBVLC_FOUND) && !defined(LIBMPV_FOUND)
#include <QtMultimedia/QMediaPlayer>
#endif
#include <QTimer>

static const int constPlayerCheckPeriod = 250;
static const int constMaxPlayStateChecks= 2000 / constPlayerCheckPeriod;

#include <QDebug>
static bool debugEnabled = false;
#define DBUG if (debugEnabled) qWarning() << metaObject()->className() << __FUNCTION__
void HttpStream::enableDebug()
{
    debugEnabled = true;
}

GLOBAL_STATIC(HttpStream, instance)

HttpStream::HttpStream(QObject *p)
    : QObject(p)
    , enabled(false)
    , muted(false)
    , state(MPDState_Inactive)
    , playStateChecks(0)
    , currentVolume(50)
    , unmuteVol(50)
    , playStateCheckTimer(nullptr)
    , player(nullptr)
{
}

void HttpStream::save() const
{
    Configuration config(metaObject()->className());
    config.set("volume", currentVolume);
}

void HttpStream::setEnabled(bool e)
{
    if (e == enabled) {
        return;
    }

    enabled = e;
    if (enabled) {
        connect(MPDConnection::self(), SIGNAL(streamUrl(QString)), this, SLOT(streamUrl(QString)));
        connect(MPDStatus::self(), SIGNAL(updated()), this, SLOT(updateStatus()));
        streamUrl(MPDConnection::self()->getDetails().streamUrl);
    } else {
        disconnect(MPDConnection::self(), SIGNAL(streamUrl(QString)), this, SLOT(streamUrl(QString)));
        disconnect(MPDStatus::self(), SIGNAL(updated()), this, SLOT(updateStatus()));
        if (player) {
            save();
            #ifdef LIBVLC_FOUND
            libvlc_media_player_stop(player);
            #elif LIBMPV_FOUND
            mpv_command_string(player, "stop");
            #else
            player->stop();
            #endif
        }
    }
    emit isEnabled(enabled);
}

void HttpStream::setVolume(int vol)
{
    DBUG << vol;
    if (player) {
        currentVolume = vol;
        #ifdef LIBVLC_FOUND
        libvlc_audio_set_volume(player, vol);
        #elif LIBMPV_FOUND
        double d_vol = (double)vol;
        mpv_set_property(player, "volume", MPV_FORMAT_DOUBLE, &d_vol);
        #else
        player->setVolume(vol);
        #endif
        emit update();
    }
}

int HttpStream::volume()
{
    if (!enabled) {
        return -1;
    }

    int vol = currentVolume;
    if (player && !isMuted()) {
        #ifdef LIBVLC_FOUND
        vol = libvlc_audio_get_volume(player);
        #elif LIBMPV_FOUND
        double d_vol = 0.f;
        mpv_get_property(player, "volume", MPV_FORMAT_DOUBLE, &d_vol);
        vol = (int)d_vol;
        #else
        vol = player->volume();
        #endif
        if (vol < 0) {
            vol = currentVolume;
        } else {
            currentVolume = vol;
        }
    }
    DBUG << vol;
    return vol;
}

void HttpStream::toggleMute()
{
    DBUG << isMuted();
    if (player) {
        muted =! muted;
        #ifdef LIBVLC_FOUND
        libvlc_audio_set_mute(player, muted);
        #elif LIBMPV_FOUND
        int flag = muted ? 1 : 0;
        mpv_set_property(player, "mute", MPV_FORMAT_FLAG, &flag);
        #else
        player->setMuted(!muted);
        #endif
        emit update();
    }
}

void HttpStream::streamUrl(const QString &url)
{
    DBUG << url;
    #ifdef LIBVLC_FOUND
    if (player) {
        libvlc_media_player_stop(player);
        libvlc_media_player_release(player);
        libvlc_release(instance);
        player = 0;
    }
    #elif LIBMPV_FOUND
    if (player) {
        mpv_terminate_destroy(player);
        player = nullptr;
    }
    #else
    if (player) {
        QMediaContent media = player->media();
        if (media != nullptr && media.canonicalUrl() != url) {
            player->stop();
            player->deleteLater();
            player = nullptr;
        }
    }
    #endif
    QUrl qUrl(url);
    if (!url.isEmpty() && qUrl.isValid() && qUrl.scheme().startsWith("http") && !player) {
        #if defined LIBVLC_FOUND
        instance = libvlc_new(0, NULL);
        QByteArray byteArrayUrl = url.toUtf8();
        media = libvlc_media_new_location(instance, byteArrayUrl.constData());
        player = libvlc_media_player_new_from_media(media);
        libvlc_media_release(media);
        #elif LIBMPV_FOUND
        player = mpv_create();
        mpv_initialize(player);
        QByteArray byteArrayUrl = url.toUtf8();
        const char *cmd[] = {"loadfile", byteArrayUrl.constData(), NULL};
        mpv_command(player, cmd);
        #else
        player = new QMediaPlayer(this);
        player->setMedia(qUrl);
        connect(player, &QMediaPlayer::bufferStatusChanged, this, &HttpStream::bufferingProgress);
        #endif
        muted = false;
        setVolume(Configuration(metaObject()->className()).get("volume", currentVolume));
    }
    if (player) {
        state = 0xFFFF; // Force an update...
        updateStatus();
    } else {
        state = MPDState_Inactive;
    }
    emit update();
}

#if !defined(LIBVLC_FOUND) && !defined(LIBMPV_FOUND)
void HttpStream::bufferingProgress(int progress)
{
    MPDStatus * const status = MPDStatus::self();
    if (status->state() == MPDState_Playing) {
        if (progress == 100) {
            player->play();
        } else {
            player->pause();
        }
    }
}
#endif

void HttpStream::updateStatus()
{
    if (!player) {
        return;
    }

    MPDStatus * const status = MPDStatus::self();
    DBUG << status->state() << state;

    // evaluates to true when it is needed to start or restart media player
    bool playerNeedsToStart = status->state() == MPDState_Playing;
    #ifdef LIBVLC_FOUND
    playerNeedsToStart = playerNeedsToStart && libvlc_media_player_get_state(player) != libvlc_Playing;
    #elif LIBMPV_FOUND
    char* is_idle = mpv_get_property_string(player, "idle-active");
    playerNeedsToStart = playerNeedsToStart && strcmp(is_idle, "yes");
    // mpv_free(is_idle);
    #else
    playerNeedsToStart = playerNeedsToStart && player->state() == QMediaPlayer::StoppedState;
    #endif

    if (status->state() == state && !playerNeedsToStart) {
        return;
    }

    state = status->state();
    switch (status->state()) {
    case MPDState_Playing:
        // Only start playback if not aready playing
        if (playerNeedsToStart) {
        #ifdef LIBVLC_FOUND
            libvlc_media_player_play(player);
            startTimer();
        #elif LIBMPV_FOUND
            {
                int flag = 0;
                mpv_set_property(player, "pause", MPV_FORMAT_FLAG, &flag);
            }
        #else
            QUrl url = player->media().canonicalUrl();
            player->setMedia(url);
        #endif
        }
        break;
    case MPDState_Paused:
    case MPDState_Inactive:
    case MPDState_Stopped:
        #ifdef LIBVLC_FOUND
        libvlc_media_player_stop(player);
        stopTimer();
        #elif LIBMPV_FOUND
        {
            int flag = 1;
            mpv_set_property(player, "pause", MPV_FORMAT_FLAG, &flag);
        }
        #else
        player->stop();
        #endif
        break;
    default:
        #ifdef LIBVLC_FOUND
        stopTimer();
        #endif
        break;
    }
}

void HttpStream::checkPlayer()
{
    #ifdef LIBVLC_FOUND
    if (0 == --playStateChecks) {
        stopTimer();
        DBUG << "Max checks reached";
    }
    if (libvlc_media_player_get_state(player) == libvlc_Playing) {
        DBUG << "Playing";
        stopTimer();
    } else {
        DBUG << "Try again";
        libvlc_media_player_play(player);
    }
    #endif
}

void HttpStream::startTimer()
{
    if (!playStateCheckTimer) {
        playStateCheckTimer = new QTimer(this);
        playStateCheckTimer->setSingleShot(false);
        playStateCheckTimer->setInterval(constPlayerCheckPeriod);
        connect(playStateCheckTimer, SIGNAL(timeout()), SLOT(checkPlayer()));
    }
    playStateChecks = constMaxPlayStateChecks;
    DBUG << playStateChecks;
    playStateCheckTimer->start();
}

void HttpStream::stopTimer()
{
    if (playStateCheckTimer) {
        DBUG;
        playStateCheckTimer->stop();
    }
    playStateChecks = 0;
}

#include "moc_httpstream.cpp"
