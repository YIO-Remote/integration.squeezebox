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

#include "squeezebox.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QString>
#include <QtDebug>

#include "yio-interface/entities/blindinterface.h"
#include "yio-interface/entities/entityinterface.h"
#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-interface/entities/switchinterface.h"

SqueezeboxPlugin::SqueezeboxPlugin() : Plugin("squeezebox", NO_WORKER_THREAD) {}

Integration* SqueezeboxPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                                 NotificationsInterface* notifications, YioAPIInterface* api,
                                                 ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating Squeezebox integration plugin" << PLUGIN_VERSION;

    return new Squeezebox(config, entities, notifications, api, configObj, this);
}

Squeezebox::Squeezebox(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                       YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin), _nam(this), _socket(this) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "url") {
            _url = iter.value().toString();
        } else if (iter.key() == "port") {
            _port = iter.value().toInt();
        }
    }

    _httpurl = "http://" + _url + ":" + QString::number(_port) + "/";

    _connectionState = idle;

    // read added entities
    _myEntities = m_entities->getByIntegration(integrationId());
    for (auto entity : _myEntities) {
        _sqPlayerDatabase.insert(entity->entity_id(), SqPlayer());
    }

    // prepare connection timeout timer
    _connectionTimeout.setSingleShot(true);
    _connectionTimeout.setInterval(3 * 1000);
    _connectionTimeout.stop();

    QObject::connect(&_connectionTimeout, &QTimer::timeout, this, &Squeezebox::onConnectionTimeoutTimer);

    _connectionTries = 0;

    // prepare media progress timer
    _mediaProgress.setSingleShot(false);
    _mediaProgress.setInterval(500);
    _mediaProgress.stop();

    QObject::connect(&_mediaProgress, &QTimer::timeout, this, &Squeezebox::onMediaProgressTimer);

    QObject::connect(&_socket, &QTcpSocket::connected, this, &Squeezebox::socketConnected);
    QObject::connect(&_socket, &QIODevice::readyRead, this, &Squeezebox::socketReceived);
    QObject::connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this,
                     &Squeezebox::socketError);

    QObject::connect(&_nam, &QNetworkAccessManager::networkAccessibleChanged, this,
                     &Squeezebox::networkAccessibleChanged);

    qCDebug(m_logCategory) << "setup";
}

void Squeezebox::networkAccessibleChanged(QNetworkAccessManager::NetworkAccessibility accessible) {
    if (accessible != QNetworkAccessManager::NetworkAccessibility::Accessible) {
        disconnect();
    }
}

void Squeezebox::connect() {
    setState(CONNECTING);
    _userDisconnect = false;

    qCDebug(m_logCategory) << "Try to connect for the " << QString::number(_connectionTries + 1) << "st/nd time";

    _connectionTimeout.start();
    getPlayers();
}

void Squeezebox::disconnect() {
    _userDisconnect = true;

    _socket.close();
    _mediaProgress.stop();

    setState(DISCONNECTED);
}

void Squeezebox::enterStandby() {
    _mediaProgress.stop();
    _inStandby = true;
}

void Squeezebox::leaveStandby() {
    _inStandby = false;
    for (QMap<QString, SqPlayer>::iterator i = _sqPlayerDatabase.begin(); i != _sqPlayerDatabase.end(); ++i) {
        if (i->isPlaying) {
            getPlayerStatus(i.key());
        }
    }
}

void Squeezebox::onConnectionTimeoutTimer() {
    if (_connectionState == connected) {
        _connectionTries = 0;
        return;
    }

    if (_connectionTries == 3) {
        disconnect();

        qCCritical(m_logCategory) << "Cannot connect to Squeezebox server: retried 3 times connecting to" << _url;
        QObject* param = this;

        m_notifications->add(true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                             [](QObject* param) {
                                 Integration* i = qobject_cast<Integration*>(param);
                                 i->connect();
                             },
                             param);

        _connectionTries = 0;
    } else {
        _connectionTries++;
        connect();
    }
}

