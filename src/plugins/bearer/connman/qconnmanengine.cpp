/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qconnmanengine.h"
#include "qconnmanservice_linux_p.h"
#include "../qnetworksession_impl.h"

#include <QtNetwork/private/qnetworkconfiguration_p.h>

#include <QtNetwork/qnetworksession.h>

#include <QtCore/qdebug.h>

#include <QtDBus/QtDBus>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#ifndef QT_NO_DBUS

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(qLcLibBearer, "qt.qpa.bearer.connman", QtWarningMsg)

QConnmanEngine::QConnmanEngine(QObject *parent)
:   QBearerEngineImpl(parent),
    connmanManager(0),
    ofonoManager(0),
    ofonoNetwork(0),
    ofonoContextManager(0),
    connmanAvailable(false)

{
    qDBusRegisterMetaType<ConnmanMap>();
    qDBusRegisterMetaType<ConnmanMapList>();
    qRegisterMetaType<ConnmanMapList>("ConnmanMapList");

    connmanWatcher = new QDBusServiceWatcher(QLatin1String(CONNMAN_SERVICE), QDBusConnection::systemBus(),
            QDBusServiceWatcher::WatchForRegistration |
            QDBusServiceWatcher::WatchForUnregistration, this);
    connect(connmanWatcher, SIGNAL(serviceRegistered(QString)),
            this, SLOT(connmanRegistered(QString)));
    connect(connmanWatcher, SIGNAL(serviceUnregistered(QString)),
            this, SLOT(connmanUnRegistered(QString)));

    ofonoWatcher = new QDBusServiceWatcher(QLatin1String(OFONO_SERVICE), QDBusConnection::systemBus(),
            QDBusServiceWatcher::WatchForRegistration |
            QDBusServiceWatcher::WatchForUnregistration, this);
    connect(ofonoWatcher, SIGNAL(serviceRegistered(QString)),
            this, SLOT(ofonoRegistered(QString)));
    connect(ofonoWatcher, SIGNAL(serviceUnregistered(QString)),
            this, SLOT(ofonoUnRegistered(QString)));

    QDBusConnectionInterface *interface = QDBusConnection::systemBus().interface();
    if (!interface) {
        qCCritical(qLcLibBearer) << "QConnmanEngine: something is badly wrong, no system bus interface.";
        return;
    }

    if (interface->isServiceRegistered(QLatin1String(OFONO_SERVICE))) {
        QMetaObject::invokeMethod(this, "ofonoRegistered", Qt::QueuedConnection, Q_ARG(QString, OFONO_SERVICE));
    } else {
        qCDebug(qLcLibBearer) << "QConnmanEngine:" << OFONO_SERVICE << "dbus service is not registered";
    }

    if (interface->isServiceRegistered(QLatin1String(CONNMAN_SERVICE))) {
        QMetaObject::invokeMethod(this, "connmanRegistered", Qt::QueuedConnection, Q_ARG(QString, CONNMAN_SERVICE));
    } else {
        qCDebug(qLcLibBearer) << "QConnmanEngine:" << CONNMAN_SERVICE << "dbus service is not registered";
    }
}

QConnmanEngine::~QConnmanEngine()
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: destroyed";
}

void QConnmanEngine::initialize()
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: initialize";
    if (connmanAvailable) {
        setupConfigurations();
    }
}

void QConnmanEngine::changedModem()
{
    QMutexLocker locker(&mutex);
    if (ofonoNetwork)
        delete ofonoNetwork;

    ofonoNetwork = new QOfonoNetworkRegistrationInterface(ofonoManager->currentModem(),this);

    if (ofonoContextManager)
        delete ofonoContextManager;
    ofonoContextManager = new QOfonoDataConnectionManagerInterface(ofonoManager->currentModem(),this);

    if (connmanManager) {
        refreshConfigurations();
    }
}

void QConnmanEngine::servicesReady(const QStringList &list)
{
    QMutexLocker locker(&mutex);
    foreach (const QString &servPath, list) {
        addServiceConfiguration(servPath);
    }

    Q_EMIT updateCompleted();
}

