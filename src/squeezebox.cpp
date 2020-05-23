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

    _jsonrpc = "http://" + _url + ":" + QString::number(_port) + "/jsonrpc.js";

    connectionState = idle;

    // read added entities
    _myEntities = m_entities->getByIntegration(integrationId());
    for (auto entity : _myEntities) {
        _sqPlayerDatabase.insert(entity->entity_id(), SqPlayer(false));
    }

    QObject::connect(&_socket, &QTcpSocket::connected, this, &Squeezebox::socketConnected);
    QObject::connect(&_socket, &QIODevice::readyRead, this, &Squeezebox::socketReceived);

    qCDebug(m_logCategory) << "setup";
}

void Squeezebox::connect() {
    setState(CONNECTING);

    getPlayers();
}

void Squeezebox::disconnect() { setState(DISCONNECTED); }

void Squeezebox::enterStandby() {}

void Squeezebox::leaveStandby() {}

QByteArray Squeezebox::buildRpcJson(int id, QString player, QString command) {
    QJsonArray arr = QJsonArray();
    arr.append(player);
    arr.append(QJsonArray::fromStringList(command.split(" ")));

    QJsonObject json = QJsonObject();
    json.insert("method", "slim.request");
    json.insert("id", id);
    json.insert("params", arr);

    return QJsonDocument(json).toJson();
}

QNetworkRequest Squeezebox::buildRpcRequest(){
    QNetworkRequest request(_jsonrpc);
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");

    return request;
}

void Squeezebox::getPlayers() {
    connectionState = playerInfo;

    QNetworkReply* reply = _nam.post(buildRpcRequest(), buildRpcJson(1, "-", "players 0 99"));
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
            auto player = players.takeFirst().toMap();
            QString playerid = player["playerid"].toString();
            QStringList features("VOLUME");

            addAvailableEntity(playerid, "media_player", integrationId(), player["name"].toString(), features);

            if (_sqPlayerDatabase.contains(playerid)) {
                _sqPlayerDatabase[playerid].connected = true;
            }
        }

        // HERE: suche nicht verbundene player
        qCDebug(m_logCategory) << "Server reported " << _playerCnt << "player/s";

        // connect to socket
        _socket.connectToHost(_url, _port);
    });
}


void Squeezebox::sendCometd(QByteArray* message) {
    QByteArray header = "POST /cometd HTTP/1.1\n";
    header += QStringLiteral("Content-Length: %1\n").arg(message->length());
    header += "Content-Type: application/json\n\n";

    // QByteArray sender(header.length() + message->length(), Qt::Initialization::Uninitialized);
    // sender.append(header);
    // sender.append(*message);

    QByteArray sender = header;
    sender.append(*message);

    _socket.write(header + *message + "\n");
}

void Squeezebox::socketConnected() {
    connectionState = cometdHandshake;
    QByteArray message =
        "[{\"channel\":\"/meta/"
        "handshake\",\"supportedConnectionTypes\":[\"long-polling\",\"streaming\"],\"version\":\"1.0\"}]";
    sendCometd(&message);
    qCInfo(m_logCategory) << "connected to socket";
}

void Squeezebox::socketReceived() {
    QString     answer = _socket.readAll();
    QStringList all = answer.split(QRegExp("[\r\n]"), QString::SkipEmptyParts);

    // check if the answer is a valid http 200 response or if it is a CometD packet
    if (! ((all[0].startsWith("HTTP") && all[0].endsWith("200 OK")) || (all.size() == 2))) {
        return;
    }

    QJsonParseError parseerror;
    QByteArray      document = all[all.length() - 1].toUtf8();
    qCInfo(m_logCategory) << "Answer: " << document;

    QJsonDocument doc = QJsonDocument::fromJson(document, &parseerror);
    if (parseerror.error != QJsonParseError::NoError) {
        jsonError(parseerror.errorString());
        return;
    }

    QVariant     var = doc.toVariant();
    QVariantList list = var.toList();

    while (!list.isEmpty()) {
        QVariantMap map = list.takeFirst().toMap();

        if (connectionState == cometdHandshake && map.value("successful").toBool() == true && map.value("channel").toString() == "/meta/handshake") {
            // first step of handshake process; getting client id
            _clientId = map.value("clientId").toString().remove("\"");
            qCInfo(m_logCategory) << "Client ID: " << _clientId;
            _subscriptionChannel = "/slim/" + _clientId + "/status";

            connectionState = cometdConnect;

            QByteArray message = "[{\"channel\":\"/meta/connect\",\"clientId\":\"" + _clientId.toUtf8() +
                                 "\",\"connectionType\":\"streaming\"}]";
            sendCometd(&message);
        } else if( connectionState == cometdConnect && map.value("successful").toBool() == true && map.value("channel").toString() == "/meta/connect") {
            // now connected
            // subscribe to player messages

            connectionState = cometdSubscribe;

            for (QMap<QString, SqPlayer>::iterator i = _sqPlayerDatabase.begin(); i != _sqPlayerDatabase.end(); ++i) {
                SqPlayer player = *i;
                if (player.connected && !player.subscribed) {
                    int rand = qrand();
                    //QString command = "status 0 999 tags:acdj subscribe:60";
                    QString command = "status - 1 tags:aBdgKlNotuxyY subscribe:60";

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
                    sendCometd(&message);
                }
            }
        } else if (connectionState == cometdSubscribe && map.value("successful").toBool() == true && map.value("channel").toString() == "/slim/subscribe") {
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
                connectionState = connectionStates::connected;
            }
        } else if (map.value("channel").toString() == _subscriptionChannel) {
            QString player = _sqPlayerIdMapping.value(map["id"].toInt());
        }
    }
}

void Squeezebox::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {}

void Squeezebox::jsonError(const QString& error) { qCWarning(m_logCategory) << "JSON error " << error; }