QByteArray Squeezebox::buildRpcJson(int id, const QString& player, const QString& command) {
    QJsonArray arr = QJsonArray();
    arr.append(player);
    arr.append(QJsonArray::fromStringList(command.split(" ")));

    QJsonObject json = QJsonObject();
    json.insert("method", "slim.request");
    json.insert("id", id);
    json.insert("params", arr);

    return QJsonDocument(json).toJson();
}

QNetworkRequest Squeezebox::buildRpcRequest() {
    QNetworkRequest request(_httpurl + "jsonrpc.js");
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");

    return request;
}

void Squeezebox::getPlayers() {
    _connectionState = playerInfo;

    QNetworkReply* reply = _nam.post(buildRpcRequest(), buildRpcJson(1, "-", "players 0 99"));
    QObject::connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), this,
                     &Squeezebox::networkError);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);

        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }

        QVariantMap map = doc.toVariant().toMap();
        QVariantMap results = qvariant_cast<QVariantMap>(map.value("result"));

        _playerCnt = results.value("count").toInt();

        QVariantList players = results.value("players_loop").toList();
        while (!players.isEmpty()) {
            auto    player = players.takeFirst().toMap();
            QString playerid = player["playerid"].toString();

            QStringList features({"MEDIA_ALBUM", "MEDIA_ARTIST", "MEDIA_DURATION", "MEDIA_POSITION", "MEDIA_IMAGE",
                                  "MEDIA_TITLE", "MEDIA_TYPE",   "MUTE",           "MUTE_SET",       "NEXT",
                                  "PAUSE",       "PLAY",         "PREVIOUS",       "SEARCH",         "SEEK",
                                  "STOP",        "VOLUME",       "VOLUME_SET",     "VOLUME_UP",      "VOLUME_DOWN"});
            if (player["canpoweroff"].toBool()) {
                features.append({"TURN_OFF", "TURN_ON"});
            }

            addAvailableEntity(playerid, "media_player", integrationId(), player["name"].toString(), features);

            if (_sqPlayerDatabase.contains(playerid)) {
                _sqPlayerDatabase[playerid].connected = true;
                getPlayerStatus(playerid);
            }
        }

        // HERE: suche nicht verbundene player
        qCDebug(m_logCategory) << "Server reported " << _playerCnt << "player/s";

        // connect to socket
        _socket.connectToHost(_url, _port);
    });
}

void Squeezebox::getPlayerStatus(const QString& playerMac) {
    QNetworkReply* reply = _nam.post(buildRpcRequest(), buildRpcJson(1, playerMac, _sqCmdPlayerStatus));
    QObject::connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), this,
                     &Squeezebox::networkError);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);

        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }

        QVariantMap map = doc.toVariant().toMap();
        parsePlayerStatus(playerMac, qvariant_cast<QVariantMap>(map.value("result")));
    });
}

void Squeezebox::sqCommand(const QString& playerMac, const QString& command) {
    QNetworkReply* reply = _nam.post(buildRpcRequest(), buildRpcJson(1, playerMac, command));
    QObject::connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), this,
                     &Squeezebox::networkError);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);

        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }

        QVariantMap map = doc.toVariant().toMap();
        QVariantMap results = qvariant_cast<QVariantMap>(map.value("result"));

        qCDebug(m_logCategory) << "kommando gesendet";
    });
}

void Squeezebox::sendCometd(const QByteArray& message) {
    QByteArray header = "POST /cometd HTTP/1.1\n";
    header += QStringLiteral("Content-Length: %1\n").arg(message.length());
    header += "Content-Type: application/json\n\n";

    _socket.write(header + message + "\n");
}

void Squeezebox::socketConnected() {
    _connectionState = cometdHandshake;

    QJsonArray connectionTypes = QJsonArray();
    connectionTypes.append("long-polling");
    connectionTypes.append("streaming");

    QJsonObject json = QJsonObject();
    json.insert("channel", "/meta/handshake");
    json.insert("supportedConnectionTypes", connectionTypes);
    json.insert("version", "1.0");

    QJsonArray message = QJsonArray();
    message.append(json);

    sendCometd(QJsonDocument(message).toJson());
    qCDebug(m_logCategory) << "connected to socket";
}