QList<QNetworkConfigurationPrivate *> QConnmanEngine::getConfigurations()
{
    QMutexLocker locker(&mutex);
    QList<QNetworkConfigurationPrivate *> fetchedConfigurations;
    QNetworkConfigurationPrivate* cpPriv = 0;
    const int numFoundConfigurations = foundConfigurations.count();
    fetchedConfigurations.reserve(numFoundConfigurations);

    for (int i = 0; i < numFoundConfigurations; ++i) {
        QNetworkConfigurationPrivate *config = new QNetworkConfigurationPrivate;
        cpPriv = foundConfigurations.at(i);

        config->name = cpPriv->name;
        config->isValid = cpPriv->isValid;
        config->id = cpPriv->id;
        config->state = cpPriv->state;
        config->type = cpPriv->type;
        config->roamingSupported = cpPriv->roamingSupported;
        config->purpose = cpPriv->purpose;
        config->bearerType = cpPriv->bearerType;

        fetchedConfigurations.append(config);
        delete config;
    }
    return fetchedConfigurations;
}

QString QConnmanEngine::getInterfaceFromId(const QString &id)
{
    QMutexLocker locker(&mutex);
    return configInterfaces.value(id);
}

bool QConnmanEngine::hasIdentifier(const QString &id)
{
    QMutexLocker locker(&mutex);
    return accessPointConfigurations.contains(id);
}

void QConnmanEngine::connectToId(const QString &id)
{
    QMutexLocker locker(&mutex);

    QConnmanServiceInterface *serv = connmanServiceInterfaces.value(id);

    if (!serv || !serv->isValid()) {
        emit connectionError(id, QBearerEngineImpl::InterfaceLookupError);
    } else {
        if (serv->type() == QLatin1String("cellular")) {
            if (serv->roaming()) {
                if (!isRoamingAllowed(serv->path())) {
                    emit connectionError(id, QBearerEngineImpl::OperationNotSupported);
                    return;
                }
            }
        }
        if (serv->autoConnect())
            serv->connect();
    }
}

void QConnmanEngine::disconnectFromId(const QString &id)
{
    QMutexLocker locker(&mutex);
    QConnmanServiceInterface *serv = connmanServiceInterfaces.value(id);

    if (!serv || !serv->isValid()) {
        emit connectionError(id, DisconnectionError);
    } else {
        serv->disconnect();
    }
}

void QConnmanEngine::requestUpdate()
{
    QMutexLocker locker(&mutex);
    QTimer::singleShot(0, this, SLOT(doRequestUpdate()));
}

void QConnmanEngine::doRequestUpdate()
{
    if (!connmanManager || !connmanManager->requestScan("wifi"))
        Q_EMIT updateCompleted();
}

void QConnmanEngine::finishedScan(bool error)
{
    if (error)
        Q_EMIT updateCompleted();
}

void QConnmanEngine::updateServices(const ConnmanMapList &changed, const QList<QDBusObjectPath> &removed)
{
    QMutexLocker locker(&mutex);

    foreach (const QDBusObjectPath &objectPath, removed) {
        removeConfiguration(objectPath.path());
    }

    foreach (const ConnmanMap &connmanMap, changed) {
        const QString id = connmanMap.objectPath.path();
        if (accessPointConfigurations.contains(id)) {
            configurationChange(connmanServiceInterfaces.value(id));
        } else {
            addServiceConfiguration(connmanMap.objectPath.path());
        }
    }
    Q_EMIT updateCompleted();
}

