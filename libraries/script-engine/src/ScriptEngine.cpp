//
//  ScriptEngine.cpp
//  hifi
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#include <AvatarData.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <UUID.h>
#include <VoxelConstants.h>
#include <VoxelDetail.h>
#include <ParticlesScriptingInterface.h>

#include <Sound.h>

#include "ScriptEngine.h"

const unsigned int VISUAL_DATA_CALLBACK_USECS = (1.0 / 60.0) * 1000 * 1000;

int ScriptEngine::_scriptNumber = 1;
VoxelsScriptingInterface ScriptEngine::_voxelsScriptingInterface;
ParticlesScriptingInterface ScriptEngine::_particlesScriptingInterface;

static QScriptValue soundConstructor(QScriptContext* context, QScriptEngine* engine) {
    QUrl soundURL = QUrl(context->argument(0).toString());
    QScriptValue soundScriptValue = engine->newQObject(new Sound(soundURL), QScriptEngine::ScriptOwnership);

    return soundScriptValue;
}


ScriptEngine::ScriptEngine(const QString& scriptContents, bool wantMenuItems, const QString& fileNameString,
                           AbstractMenuInterface* menu,
                           AbstractControllerScriptingInterface* controllerScriptingInterface) :
    _isAvatar(false),
    _dataServerScriptingInterface(),
    _avatarData(NULL)
{
    _scriptContents = scriptContents;
    _isFinished = false;
    _isRunning = false;
    _isInitialized = false;
    _fileNameString = fileNameString;

    QByteArray fileNameAscii = fileNameString.toLocal8Bit();
    const char* scriptMenuName = fileNameAscii.data();

    // some clients will use these menu features
    _wantMenuItems = wantMenuItems;
    if (!fileNameString.isEmpty()) {
        _scriptMenuName = "Stop ";
        _scriptMenuName.append(scriptMenuName);
        _scriptMenuName.append(QString(" [%1]").arg(_scriptNumber));
    } else {
        _scriptMenuName = "Stop Script ";
        _scriptMenuName.append(_scriptNumber);
    }
    _scriptNumber++;
    _menu = menu;
    _controllerScriptingInterface = controllerScriptingInterface;
}

ScriptEngine::~ScriptEngine() {
    //printf("ScriptEngine::~ScriptEngine()...\n");
}

void ScriptEngine::setAvatarData(AvatarData* avatarData, const QString& objectName) {
    _avatarData = avatarData;
    
    // remove the old Avatar property, if it exists
    _engine.globalObject().setProperty(objectName, QScriptValue());
    
    // give the script engine the new Avatar script property
    registerGlobalObject(objectName, _avatarData);
}

void ScriptEngine::setupMenuItems() {
    if (_menu && _wantMenuItems) {
        _menu->addActionToQMenuAndActionHash(_menu->getActiveScriptsMenu(), _scriptMenuName, 0, this, SLOT(stop()));
    }
}

void ScriptEngine::cleanMenuItems() {
    if (_menu && _wantMenuItems) {
        _menu->removeAction(_menu->getActiveScriptsMenu(), _scriptMenuName);
    }
}

bool ScriptEngine::setScriptContents(const QString& scriptContents) {
    if (_isRunning) {
        return false;
    }
    _scriptContents = scriptContents;
    return true;
}

Q_SCRIPT_DECLARE_QMETAOBJECT(AudioInjectorOptions, QObject*)
Q_SCRIPT_DECLARE_QMETAOBJECT(QTimer, QObject*)

void ScriptEngine::init() {
    if (_isInitialized) {
        return; // only initialize once
    }

    _isInitialized = true;

    _voxelsScriptingInterface.init();
    _particlesScriptingInterface.init();

    // register various meta-types
    registerMetaTypes(&_engine);
    registerVoxelMetaTypes(&_engine);
    //registerParticleMetaTypes(&_engine);
    registerEventTypes(&_engine);
    
    qScriptRegisterMetaType(&_engine, ParticlePropertiesToScriptValue, ParticlePropertiesFromScriptValue);
    qScriptRegisterMetaType(&_engine, ParticleIDtoScriptValue, ParticleIDfromScriptValue);
    qScriptRegisterSequenceMetaType<QVector<ParticleID> >(&_engine);

    QScriptValue soundConstructorValue = _engine.newFunction(soundConstructor);
    QScriptValue soundMetaObject = _engine.newQMetaObject(&Sound::staticMetaObject, soundConstructorValue);
    _engine.globalObject().setProperty("Sound", soundMetaObject);

    QScriptValue injectionOptionValue = _engine.scriptValueFromQMetaObject<AudioInjectorOptions>();
    _engine.globalObject().setProperty("AudioInjectionOptions", injectionOptionValue);
    
    QScriptValue timerValue = _engine.scriptValueFromQMetaObject<QTimer>();
    _engine.globalObject().setProperty("Timer", timerValue);

    registerGlobalObject("Script", this);
    registerGlobalObject("Audio", &_audioScriptingInterface);
    registerGlobalObject("Controller", _controllerScriptingInterface);
    registerGlobalObject("Data", &_dataServerScriptingInterface);
    registerGlobalObject("Particles", &_particlesScriptingInterface);
    registerGlobalObject("Quat", &_quatLibrary);

    registerGlobalObject("Voxels", &_voxelsScriptingInterface);

    QScriptValue treeScaleValue = _engine.newVariant(QVariant(TREE_SCALE));
    _engine.globalObject().setProperty("TREE_SCALE", treeScaleValue);

    // let the VoxelPacketSender know how frequently we plan to call it
    _voxelsScriptingInterface.getVoxelPacketSender()->setProcessCallIntervalHint(VISUAL_DATA_CALLBACK_USECS);
    _particlesScriptingInterface.getParticlePacketSender()->setProcessCallIntervalHint(VISUAL_DATA_CALLBACK_USECS);

    //qDebug() << "Script:\n" << _scriptContents << "\n";
}