void Squeezebox::socketError(QAbstractSocket::SocketError socketError) {
    if (_userDisconnect) {
        return;
    }
    qCCritical(m_logCategory) << "Socket error: " << socketError << " - try to reconnect";
    _connectionState = error;
    if (!_connectionTimeout.isActive()) {
        _connectionTimeout.start();
    }
}

void Squeezebox::networkError(QNetworkReply::NetworkError code) {
    if (_userDisconnect) {
        return;
    }
    qCCritical(m_logCategory) << "HTTP connection error: " << code << " - no reconnect attempt";
}

void Squeezebox::parsePlayerStatus(const QString& playerMac, const QVariantMap& data) {
    EntityInterface* entity = m_entities->getEntityInterface(playerMac);

    QVariantList playlist = data.value("playlist_loop").toList();

    // get current player status
    if (!data.value("power").toBool()) {
        entity->setState(MediaPlayerDef::OFF);
    } else {
        entity->setState(MediaPlayerDef::ON);

        if (data.value("mode").toString() == "play") {
            entity->setState(MediaPlayerDef::PLAYING);
            _sqPlayerDatabase[playerMac].isPlaying = true;
            if (_inStandby == false) {
                _mediaProgress.start();
            }
        } else if (data.value("mode").toString() == "pause" || data.value("mode").toString() == "stop") {
            entity->setState(MediaPlayerDef::IDLE);
            _sqPlayerDatabase[playerMac].isPlaying = false;
        }
    }

    // get track infos
    int         playlistIndex = data.value("playlist_curr_index").toInt();
    QVariantMap playlistItem = qvariant_cast<QVariantMap>(playlist.at(playlistIndex));
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, playlistItem.value("artist").toString());
    entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, playlistItem.value("title").toString());
    if (playlistItem.value("coverart").toBool()) {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE,
                                  _httpurl + "music/" + playlistItem.value("coverid").toString() + "/cover.jpg");
    } else {
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
    }
    int volume = data.value("mixer_volume").toInt();
    if (volume < 0) {
        entity->updateAttrByIndex(MediaPlayerDef::MUTED, true);
    } else {
        entity->updateAttrByIndex(MediaPlayerDef::MUTED, false);
        entity->updateAttrByIndex(MediaPlayerDef::VOLUME, data.value("mixer_volume").toInt());
    }
    entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, data.value("duration").toInt());

    _sqPlayerDatabase[playerMac].position = data.value("time").toDouble();
    entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, _sqPlayerDatabase[playerMac].position);
}

void Squeezebox::onMediaProgressTimer() {
    bool onePlaying = false;
    for (QMap<QString, SqPlayer>::iterator i = _sqPlayerDatabase.begin(); i != _sqPlayerDatabase.end(); ++i) {
        if (i->isPlaying) {
            onePlaying = true;
            i->position += 0.5;

            m_entities->getEntityInterface(i.key())->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, i->position);
        }
    }

    if (onePlaying == false) {
        _mediaProgress.stop();
    }
}