QNetworkSession::State QConnmanEngine::sessionStateForId(const QString &id)
{
    QMutexLocker locker(&mutex);

    QNetworkConfigurationPrivatePointer ptr = accessPointConfigurations.value(id);

    if (!ptr || !ptr->isValid)
        return QNetworkSession::Invalid;

    QConnmanServiceInterface *serv = connmanServiceInterfaces.value(id);
    if (!serv)
        return QNetworkSession::Invalid;

    QString servState = serv->state();

    if (servState == QLatin1String("idle") ||
        servState == QLatin1String("failure") ||
        servState == QLatin1String("disconnect")) {
        return QNetworkSession::Disconnected;
    }

    if (servState == QLatin1String("association") || servState == QLatin1String("configuration")) {
        return QNetworkSession::Connecting;
    }

    if (servState == QLatin1String("online") || servState == QLatin1String("ready")) {
        return QNetworkSession::Connected;
    }

    if ((ptr->state & QNetworkConfiguration::Discovered) ==
                QNetworkConfiguration::Discovered) {
        return QNetworkSession::Disconnected;
    } else if ((ptr->state & QNetworkConfiguration::Defined) == QNetworkConfiguration::Defined) {
        return QNetworkSession::NotAvailable;
    } else if ((ptr->state & QNetworkConfiguration::Undefined) ==
                QNetworkConfiguration::Undefined) {
        return QNetworkSession::NotAvailable;
    }

    return QNetworkSession::Invalid;
}

quint64 QConnmanEngine::bytesWritten(const QString &id)
{//TODO use connman counter API
    QMutexLocker locker(&mutex);
    quint64 result = 0;
    QString devFile = getInterfaceFromId(id);
    QFile tx("/sys/class/net/"+devFile+"/statistics/tx_bytes");
    if (tx.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&tx);
        in >> result;
        tx.close();
    }

    return result;
}

quint64 QConnmanEngine::bytesReceived(const QString &id)
{//TODO use connman counter API
    QMutexLocker locker(&mutex);
    quint64 result = 0;
    QString devFile = getInterfaceFromId(id);
    QFile rx("/sys/class/net/"+devFile+"/statistics/rx_bytes");
    if (rx.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&rx);
        in >> result;
        rx.close();
    }
    return result;
}

quint64 QConnmanEngine::startTime(const QString &/*id*/)
{
    // TODO
    QMutexLocker locker(&mutex);
    if (activeTime.isNull()) {
        return 0;
    }
    return activeTime.secsTo(QDateTime::currentDateTime());
}

QNetworkConfigurationManager::Capabilities QConnmanEngine::capabilities() const
{
    return QNetworkConfigurationManager::ForcedRoaming |
            QNetworkConfigurationManager::DataStatistics |
            QNetworkConfigurationManager::CanStartAndStopInterfaces |
            QNetworkConfigurationManager::NetworkSessionRequired;
}

QNetworkSessionPrivate *QConnmanEngine::createSessionBackend()
{
     return new QNetworkSessionPrivateImpl;
}

QNetworkConfigurationPrivatePointer QConnmanEngine::defaultConfiguration()
{
    const QMutexLocker locker(&mutex);
    if (connmanManager) {
        Q_FOREACH (const QString &servPath, connmanManager->getServices()) {
            if (connmanServiceInterfaces.contains(servPath)) {
                if (accessPointConfigurations.contains(servPath))
                    return accessPointConfigurations.value(servPath);
            }
        }
    }
    return QNetworkConfigurationPrivatePointer();
}

void QConnmanEngine::serviceStateChanged(const QString &state)
{
    QConnmanServiceInterface *service = qobject_cast<QConnmanServiceInterface *>(sender());
    configurationChange(service);

    if (state == QLatin1String("failure")) {
        emit connectionError(service->path(), ConnectError);
    }
}

void QConnmanEngine::configurationChange(QConnmanServiceInterface *serv)
{
    if (!serv)
        return;
    QMutexLocker locker(&mutex);
    QString id = serv->path();

    if (accessPointConfigurations.contains(id)) {
        bool changed = false;
        QNetworkConfigurationPrivatePointer ptr = accessPointConfigurations.value(id);
        QString networkName = serv->name();
        QNetworkConfiguration::StateFlags curState = getStateForService(serv->path());
        ptr->mutex.lock();

        if (!ptr->isValid) {
            ptr->isValid = true;
        }

        if (ptr->name != networkName) {
            ptr->name = networkName;
            changed = true;
        }

        if (ptr->state != curState) {
            ptr->state = curState;
            changed = true;
        }

        ptr->mutex.unlock();

        if (!changed) {
            const QNetworkSession::State curSessionState = sessionStateForId(id);
            const QNetworkSession::State prevSessionState =
                connmanLastKnownSessionState.contains(id) ?
                    connmanLastKnownSessionState.value(id) :
                    QNetworkSession::Invalid;
            if (curSessionState != prevSessionState) {
                if (curSessionState == QNetworkSession::Invalid) {
                    connmanLastKnownSessionState.remove(id);
                } else {
                    connmanLastKnownSessionState.insert(id, curSessionState);
                }
                changed = true;
            }
        }

        if (changed) {
            locker.unlock();
            emit configurationChanged(ptr);
            locker.relock();
        }
    }

     locker.unlock();
     emit updateCompleted();
}