void ScriptEngine::registerGlobalObject(const QString& name, QObject* object) {
    if (object) {
        QScriptValue value = _engine.newQObject(object);
        _engine.globalObject().setProperty(name, value);
    }
}

void ScriptEngine::evaluate() {
    if (!_isInitialized) {
        init();
    }

    QScriptValue result = _engine.evaluate(_scriptContents);

    if (_engine.hasUncaughtException()) {
        int line = _engine.uncaughtExceptionLineNumber();
        qDebug() << "Uncaught exception at line" << line << ":" << result.toString();
    }
}

void ScriptEngine::run() {
    if (!_isInitialized) {
        init();
    }
    _isRunning = true;

    QScriptValue result = _engine.evaluate(_scriptContents);
    if (_engine.hasUncaughtException()) {
        int line = _engine.uncaughtExceptionLineNumber();
        qDebug() << "Uncaught exception at line" << line << ":" << result.toString();
    }

    timeval startTime;
    gettimeofday(&startTime, NULL);

    int thisFrame = 0;
    
    NodeList* nodeList = NodeList::getInstance();

    while (!_isFinished) {
        int usecToSleep = usecTimestamp(&startTime) + (thisFrame++ * VISUAL_DATA_CALLBACK_USECS) - usecTimestampNow();
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        }

        if (_isFinished) {
            break;
        }

        QCoreApplication::processEvents();

        if (_isFinished) {
            break;
        }

        bool willSendVisualDataCallBack = false;
        if (_voxelsScriptingInterface.getVoxelPacketSender()->serversExist()) {
            // allow the scripter's call back to setup visual data
            willSendVisualDataCallBack = true;

            // release the queue of edit voxel messages.
            _voxelsScriptingInterface.getVoxelPacketSender()->releaseQueuedMessages();

            // since we're in non-threaded mode, call process so that the packets are sent
            if (!_voxelsScriptingInterface.getVoxelPacketSender()->isThreaded()) {
                _voxelsScriptingInterface.getVoxelPacketSender()->process();
            }
        }

        if (_particlesScriptingInterface.getParticlePacketSender()->serversExist()) {
            // allow the scripter's call back to setup visual data
            willSendVisualDataCallBack = true;

            // release the queue of edit voxel messages.
            _particlesScriptingInterface.getParticlePacketSender()->releaseQueuedMessages();

            // since we're in non-threaded mode, call process so that the packets are sent
            if (!_particlesScriptingInterface.getParticlePacketSender()->isThreaded()) {
                _particlesScriptingInterface.getParticlePacketSender()->process();
            }
        }
        
        if (_isAvatar && _avatarData) {
            static QByteArray avatarPacket;
            int numAvatarHeaderBytes = 0;
            
            if (avatarPacket.size() == 0) {
                // pack the avatar header bytes the first time
                // unlike the _avatar.getBroadcastData these won't change
                numAvatarHeaderBytes = populatePacketHeader(avatarPacket, PacketTypeAvatarData);
            }
            
            avatarPacket.resize(numAvatarHeaderBytes);
            avatarPacket.append(_avatarData->toByteArray());
            
            nodeList->broadcastToNodes(avatarPacket, NodeSet() << NodeType::AvatarMixer);
        }

        if (willSendVisualDataCallBack) {
            emit willSendVisualDataCallback();
        }

        if (_engine.hasUncaughtException()) {
            int line = _engine.uncaughtExceptionLineNumber();
            qDebug() << "Uncaught exception at line" << line << ":" << _engine.uncaughtException().toString();
        }
    }
    emit scriptEnding();
    
    if (_voxelsScriptingInterface.getVoxelPacketSender()->serversExist()) {
        // release the queue of edit voxel messages.
        _voxelsScriptingInterface.getVoxelPacketSender()->releaseQueuedMessages();

        // since we're in non-threaded mode, call process so that the packets are sent
        if (!_voxelsScriptingInterface.getVoxelPacketSender()->isThreaded()) {
            _voxelsScriptingInterface.getVoxelPacketSender()->process();
        }
    }

    if (_particlesScriptingInterface.getParticlePacketSender()->serversExist()) {
        // release the queue of edit voxel messages.
        _particlesScriptingInterface.getParticlePacketSender()->releaseQueuedMessages();

        // since we're in non-threaded mode, call process so that the packets are sent
        if (!_particlesScriptingInterface.getParticlePacketSender()->isThreaded()) {
            _particlesScriptingInterface.getParticlePacketSender()->process();
        }
    }
    
    cleanMenuItems();

    // If we were on a thread, then wait till it's done
    if (thread()) {
        thread()->quit();
    }

    emit finished(_fileNameString);
    
    _isRunning = false;
}

void ScriptEngine::stop() {
    _isFinished = true;
}


