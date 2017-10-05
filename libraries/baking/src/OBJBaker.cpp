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
#include "OBJReader.h"
#include "ModelBaker.h"
#include "FBXWriter.h"

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

    connect(this, &OBJBaker::OBJLoaded, this, &OBJBaker::startBake);

    // make a local copy of the obj file
    loadOBJ();
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
    // Read the OBJ
    QFile objFile(_originalOBJFilePath);
    if (!objFile.open(QIODevice::ReadOnly)) {
        handleError("Error opening " + _originalOBJFilePath + " for reading");
        return;
    }

    QByteArray objData = objFile.readAll();

    bool combineParts = false;
    OBJReader reader;
    FBXGeometry* geometry = reader.readOBJ(objData, QVariantHash(), combineParts);
    
    
    
    FBXNode objRoot;
    FBXNode dracoNode;

    dracoNode = ModelBaker::compressMesh(geometry->meshes[0]);
    
    createFBXNodeTree(&objRoot, &dracoNode);

    auto encodedFBX = FBXWriter::encodeFBX(objRoot);

    qCDebug(model_baking) << "encoded FBX" << encodedFBX;
    auto fileName = _objURL.fileName();
    auto baseName = fileName.left(fileName.lastIndexOf('.'));
    auto bakedFilename = baseName + ".baked.fbx";

    _bakedOBJFilePath = _bakedOutputDir + "/" + bakedFilename;

    QFile bakedFile;
    bakedFile.setFileName(_bakedOBJFilePath);
    if (!bakedFile.open(QIODevice::WriteOnly)) {
        handleError("Error opening " + _bakedOBJFilePath + " for writing");
        return;
    }

    bakedFile.write(encodedFBX);

    // Export successful
    _outputFiles.push_back(_bakedOBJFilePath);
    qCDebug(model_baking) << "Exported" << _objURL << "with re-written paths to" << _bakedOBJFilePath;
    
    emit finished();

}

void OBJBaker::createFBXNodeTree(FBXNode* objRoot, FBXNode* dracoNode) {
    
    FBXNode header;
    FBXNode FileId;
    FBXNode creationTime;
    FBXNode creator;
    FBXNode globalSettings;
    FBXNode documents;
    FBXNode references;
    FBXNode definitions;
    FBXNode objectNode;
    FBXNode connections;
    FBXNode takes;

    header.name = "FBXHeaderExtension";
    FileId.name = "FileId";
    creationTime.name = "CreationTime";
    creator.name = "Creator";
    globalSettings.name = "GlobalSettings";
    documents.name = "Documents";
    references.name = "References";
    definitions.name = "Definitions";
    objectNode.name = "Objects";
    connections.name = "Connections";
    takes.name = "Takes";

    QByteArray property0 = "UnitScaleFactor";
    auto prop0 = QVariant::fromValue(QByteArray(property0.data(), (int)property0.size()));
    QByteArray property1 = "double";
    auto prop1 = QVariant::fromValue(QByteArray(property1.data(), (int)property1.size()));
    QByteArray property2 = "Number";
    auto prop2 = QVariant::fromValue(QByteArray(property2.data(), (int)property2.size()));
    QByteArray property3 = "";
    auto prop3 = QVariant::fromValue(QByteArray(property3.data(), (int)property3.size()));
    double d = 1;
    QVariant prop4 = d;
    
    FBXNode p;
    p.name = "P";
    p.properties = {prop0, prop1, prop2, prop3, prop4};

    FBXNode properties70;
    properties70.name = "Properties70";
    properties70.children = { p };

    globalSettings.children = { properties70 };

    FBXNode geometryNode;
    geometryNode.name = "Geometry";
    QVariant val = 837754154LL;
    QByteArray buffer = "Cube  Geometry";
    auto value = QVariant::fromValue(QByteArray(buffer.data(), (int)buffer.size()));
    QByteArray buffer1 = "Mesh";
    auto value1 = QVariant::fromValue(QByteArray(buffer1.data(), (int)buffer1.size()));
    geometryNode.properties = { val, value, value1 };

    FBXNode modelNode;
    modelNode.name = "Model";
    QVariant val2 = 579498249LL;
    QByteArray buffer2 = "Cube  Model";
    auto value2 = QVariant::fromValue(QByteArray(buffer2.data(), (int)buffer2.size()));
    QByteArray buffer3 = "Mesh";
    auto value3 = QVariant::fromValue(QByteArray(buffer3.data(), (int)buffer3.size()));
   modelNode.properties = { val2, value2, value3 };

    FBXNode materialNode;
    materialNode.name = "Material";
    QVariant val3 = 51114185LL;
    QByteArray buffer4 = "Material  Material";
    auto value4 = QVariant::fromValue(QByteArray(buffer4.data(), (int)buffer4.size()));
    QByteArray buffer5 = "Mesh";
    auto value5 = QVariant::fromValue(QByteArray(buffer5.data(), (int)buffer5.size()));
    materialNode.properties = { val3, value4, value5 };
    

    //objRoot.children = { header,FileId, creationTime, creator, globalSettings, documents, references, definitions, objectNode, connections, takes };
       
    objRoot->children = { globalSettings, objectNode};
    objRoot->children[1].children = { geometryNode, modelNode, materialNode };
    objRoot->children[1].children[0].children = { *dracoNode };
    


}