QNetworkConfiguration::StateFlags QConnmanEngine::getStateForService(const QString &service)
{
    QMutexLocker locker(&mutex);
    QConnmanServiceInterface *serv = connmanServiceInterfaces.value(service);
    if (!serv)
        return QNetworkConfiguration::Undefined;

    QString state = serv->state();
    QNetworkConfiguration::StateFlags flag = QNetworkConfiguration::Defined;

    if (serv->type() == QLatin1String("cellular")) {

        if (!serv->autoConnect()|| (serv->roaming()
                    && !isRoamingAllowed(serv->path()))) {
            flag = (flag | QNetworkConfiguration::Defined);
        } else {
            flag = (flag | QNetworkConfiguration::Discovered);
        }
    } else {
        if (serv->favorite()) {
            if (serv->autoConnect()) {
                flag = (flag | QNetworkConfiguration::Discovered);
            }
        } else {
            flag = QNetworkConfiguration::Undefined;
        }
    }
    if (state == QLatin1String("online") || state == QLatin1String("ready")) {
        flag = (flag | QNetworkConfiguration::Active);
    }

    return flag;
}

QNetworkConfiguration::BearerType QConnmanEngine::typeToBearer(const QString &type)
{
    if (type == QLatin1String("wifi"))
        return QNetworkConfiguration::BearerWLAN;
    if (type == QLatin1String("ethernet"))
        return QNetworkConfiguration::BearerEthernet;
    if (type == QLatin1String("bluetooth"))
        return QNetworkConfiguration::BearerBluetooth;
    if (type == QLatin1String("cellular")) {
        return ofonoTechToBearerType(type);
    }
    if (type == QLatin1String("wimax"))
        return QNetworkConfiguration::BearerWiMAX;

    return QNetworkConfiguration::BearerUnknown;
}

QNetworkConfiguration::BearerType QConnmanEngine::ofonoTechToBearerType(const QString &/*type*/)
{
    if (ofonoNetwork) {
        QString currentTechnology = ofonoNetwork->getTechnology();
        if (currentTechnology == QLatin1String("gsm")) {
            return QNetworkConfiguration::Bearer2G;
        } else if (currentTechnology == QLatin1String("edge")) {
            return QNetworkConfiguration::BearerCDMA2000; //wrong, I know
        } else if (currentTechnology == QLatin1String("umts")) {
            return QNetworkConfiguration::BearerWCDMA;
        } else if (currentTechnology == QLatin1String("hspa")) {
            return QNetworkConfiguration::BearerHSPA;
        } else if (currentTechnology == QLatin1String("lte")) {
            return QNetworkConfiguration::BearerLTE;
        } else {
            qCWarning(qLcLibBearer) << "QConnmanEngine: Unable to translate the bearer type of the unknown network technology:" << currentTechnology;
        }
    } else {
        qCWarning(qLcLibBearer) << "QConnmanEngine: Attempted to query the bearer type of a cellular connection but Ofono isn't available";
    }

    // If the actual type is unknown return something that still identifies it as a cellular connection.
    return QNetworkConfiguration::Bearer2G;
}

bool QConnmanEngine::isRoamingAllowed(const QString &context)
{
    if (ofonoContextManager) {
        foreach (const QString &dcPath, ofonoContextManager->contexts()) {
            if (dcPath.contains(context.section("_",-1))) {
                return ofonoContextManager->roamingAllowed();
            }
        }
    }
    return false;
}

