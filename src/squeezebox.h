/******************************************************************************
 *
 * Copyright (C) 2020 Andreas Mroß <andreas@mross.pw>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

const bool NO_WORKER_THREAD = false;

class SqueezeboxPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "openhab.json")

 public:
    SqueezeboxPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

class Squeezebox : public Integration {
    Q_OBJECT

 public:
    explicit Squeezebox(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                        YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    void sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) override;

 private slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void connect() override;
    void disconnect() override;
    void leaveStandby() override;
    void enterStandby() override;

    void socketConnected();
    void socketReceived();
    void onMediaProgressTimer();

 private:
    struct SqPlayer {
        SqPlayer() {}
        SqPlayer(bool connected) : connected(connected) {}
        bool   connected = false;
        bool   subscribed = false;
        bool isPlaying = false;
        double position = 0;
    };
    const QString _sqCmdPlayerStatus = "status - 1 tags:aBcdgjKlNotuxyY power";

    void getPlayers();
    void jsonError(const QString& error);
    void sendCometd(const QByteArray& message);
    void sqCommand(const QString& playerMac, const QString& command);
    void getPlayerStatus(const QString& playerMac);
    void parsePlayerStatus(const QString& playerMac, const QVariantMap& data);

    QByteArray      buildRpcJson(int id, const QString& player, const QString& command);
    QNetworkRequest buildRpcRequest();

 private:
    enum connectionStates {
        idle,
        playerInfo,
        cometdHandshake,
        cometdConnect,
        cometdSubscribe,
        connected
    } connectionState;

    QString                 _url;
    int                     _port;
    QString                 _httpurl;
    QNetworkAccessManager   _nam;
    QTcpSocket              _socket;
    QTimer                  _mediaProgress;
    QString                 _clientId;
    int                     _playerCnt;
    QString                 _subscriptionChannel;
    QMap<QString, SqPlayer> _sqPlayerDatabase;   // key: player mac, value: player infos
    QMap<int, QString>      _sqPlayerIdMapping;  // key: subscription id, value: player mac
    QList<EntityInterface*> _myEntities;
    bool    _inStandby;
};