void Squeezebox::socketReceived() {
    QString     answer = _socket.readAll();
    QStringList all = answer.split(QRegExp("[\r\n]"), QString::SkipEmptyParts);

    // check if the answer is a valid http 200 response or if it is a CometD packet
    if (!((all[0].startsWith("HTTP") && all[0].endsWith("200 OK")) || (all.size() == 2))) {
        return;
    }

    QJsonParseError parseerror;
    QByteArray      document = all[all.length() - 1].toUtf8();

    QJsonDocument doc = QJsonDocument::fromJson(document, &parseerror);
    if (parseerror.error != QJsonParseError::NoError) {
        jsonError(parseerror.errorString());
        return;
    }

    QVariant     var = doc.toVariant();
    QVariantList list = var.toList();

    while (!list.isEmpty()) {
        QVariantMap map = list.takeFirst().toMap();

        if (_connectionState == cometdHandshake && map.value("successful").toBool() == true &&
            map.value("channel").toString() == "/meta/handshake") {
            // first step of handshake process; getting client id
            _clientId = map.value("clientId").toString().remove("\"");
            qCInfo(m_logCategory) << "Client ID: " << _clientId;
            _subscriptionChannel = "/slim/" + _clientId + "/status";

            _connectionState = cometdConnect;

            QJsonObject json = QJsonObject();
            json.insert("channel", "/meta/connect");
            json.insert("clientId", _clientId);
            json.insert("connectionType", "streaming");

            QJsonArray message = QJsonArray();
            message.append(json);

            sendCometd(QJsonDocument(message).toJson());
        } else if (_connectionState == cometdConnect && map.value("successful").toBool() == true &&
                   map.value("channel").toString() == "/meta/connect") {
            // now connected
            // subscribe to player messages

            _connectionState = cometdSubscribe;

            for (QMap<QString, SqPlayer>::iterator i = _sqPlayerDatabase.begin(); i != _sqPlayerDatabase.end(); ++i) {
                SqPlayer player = *i;
                if (player.connected && !player.subscribed) {
                    int     rand = qrand();
                    QString command = _sqCmdPlayerStatus + " subscribe:60";

                    QJsonArray request = QJsonArray();
                    request.append(i.key());
                    request.append(QJsonArray::fromStringList(command.split(" ")));

                    QJsonObject data = QJsonObject();
                    data.insert("response", _subscriptionChannel);
                    data.insert("request", request);
                    data.insert("priority", 1);

                    QJsonObject json = QJsonObject();
                    json.insert("channel", "/slim/subscribe");
                    json.insert("clientId", _clientId);
                    json.insert("id", rand);
                    json.insert("data", data);

                    QJsonArray complete = QJsonArray();
                    complete.append(json);

                    QByteArray message = QJsonDocument(complete).toJson();

                    _sqPlayerIdMapping.insert(rand, i.key());
                    sendCometd(message);
                }
            }
        } else if (_connectionState == cometdSubscribe && map.value("successful").toBool() == true &&
                   map.value("channel").toString() == "/slim/subscribe") {
            QString player = _sqPlayerIdMapping.value(map["id"].toInt());
            _sqPlayerDatabase[player].subscribed = true;

            int subscriptions = 0;
            int connected = 0;
            for (auto i : _sqPlayerDatabase) {
                if (i.connected) {
                    connected++;
                }
                if (i.connected && i.subscribed) {
                    subscriptions++;
                }
            }
            if (connected == subscriptions) {
                _connectionState = connectionStates::connected;
                setState(CONNECTED);
            }
        } else if (map.value("channel").toString() == _subscriptionChannel) {
            QString     player = _sqPlayerIdMapping.value(map["id"].toInt());
            QVariantMap data = qvariant_cast<QVariantMap>(map.value("data"));

            parsePlayerStatus(player, data);
        }
    }
}

void Squeezebox::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    if (type != "media_player") {
        qCCritical(m_logCategory) << "Something went completly wrong - command with item type: " << type;
        return;
    }

    if (command == MediaPlayerDef::C_PLAY) {
        sqCommand(entityId, "play");
    } else if (command == MediaPlayerDef::C_PAUSE) {
        sqCommand(entityId, "pause 1");
    } else if (command == MediaPlayerDef::C_STOP) {
        sqCommand(entityId, "stop");
    } else if (command == MediaPlayerDef::C_NEXT) {
        sqCommand(entityId, "playlist jump +1");
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        sqCommand(entityId, "playlist jump -1");
    } else if (command == MediaPlayerDef::C_TURNON) {
        sqCommand(entityId, "power 1");
    } else if (command == MediaPlayerDef::C_TURNOFF) {
        sqCommand(entityId, "power 0");
    } else if (command == MediaPlayerDef::C_MUTE) {
        sqCommand(entityId, "mixer muting 1");
    } else if (command == MediaPlayerDef::C_VOLUME_UP) {
        sqCommand(entityId, "button volume_up");
    } else if (command == MediaPlayerDef::C_VOLUME_DOWN) {
        sqCommand(entityId, "button volume_down");
    } else if (command == MediaPlayerDef::C_VOLUME_SET) {
        sqCommand(entityId, "mixer volume " + param.toString());
    }
}

void Squeezebox::jsonError(const QString& error) { qCWarning(m_logCategory) << "JSON error " << error; }
