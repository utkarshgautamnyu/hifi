//
//  OBJBaker.cpp
//  libraries/baking/src
//
//  Created by Utkarsh Gautam on 9/29/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <PathUtils.h>
#include <NetworkAccessManager.h>
#include <QtConcurrent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <mutex>
#include "OBJBaker.h"
#include "ModelBakingLoggingCategory.h"
#include "TextureBaker.h"

OBJBaker::OBJBaker(const QUrl& objURL, TextureBakerThreadGetter textureThreadGetter,
                   const QString& bakedOutputDir, const QString& originalOutputDir) :
    _objURL(objURL),
    _bakedOutputDir(bakedOutputDir),
    _originalOutputDir(originalOutputDir),
    _textureThreadGetter(textureThreadGetter) {

}

void OBJBaker::abort() {
    Baker::abort();

    // tell our underlying TextureBaker instances to abort
    // the objBaker will wait until all are aborted before emitting its own abort signal
    for (auto& textureBaker : _bakingTextures) {
        textureBaker->abort();
    }
}

void OBJBaker::bake() {
    qDebug() << "OBJBaker" << _objURL << "bake starting";

    auto tempDir = PathUtils::generateTemporaryDir();

    if (tempDir.isEmpty()) {
        handleError("Failed to create a temporary directory.");
        return;
    }

    _tempDir = tempDir;

    _originalOBJFilePath = _tempDir.filePath(_objURL.fileName());
    qDebug() << "Made temporary dir " << _tempDir;
    qDebug() << "Origin file path: " << _originalOBJFilePath;

    // setup the output folder for the results of this bake
    setupOutputFolder();

    if (shouldStop()) {
        return;
    }

    connect(this, &OBJBaker::OBJLoaded, this, &OBJBaker::startBake);

    // make a local copy of the obj file
    loadOBJ();
}

void OBJBaker::setupOutputFolder() {
    // make sure there isn't already an output directory using the same name
    if (QDir(_bakedOutputDir).exists()) {
        qWarning() << "Output path" << _bakedOutputDir << "already exists. Continuing.";
    } else {
        qCDebug(model_baking) << "Creating obj output folder" << _bakedOutputDir;

        // attempt to make the output folder
        if (!QDir().mkpath(_bakedOutputDir)) {
            handleError("Failed to create obj output folder " + _bakedOutputDir);
            return;
        }
        // attempt to make the output folder
        if (!QDir().mkpath(_originalOutputDir)) {
            handleError("Failed to create obj output folder " + _bakedOutputDir);
            return;
        }
    }
}

void OBJBaker::loadOBJ() {
    // check if the obj is local or first needs to be downloaded
    if (_objURL.isLocalFile()) {
        // load up the local file
        QFile localobj{ _objURL.toLocalFile() };

        qDebug() << "Local file url: " << _objURL << _objURL.toString() << _objURL.toLocalFile() << ", copying to: " << _originalOBJFilePath;

        if (!localobj.exists()) {
            //QMessageBox::warning(this, "Could not find " + _objURL.toString(), "");
            handleError("Could not find " + _objURL.toString());
            return;
        }

        // make a copy in the output folder
        if (!_originalOutputDir.isEmpty()) {
            qDebug() << "Copying to: " << _originalOutputDir << "/" << _objURL.fileName();
            localobj.copy(_originalOutputDir + "/" + _objURL.fileName());
        }

        localobj.copy(_originalOBJFilePath);

        // emit our signal to start the import of the obj source copy
        emit OBJLoaded();
    } else {
        // remote file, kick off a download
        auto& networkAccessManager = NetworkAccessManager::getInstance();

        QNetworkRequest networkRequest;

        // setup the request to follow re-directs and always hit the network
        networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        networkRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);

        networkRequest.setUrl(_objURL);

        qCDebug(model_baking) << "Downloading" << _objURL;
        auto networkReply = networkAccessManager.get(networkRequest);

        connect(networkReply, &QNetworkReply::finished, this, &OBJBaker::handleOBJNetworkReply);
    }
}

void OBJBaker::handleOBJNetworkReply() {
    auto requestReply = qobject_cast<QNetworkReply*>(sender());

    if (requestReply->error() == QNetworkReply::NoError) {
        qCDebug(model_baking) << "Downloaded" << _objURL;

        // grab the contents of the reply and make a copy in the output folder
        QFile copyOfOriginal(_originalOBJFilePath);

        qDebug(model_baking) << "Writing copy of original obj to" << _originalOBJFilePath << copyOfOriginal.fileName();

        if (!copyOfOriginal.open(QIODevice::WriteOnly)) {
            // add an error to the error list for this obj stating that a duplicate of the original obj could not be made
            handleError("Could not create copy of " + _objURL.toString() + " (Failed to open " + _originalOBJFilePath + ")");
            return;
        }
        if (copyOfOriginal.write(requestReply->readAll()) == -1) {
            handleError("Could not create copy of " + _objURL.toString() + " (Failed to write)");
            return;
        }

        // close that file now that we are done writing to it
        copyOfOriginal.close();

        if (!_originalOutputDir.isEmpty()) {
            copyOfOriginal.copy(_originalOutputDir + "/" + _objURL.fileName());
        }

        // emit our signal to start the import of the obj source copy
        emit OBJLoaded();
    } else {
        // add an error to our list stating that the obj could not be downloaded
        handleError("Failed to download " + _objURL.toString());
    }
}


void OBJBaker::startBake() {
    qCDebug(model_baking) << "OBJ Baking";

    emit finished();
}