void QConnmanEngine::removeConfiguration(const QString &id)
{
    QMutexLocker locker(&mutex);

    if (accessPointConfigurations.contains(id)) {

        disconnect(connmanServiceInterfaces.value(id),SIGNAL(stateChanged(QString)),
                this,SLOT(serviceStateChanged(QString)));
        serviceNetworks.removeOne(id);
        QConnmanServiceInterface *service  = connmanServiceInterfaces.take(id);
        delete service;
        QNetworkConfigurationPrivatePointer ptr = accessPointConfigurations.take(id);
        foundConfigurations.removeOne(ptr.data());
        locker.unlock();
        emit configurationRemoved(ptr);
        locker.relock();
    }
}

void QConnmanEngine::addServiceConfiguration(const QString &servicePath)
{
    QMutexLocker locker(&mutex);
    if (!connmanServiceInterfaces.contains(servicePath)) {
        QConnmanServiceInterface *serv = new QConnmanServiceInterface(servicePath, this);
        connmanServiceInterfaces.insert(serv->path(),serv);
    }

    if (!accessPointConfigurations.contains(servicePath)) {
        QConnmanServiceInterface *service = connmanServiceInterfaces.value(servicePath);

        const QString connectionType = service->type();

        // ofonoNetwork is queried to identify the specific bearer type of a cellular connection,
        // and if it's not available at this time that type will be unresolvable so skip the
        // connection for now. When ofonoNetwork does become available a new attempt at adding
        // the service configuration will be made.
        if (!ofonoNetwork && connectionType == QLatin1String("cellular")) {
            qCWarning(qLcLibBearer) << "QConnmanEngine: Deferring a cellular service configuration because ofonoNetwork is unavailable";
            return;
        }

        serviceNetworks.append(servicePath);

        connect(connmanServiceInterfaces.value(servicePath),SIGNAL(stateChanged(QString)),
                this,SLOT(serviceStateChanged(QString)));

        QNetworkConfigurationPrivate* cpPriv = new QNetworkConfigurationPrivate();

        QString networkName = service->name();

        if (connectionType == QLatin1String("ethernet")) {
            cpPriv->bearerType = QNetworkConfiguration::BearerEthernet;
        } else if (connectionType == QLatin1String("wifi")) {
            cpPriv->bearerType = QNetworkConfiguration::BearerWLAN;
        } else if (connectionType == QLatin1String("cellular")) {
            cpPriv->bearerType = ofonoTechToBearerType(QLatin1String("cellular"));
            cpPriv->roamingSupported = service->roaming() && isRoamingAllowed(servicePath);
        } else if (connectionType == QLatin1String("wimax")) {
            cpPriv->bearerType = QNetworkConfiguration::BearerWiMAX;
        } else {
            if (connectionType != QLatin1String("vpn")) { // we know this exists but can't map it. warn for others
                qCWarning(qLcLibBearer) << "QConnmanEngine: Unable to translate the bearer type of the unknown connection type:" << connectionType;
            }
            cpPriv->bearerType = QNetworkConfiguration::BearerUnknown;
        }

        cpPriv->name = networkName;
        cpPriv->isValid = true;
        cpPriv->id = servicePath;
        cpPriv->type = QNetworkConfiguration::InternetAccessPoint;

        if (service->security() == QLatin1String("none")) {
            cpPriv->purpose = QNetworkConfiguration::PublicPurpose;
        } else {
            cpPriv->purpose = QNetworkConfiguration::PrivatePurpose;
        }

        cpPriv->state = getStateForService(servicePath);

        QNetworkConfigurationPrivatePointer ptr(cpPriv);
        accessPointConfigurations.insert(ptr->id, ptr);
        if (connectionType == QLatin1String("cellular")) {
            foundConfigurations.append(cpPriv);
        } else {
            foundConfigurations.prepend(cpPriv);
        }
        configInterfaces[cpPriv->id] = service->serviceInterface();

        locker.unlock();
        Q_EMIT configurationAdded(ptr);
        locker.relock();
    }
}

void QConnmanEngine::setupConfigurations()
{
    QMutexLocker locker(&mutex);
    qCDebug(qLcLibBearer) << "QConnmanEngine: setup connman configurations";

    if (connmanManager) {
        delete connmanManager;
    }

    connmanManager = new QConnmanManagerInterface(this);
    connect(connmanManager,SIGNAL(servicesChanged(ConnmanMapList,QList<QDBusObjectPath>)),
            this, SLOT(updateServices(ConnmanMapList,QList<QDBusObjectPath>)));

    connect(connmanManager,SIGNAL(servicesReady(QStringList)),this,SLOT(servicesReady(QStringList)));
    connect(connmanManager,SIGNAL(scanFinished(bool)),this,SLOT(finishedScan(bool)));

    refreshConfigurations();
}

void QConnmanEngine::refreshConfigurations()
{
    foreach (const QString &servPath, connmanManager->getServices()) {
        addServiceConfiguration(servPath);
    }

    Q_EMIT updateCompleted();
}

bool QConnmanEngine::requiresPolling() const
{
    return false;
}

void QConnmanEngine::reEvaluateCellular()
{
    if (connmanManager) {
        Q_FOREACH (const QString &servicePath, connmanManager->getServices()) {
            if (servicePath.contains("cellular") && accessPointConfigurations.contains(servicePath)) {
                configurationChange(connmanServiceInterfaces.value(servicePath));
            }
        }
    }
}

void QConnmanEngine::connmanRegistered(const QString &serviceName)
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: connman dbus service registered:" << serviceName;
    connmanAvailable = true;
    setupConfigurations();
}

void QConnmanEngine::connmanUnRegistered(const QString &serviceName)
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: connman dbus service unregistered:" << serviceName;

    qDeleteAll(connmanServiceInterfaces);
    connmanServiceInterfaces.clear();

    // Remove all configurations.
    QList<QString> keys = accessPointConfigurations.uniqueKeys();
    for (const QString &key : keys) {
        removeConfiguration(key);
    }

    serviceNetworks.clear();
    connmanLastKnownSessionState.clear();
    configInterfaces.clear();

    delete connmanManager;
    connmanManager = nullptr;
    connmanAvailable = false;
}

void QConnmanEngine::ofonoRegistered(const QString &serviceName)
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: ofono dbus service registered:" << serviceName;
    if (ofonoManager) {
        delete ofonoManager;
    }

    if (ofonoNetwork) {
        delete ofonoNetwork;
    }

    if (ofonoContextManager) {
        delete ofonoContextManager;
    }

    ofonoManager = new QOfonoManagerInterface(this);
    ofonoNetwork = new QOfonoNetworkRegistrationInterface(ofonoManager->currentModem(),this);
    ofonoContextManager = new QOfonoDataConnectionManagerInterface(ofonoManager->currentModem(),this);

    connect(ofonoManager,SIGNAL(modemChanged()),this,SLOT(changedModem()));
    connect(ofonoContextManager,SIGNAL(roamingAllowedChanged(bool)),this,SLOT(reEvaluateCellular()));

    QMutexLocker locker(&mutex);
    if (connmanManager && !connmanManager->getServices().isEmpty()) {
        refreshConfigurations();
    }
}

void QConnmanEngine::ofonoUnRegistered(const QString &serviceName)
{
    qCDebug(qLcLibBearer) << "QConnmanEngine: ofono dbus service unregistered:" << serviceName;
    delete ofonoManager;
    ofonoManager = nullptr;

    delete ofonoNetwork;
    ofonoNetwork = nullptr;

    delete ofonoContextManager;
    ofonoContextManager = nullptr;

    // Remove all cellular configurations.
    QList<QString> keys = accessPointConfigurations.uniqueKeys();
    Q_FOREACH (const QString &key, keys) {
        if (key.startsWith(QLatin1String("/net/connman/service/cellular"))) {
            removeConfiguration(key);
        }
    }
}

QT_END_NAMESPACE

#endif // QT_NO_DBUS
